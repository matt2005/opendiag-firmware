#!/usr/bin/env python3
"""Commissioning run for a RejsaCAN v3.4 (or stock) board on the bench. No vehicle.

Steps:

  1. Identity reads on the console (version, ctrl ID, mode, heap, link). These are
     read-only. The SLCAN serial is read with N, after `mode slcan`, and the front
     end is restored to elm327 before the suite runs. `can` and pin commands are
     never sent.
  2. The bench suite (pytest, test/bench) runs in a subprocess with the ports
     passed through the environment, and writes JUnit XML.
  3. A Markdown report and the raw JUnit XML are written to --out-dir.

PASS means every check ran and passed on the detected build. A skipped check is
not a pass: it is counted separately, and any skip makes the run FAIL.

Exit codes:
  0  PASS: every check passed, with none failed, errored or skipped
  1  not a PASS: a check failed or errored, a check was skipped, or the run did not finish
  2  no ports configured (neither argument nor environment): nothing is written

Ports come from --console-port and --data-port, or from OPENDIAG_CONSOLE_PORT and
OPENDIAG_DATA_PORT. They are never hard-coded.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

import bench_identity as ident
from conftest import CONSOLE_ENV, DATA_ENV, SETTLE_S, Console, SlcanPort, open_port

TITLE = "RejsaCAN v3.4 hardware commissioning report"
BENCH_DIR = Path(__file__).resolve().parent
DEFAULT_OUT = BENCH_DIR / "reports"

EXIT_PASS = 0
EXIT_NOT_PASS = 1
EXIT_NO_PORTS = 2

OUTCOME_LABEL = {"passed": "PASS", "failed": "FAIL", "error": "ERROR", "skipped": "SKIPPED"}
JUNIT_OUTCOME = {"failure": "failed", "error": "error", "skipped": "skipped"}


# ---------------------------------------------------------------- data

@dataclass
class TestResult:
    name: str
    outcome: str  # passed, failed, error or skipped
    duration: float
    message: str = ""


@dataclass
class Identity:
    version: ident.VersionInfo = field(default_factory=ident.VersionInfo)
    board: str = ""  # hardware name from `ctrl ID`, e.g. "RejsaCAN v3.4"
    mode: str = ""  # front end from `mode`, e.g. "elm327"
    default_mode: str = ""
    modes: List[str] = field(default_factory=list)
    link: str = ""  # transport from `link`, e.g. "usb"
    free_heap: Optional[int] = None
    slcan_serial: Optional[str] = None  # `N` reply, e.g. "N2208"
    board_mac: Optional[str] = None  # USB serial = eFuse base MAC, e.g. "34:85:18:9c:9f:78"
    notes: List[str] = field(default_factory=list)


@dataclass
class Totals:
    passed: int = 0
    failed: int = 0
    errors: int = 0
    skipped: int = 0

    @property
    def total(self) -> int:
        return self.passed + self.failed + self.errors + self.skipped


# ---------------------------------------------------------------- identity reads

def read_identity(console_port: str, data_port: str, notes: List[str]) -> Tuple[Identity, bool]:
    """Read the console identity, then the SLCAN serial. Returns (identity, restored).

    `restored` is False when `mode elm327` did not reply, which stops the run:
    the suite needs the elm327 front end.
    """
    info = Identity()
    ser = open_port(console_port)
    try:
        console = Console(ser)
        info.version = ident.parse_version(console.command("version"))
        if info.version.build == "unknown":
            notes.append("version banner not recognised; build-specific checks will skip")

        ctrl = ident.parse_ctrl_id(console.command("ctrl ID"))
        if ctrl:
            info.board = ctrl.board
        else:
            notes.append("ctrl ID did not reply in the expected form; board type not read")

        mode = ident.parse_mode(console.command("mode"))
        info.mode = str(mode["link"])
        info.default_mode = str(mode["default"])
        info.modes = list(mode["modes"])  # type: ignore[arg-type]

        info.free_heap = ident.parse_heap(console.command("heap"))
        info.link = ident.parse_link(console.command("link")) or ""

        if data_port:
            info.slcan_serial, restored = read_slcan_serial(console, data_port, notes)
            if not restored:
                return info, False
        else:
            notes.append("data port not set: SLCAN N not read")
    finally:
        ser.close()
    return info, True


def read_board_mac(console_port: str, data_port: str, notes: List[str]) -> Optional[str]:
    """Read the board MAC from the USB serial string the firmware exposes.

    The firmware writes its eFuse base MAC into the USB serial string, so the
    port descriptor carries it. This reads the descriptor only and opens no port.
    """
    from serial.tools import list_ports

    def serial_of(device: str) -> Optional[str]:
        for info in list_ports.comports():
            if info.device.upper() == device.upper():
                return info.serial_number
        return None

    mac = ident.parse_usb_serial_mac(serial_of(console_port))
    if mac is None:
        notes.append(f"board MAC not read: no 12-digit USB serial on {console_port}")
        return None
    if data_port:
        other = ident.parse_usb_serial_mac(serial_of(data_port))
        if other != mac:
            notes.append(f"USB serial of {data_port} ({other}) differs from {console_port} ({mac})")
    return mac


def read_slcan_serial(console: Console, data_port: str, notes: List[str]) -> Tuple[Optional[str], bool]:
    """Switch to SLCAN, send N on the data port, and switch back to elm327.

    N opens no channel, so there is nothing to close with C. The restore runs in
    a finally block, so the front end is put back even if the read fails.
    Returns (serial reply or None, restored).
    """
    serial_id: Optional[str] = None
    try:
        console.set_front_end("slcan")
        time.sleep(SETTLE_S)
        try:
            ser = open_port(data_port)
        except Exception as exc:  # pyserial raises SerialException or OSError
            notes.append(f"SLCAN N not read: data port is not free ({exc})")
        else:
            try:
                serial_id = ident.parse_slcan_serial(SlcanPort(ser).send("N", timeout=2.0))
            finally:
                ser.close()
            if serial_id is None:
                notes.append("SLCAN N was refused or did not reply")
    except AssertionError as exc:
        notes.append(f"SLCAN N not read: {exc}")
    finally:
        restored_reply = console.command("mode elm327")
    restored = "MODE elm327" in restored_reply
    if not restored:
        notes.append(f"front end not restored to elm327: {restored_reply!r}")
    return serial_id, restored


# ---------------------------------------------------------------- pytest run

def pytest_command(junit_path: Path) -> List[str]:
    return [sys.executable, "-m", "pytest", "-v", "-p", "no:cacheprovider", f"--junitxml={junit_path}"]


def command_line(console_port: str, data_port: str, junit_path: Path) -> str:
    """The command that repeats this run, with the ports as environment variables."""
    parts = [f"{CONSOLE_ENV}={console_port or ''}", f"{DATA_ENV}={data_port or ''}"]
    parts += ["python", "-m", "pytest", "-v", "-p", "no:cacheprovider", f"--junitxml={junit_path.name}"]
    return " ".join(parts)


def run_suite(console_port: str, data_port: str, junit_path: Path) -> int:
    """Run the bench suite; its output streams to this terminal. Returns the exit code."""
    env = dict(os.environ)
    env[CONSOLE_ENV] = console_port
    env[DATA_ENV] = data_port
    proc = subprocess.run(pytest_command(junit_path), cwd=str(BENCH_DIR), env=env, timeout=1800)
    return proc.returncode


def parse_junit(path: Path) -> List[TestResult]:
    """One TestResult per <testcase>. The failure or skip reason is trimmed to a short line."""
    root = ET.parse(path).getroot()
    results: List[TestResult] = []
    for case in root.iter("testcase"):
        classname = case.get("classname", "")
        name = case.get("name", "")
        full = f"{classname}::{name}" if classname else name
        outcome, message = "passed", ""
        for child in case:
            if child.tag in JUNIT_OUTCOME:
                outcome = JUNIT_OUTCOME[child.tag]
                message = child.get("message") or (child.text or "")
                break
        results.append(TestResult(full, outcome, float(case.get("time") or 0), trim_message(message)))
    return results


def trim_message(text: str, lines: int = 3, limit: int = 240) -> str:
    """The first few non-blank lines of a failure or skip message, on one line."""
    kept = [line.strip() for line in text.splitlines() if line.strip()][:lines]
    joined = " / ".join(kept)
    return joined if len(joined) <= limit else joined[: limit - 3] + "..."


def totals_of(results: Sequence[TestResult]) -> Totals:
    totals = Totals()
    for result in results:
        if result.outcome == "passed":
            totals.passed += 1
        elif result.outcome == "failed":
            totals.failed += 1
        elif result.outcome == "error":
            totals.errors += 1
        else:
            totals.skipped += 1
    return totals


# ---------------------------------------------------------------- report

def md_cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def verdict_of(identity: Identity, results: Sequence[TestResult], totals: Totals,
               not_run: Optional[str]) -> List[str]:
    """Reasons the run is not a PASS. An empty list means PASS."""
    reasons: List[str] = []
    if not_run:
        reasons.append(not_run)
    if totals.failed or totals.errors:
        reasons.append(f"{totals.failed} failed, {totals.errors} errored")
    if totals.skipped:
        reasons.append(f"{totals.skipped} skipped (a skip is not a pass)")
    if not results or totals.passed == 0:
        reasons.append("no check passed")
    if identity.version.build == "unknown":
        reasons.append("build not recognised from the version banner")
    return reasons


def render_report(stamp: dt.datetime, identity: Identity, console_port: str, data_port: str,
                  command: str, operator: str, unit_label: str,
                  results: Sequence[TestResult], not_run: Optional[str]) -> str:
    totals = totals_of(results)
    reasons = verdict_of(identity, results, totals, not_run)
    verdict = "FAIL" if reasons else "PASS"
    v = identity.version
    serial_note = "fixed firmware string, not unique per board"

    out: List[str] = [f"# {TITLE}", ""]
    out += ["## Run", "", "| Item | Value |", "|---|---|"]
    out.append(f"| Run date and time (UTC) | {stamp:%Y-%m-%d %H:%M:%S} UTC |")
    out.append(f"| Operator | {md_cell(operator)} |")
    out.append(f"| Unit label | {md_cell(unit_label)} |")
    out.append(f"| Console (DEBUG) port | {md_cell(console_port) or 'not used'} |")
    out.append(f"| Protocol (COM) port | {md_cell(data_port) or 'not used'} |")
    out.append(f"| Python | `{md_cell(sys.executable)}` |")
    out.append(f"| pytest command | `{md_cell(command)}` |")
    out.append("")

    out += ["## Device", "", "| Item | Value |", "|---|---|"]
    out.append(f"| Firmware version (banner, git describe) | {md_cell(v.firmware) or 'not read'} |")
    out.append(f"| Banner | {md_cell(v.banner) or 'not read'} |")
    out.append(f"| Build detected | {md_cell(v.build)} |")
    out.append(f"| Board type (`ctrl ID`) | {md_cell(identity.board) or 'not read'} |")
    out.append(f"| Hardware name (banner) | {md_cell(v.hardware) or 'not read'} |")
    out.append(f"| ESP-IDF | {md_cell(v.idf) or 'not read'} |")
    up = f"{v.uptime_s} s" if v.uptime_s is not None else "not read"
    out.append(f"| Uptime at identity read | {up} |")
    out.append(f"| Reset reason | {md_cell(v.reset) or 'not read'} |")
    heap = f"{identity.free_heap} bytes" if identity.free_heap is not None else "not read"
    out.append(f"| Free heap | {heap} |")
    out.append(f"| Link owner (`link`) | {md_cell(identity.link) or 'not read'} |")
    front = f"{identity.mode} (default {identity.default_mode})" if identity.mode else "not read"
    out.append(f"| Front end (`mode`) | {md_cell(front)} |")
    out.append(f"| Modes | {md_cell(', '.join(identity.modes)) or 'not read'} |")
    mac = identity.board_mac or "not read"
    out.append(f"| Board MAC (USB serial, eFuse base MAC) | {md_cell(mac)} |")
    serial = f"{identity.slcan_serial} ({serial_note})" if identity.slcan_serial else "not read"
    out.append(f"| SLCAN serial (`N`) | {md_cell(serial)} |")
    out.append("")
    if identity.notes:
        out.append("Notes from the identity reads:")
        out.append("")
        out += [f"- {md_cell(note)}" for note in identity.notes]
        out.append("")

    out += ["## Results", ""]
    if results:
        out += ["| Test | Result | Duration (s) | Notes |", "|---|---|---|---|"]
        for result in results:
            label = OUTCOME_LABEL.get(result.outcome, result.outcome.upper())
            out.append(f"| `{md_cell(result.name)}` | {label} | {result.duration:.2f} | "
                       f"{md_cell(result.message)} |")
    else:
        out.append("No test results. " + (not_run or "The suite produced no JUnit results."))
    out.append("")

    out += ["## Totals", "", "| Passed | Failed | Errors | Skipped |", "|---|---|---|---|"]
    out.append(f"| {totals.passed} | {totals.failed} | {totals.errors} | {totals.skipped} |")
    out.append("")
    out.append("A skipped check is not a pass. It is counted here and makes the run FAIL.")
    out.append("")

    out += ["## Overall", ""]
    out.append(f"**Overall: {verdict}** ({totals.passed} passed, {totals.failed} failed, "
               f"{totals.errors} errors, {totals.skipped} skipped)")
    out.append("")
    if reasons:
        out.append("Not a PASS because:")
        out.append("")
        out += [f"- {md_cell(reason)}" for reason in reasons]
        out.append("")

    out += ["## Not verified", ""]
    out.append("This run does not cover the following. They need a vehicle or a separate bench setup.")
    out.append("")
    out += [
        "- Real bus traffic. No vehicle is connected and no CAN frame is asserted.",
        "- The RS pin level and any connector pin drive (`hsset`, `lsset`, `pinoff`, K-Line and J1850 payloads).",
        "- Battery sense accuracy. On RejsaCAN, `vbatt` reads -1 mV by design.",
        "- Flashing and the update path (see docs/rejsacan-flashing.md).",
        "- Stock build path: untested on hardware. No stock reply has been captured, so every "
        "build-gated check skips on stock.",
        "- The SLCAN serial. The `N` reply is a fixed firmware string, so it does not identify one board. "
        "The board MAC is the per-chip identifier.",
    ]
    skipped = [r for r in results if r.outcome == "skipped"]
    for result in skipped:
        out.append(f"- Skipped: `{md_cell(result.name)}`. {md_cell(result.message)}")
    out.append("")

    out += ["## Sign-off", "", "| Field | Entry |", "|---|---|"]
    out.append("| Operator signature | |")
    out.append("| Date | |")
    out.append("")
    return "\n".join(out)


def report_base(stamp: dt.datetime, serial_id: Optional[str]) -> str:
    safe = re.sub(r"[^A-Za-z0-9]", "", serial_id or "")[:16] or "nosn"
    return f"commissioning-{stamp:%Y%m%dT%H%M%SZ}-{safe}"


# ---------------------------------------------------------------- main

def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=TITLE)
    parser.add_argument("--console-port", help=f"DEBUG (console) port; default ${CONSOLE_ENV}")
    parser.add_argument("--data-port", help=f"COM (protocol) port; default ${DATA_ENV}")
    parser.add_argument("--out-dir", default=str(DEFAULT_OUT),
                        help="folder for the report and JUnit XML (default: test/bench/reports)")
    parser.add_argument("--operator", default="", help="operator name, written to the report")
    parser.add_argument("--unit-label", default="", help="unit or bench number, written to the report")
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    console_port = (args.console_port or os.environ.get(CONSOLE_ENV, "")).strip()
    data_port = (args.data_port or os.environ.get(DATA_ENV, "")).strip()
    if not console_port and not data_port:
        print(f"commission: no ports configured. Pass --console-port and --data-port, "
              f"or set {CONSOLE_ENV} and {DATA_ENV}. Nothing was written.", file=sys.stderr)
        return EXIT_NO_PORTS

    stamp = dt.datetime.now(dt.timezone.utc)
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    notes: List[str] = []
    identity = Identity()
    not_run: Optional[str] = None
    if console_port:
        try:
            identity, restored = read_identity(console_port, data_port, notes)
            if not restored:
                not_run = "front end not restored to elm327; the suite was not run"
        except Exception as exc:
            not_run = f"identity read failed: {exc}"
    else:
        notes.append("console port not set: identity reads skipped and SLCAN N not read")

    if console_port:
        identity.board_mac = read_board_mac(console_port, data_port, notes)
    base = report_base(stamp, identity.board_mac or identity.slcan_serial)
    junit_path = out_dir / f"{base}.junit.xml"
    md_path = out_dir / f"{base}.md"
    command = command_line(console_port, data_port, Path(junit_path.name))

    results: List[TestResult] = []
    if not_run is None:
        print(f"Running the bench suite: {command}")
        try:
            code = run_suite(console_port, data_port, junit_path)
        except subprocess.TimeoutExpired:
            code = None
            not_run = "the suite did not finish within 30 minutes"
        if not_run is None:
            if not junit_path.exists():
                not_run = f"pytest wrote no JUnit XML (exit code {code})"
            else:
                try:
                    results = parse_junit(junit_path)
                except ET.ParseError as exc:
                    not_run = f"could not parse the JUnit XML: {exc}"

    md_path.write_text(render_report(stamp, identity, console_port, data_port, command,
                                     args.operator, args.unit_label, results, not_run),
                       encoding="utf-8", newline="\n")
    totals = totals_of(results)
    reasons = verdict_of(identity, results, totals, not_run)
    print(f"Report: {md_path}")
    if junit_path.exists():
        print(f"JUnit XML: {junit_path}")
    print(f"Overall: {'FAIL' if reasons else 'PASS'} ({totals.passed} passed, {totals.failed} failed, "
          f"{totals.errors} errors, {totals.skipped} skipped)")
    return EXIT_NOT_PASS if reasons else EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
