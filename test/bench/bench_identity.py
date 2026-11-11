"""Parse the console and SLCAN identity replies on the bench. No port access here.

Shared by conftest.py (build detection for the suite) and commission.py (the
report). Kept free of I/O so it can be checked without a board.

The build comes from the hardware name in the `version` banner:

  OpenDiag v... (RejsaCAN v3.4)   RejsaCAN build (OPENDIAG_BOARD_REJSACAN)
  OpenDiag v... (rev1)            stock build (the upstream hardware name)

Anything else is "unknown". Tests that depend on the build must not guess
the other build's replies.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict, Optional

BUILD_BY_HARDWARE: Dict[str, str] = {
    "RejsaCAN v3.4": "RejsaCAN",
    "rev1": "stock",
}

_BANNER = re.compile(r"^OpenDiag\s+(\S+)\s+\(([^)]*)\)\s*$", re.MULTILINE)
_IDF = re.compile(r"^\s*idf\s*:\s*(\S+)", re.MULTILINE)
_UPTIME = re.compile(r"^\s*uptime\s*:\s*(\d+)\s*s\b", re.MULTILINE)
_RESET = re.compile(r"^\s*reset\s*:\s*(.+?)\s*$", re.MULTILINE)
_CTRL_ID = re.compile(
    r"^OpenDiag\s+(.+?)\s+fw=(\S+)\s+link=(\S+)\s+mode=(\S+)\s+modes=(\S+)",
    re.MULTILINE,
)
_MODE = re.compile(r"Link:\s*(\S+)\s*\(default\s+(\S+)\)")
_MODES = re.compile(r"^Modes:\s*(.+?)\s*$", re.MULTILINE)
_HEAP = re.compile(r"Free Heap Size:\s*(\d+)\s*bytes")
_LINK = re.compile(r"^link:\s*(\S+)", re.MULTILINE)


@dataclass
class VersionInfo:
    banner: str = ""
    firmware: str = ""  # the git describe string (ESP-IDF project version)
    hardware: str = ""  # "RejsaCAN v3.4" or "rev1"
    build: str = "unknown"  # "RejsaCAN", "stock" or "unknown"
    idf: str = ""
    uptime_s: Optional[int] = None
    reset: str = ""


def build_for_hardware(hardware: str) -> str:
    return BUILD_BY_HARDWARE.get(hardware, "unknown")


def parse_version(text: str) -> VersionInfo:
    """Parse the `version` reply. Missing lines leave their fields empty."""
    info = VersionInfo()
    match = _BANNER.search(text)
    if match:
        info.banner = match.group(0).strip()
        info.firmware = match.group(1)
        info.hardware = match.group(2).strip()
        info.build = build_for_hardware(info.hardware)
    idf = _IDF.search(text)
    if idf:
        info.idf = idf.group(1)
    uptime = _UPTIME.search(text)
    if uptime:
        info.uptime_s = int(uptime.group(1))
    reset = _RESET.search(text)
    if reset:
        info.reset = reset.group(1)
    return info


@dataclass
class CtrlId:
    board: str  # e.g. "RejsaCAN v3.4"
    firmware: str
    link: str
    mode: str
    modes: str


def parse_ctrl_id(text: str) -> Optional[CtrlId]:
    """Parse `ctrl ID`: 'OpenDiag <board> fw=<ver> link=<owner> mode=<fe> modes=<list>'."""
    match = _CTRL_ID.search(text)
    if not match:
        return None
    return CtrlId(*match.groups())


def parse_mode(text: str) -> Dict[str, object]:
    """Parse the `mode` reply: current link front end, its default, and the list."""
    result: Dict[str, object] = {"link": "", "default": "", "modes": []}
    match = _MODE.search(text)
    if match:
        result["link"], result["default"] = match.group(1), match.group(2)
    modes = _MODES.search(text)
    if modes:
        result["modes"] = modes.group(1).split()
    return result


def parse_heap(text: str) -> Optional[int]:
    match = _HEAP.search(text)
    return int(match.group(1)) if match else None


def parse_link(text: str) -> Optional[str]:
    """Parse the `link` reply: the transport that holds the link (usb, ble or none)."""
    match = _LINK.search(text)
    return match.group(1) if match else None


def parse_slcan_serial(raw: bytes) -> Optional[str]:
    """Parse an SLCAN `N` reply such as b'N2208\\r'. None when refused (BELL) or absent.

    The firmware answers a fixed string, so this is the value the front end
    reports, not a number that is unique to one board.
    """
    text = raw.decode("ascii", "replace").strip("\r\n\x07 ")
    if not text.startswith("N") or len(text) < 2:
        return None
    return text


def parse_usb_serial_mac(text: Optional[str]) -> Optional[str]:
    """Turn the USB serial string into a MAC, e.g. '3485189C9F78' -> '34:85:18:9c:9f:78'.

    The firmware writes its eFuse base MAC into the USB serial string
    (main/usb_cdc.c). That value is unique per chip, so it identifies one board.
    None when the string is absent or is not twelve hex digits.
    """
    if not text:
        return None
    digits = text.strip()
    if not re.fullmatch(r"[0-9A-Fa-f]{12}", digits):
        return None
    return ":".join(digits[i:i + 2] for i in range(0, 12, 2)).lower()
