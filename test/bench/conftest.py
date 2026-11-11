"""Shared fixtures for the RejsaCAN v3.4 bench tests (board on USB, no vehicle).

Ports come from the environment, never from the code:

  OPENDIAG_CONSOLE_PORT   the DEBUG (console) port
  OPENDIAG_DATA_PORT      the COM (protocol) port

A fixture whose variable is unset skips every test that needs that port.
Each port is opened by one fixture per test and closed in its teardown, so
no two handles ever hold the same port. Ports are opened with DTR and RTS set
to True before the open, so opening does not pulse them into a reset.

The front end is switched with the console `mode` command. The `elm327_restored`
fixture puts it back to elm327 in teardown, even when the test failed.
"""

from __future__ import annotations

import os
import re
import time
from dataclasses import dataclass, field
from typing import List, Tuple

import pytest

try:
    import serial
except ImportError:  # reported as a failure only when a port is configured
    serial = None

BAUD = 115200  # irrelevant on USB CDC; kept for pyserial
CONSOLE_ENV = "OPENDIAG_CONSOLE_PORT"
DATA_ENV = "OPENDIAG_DATA_PORT"
SETTLE_S = 0.3  # wait after each open before the first command

_ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
_LOG_LINE = re.compile(r"^[IWE] \(")  # ESP-IDF log lines: "I (", "W (", "E ("
_LINE_SPLIT = re.compile(r"\r\n|\r|\n")


# ---------------------------------------------------------------- port names

def _configured_port(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        pytest.skip(f"{name} is not set; see bench/README.md")
    return value


@pytest.fixture(scope="session")
def console_port() -> str:
    return _configured_port(CONSOLE_ENV)


@pytest.fixture(scope="session")
def data_port() -> str:
    return _configured_port(DATA_ENV)


# ---------------------------------------------------------------- open/close

def open_port(port_name: str):
    """Open a pyserial port with DTR and RTS True, after the port name is set."""
    if serial is None:
        pytest.fail("pyserial is not installed: pip install pyserial")
    ser = serial.Serial(None, baudrate=BAUD, timeout=0.05, write_timeout=2)
    try:
        ser.port = port_name
        ser.dtr = True
        ser.rts = True
        ser.open()
    except Exception:
        ser.close()
        raise
    time.sleep(SETTLE_S)
    ser.reset_input_buffer()
    return ser


def _read_quiet(ser, quiet: float, timeout: float) -> bytes:
    """Read until the line has been idle for `quiet` seconds, or `timeout` passes."""
    buf = bytearray()
    start = last = time.monotonic()
    while True:
        now = time.monotonic()
        if now - start >= timeout:
            break
        chunk = ser.read(max(1, ser.in_waiting))
        if chunk:
            buf += chunk
            last = time.monotonic()
        elif buf and now - last >= quiet:
            break
    return bytes(buf)


def _read_until(ser, stops: Tuple[bytes, ...], timeout: float) -> bytes:
    """Read until any of `stops` is in the buffer, or `timeout` passes."""
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = ser.read(max(1, ser.in_waiting))
        if chunk:
            buf += chunk
            if any(stop in buf for stop in stops):
                break
    return bytes(buf)


# ---------------------------------------------------------------- console

def console_text(raw: bytes) -> str:
    """Console reply as text: ANSI codes and firmware log lines removed, blanks dropped."""
    text = _ANSI.sub("", raw.decode("utf-8", "replace"))
    lines = [line.strip() for line in _LINE_SPLIT.split(text)]
    return "\n".join(line for line in lines if line and not _LOG_LINE.match(line))


class Console:
    """Commands on the DEBUG (console) port."""

    def __init__(self, ser) -> None:
        self._ser = ser

    def command(self, text: str, timeout: float = 5.0, quiet: float = 0.6) -> str:
        self._ser.reset_input_buffer()
        self._ser.write(text.encode("ascii") + b"\r")
        return console_text(_read_quiet(self._ser, quiet, timeout))

    def set_front_end(self, name: str) -> None:
        reply = self.command(f"mode {name}")
        if f"MODE {name}" not in reply:
            raise AssertionError(f"`mode {name}` did not reply 'MODE {name}': {reply!r}")


@pytest.fixture
def console(console_port):
    ser = open_port(console_port)
    try:
        yield Console(ser)
    finally:
        ser.close()


@pytest.fixture
def elm327_restored(console):
    """Yield, then put the front end back to elm327 whatever the test did."""
    yield
    console.set_front_end("elm327")


# ---------------------------------------------------------------- ELM327 data port

@dataclass
class ElmReply:
    raw: bytes
    lines: List[str] = field(default_factory=list)

    @property
    def prompt_seen(self) -> bool:
        return b">" in self.raw


def _elm_lines(raw: bytes, sent: str) -> List[str]:
    """Reply lines with the '>' prompt, the command echo and blank lines removed."""
    text = raw.decode("ascii", "replace").replace(">", "")
    lines = [line.strip() for line in re.split(r"[\r\n]+", text)]
    lines = [line for line in lines if line]
    if lines and lines[0].replace(" ", "").upper() == sent.replace(" ", "").upper():
        lines = lines[1:]
    return lines


class ElmPort:
    """Commands on the COM (protocol) port while the front end is ELM327."""

    def __init__(self, ser) -> None:
        self._ser = ser

    def at(self, text: str, timeout: float = 5.0) -> ElmReply:
        self._ser.reset_input_buffer()
        self._ser.write(text.encode("ascii") + b"\r")
        raw = _read_until(self._ser, (b">",), timeout)
        return ElmReply(raw=raw, lines=_elm_lines(raw, text))


@pytest.fixture
def elm_port(data_port):
    ser = open_port(data_port)
    try:
        yield ElmPort(ser)
    finally:
        ser.close()


# ---------------------------------------------------------------- SLCAN data port

class SlcanPort:
    """Commands on the COM (protocol) port while the front end is SLCAN."""

    def __init__(self, ser) -> None:
        self._ser = ser

    def send(self, command: str, timeout: float = 3.0) -> bytes:
        """Send one command; return the reply up to and including CR or BELL."""
        self._ser.reset_input_buffer()
        self._ser.write(command.encode("ascii") + b"\r")
        return _read_until(self._ser, (b"\r", b"\x07"), timeout)


@pytest.fixture
def slcan_port(console, data_port, elm327_restored):
    """Switch to SLCAN, open the data port, and close the channel and port after.

    Teardown order: this fixture first (sends C, closes the data port), then
    elm327_restored (mode elm327), then console (closes the console port).
    """
    console.set_front_end("slcan")
    time.sleep(SETTLE_S)  # let the front end switch before the data port opens
    ser = open_port(data_port)
    try:
        yield SlcanPort(ser)
    finally:
        try:
            SlcanPort(ser).send("C", timeout=1.0)  # close any open channel
        finally:
            ser.close()
