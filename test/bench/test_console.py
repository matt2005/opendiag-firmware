"""Console (DEBUG port) checks on the RejsaCAN v3.4 board. No bus, no vehicle.

Firmware log lines ("I (", "W (", "E (") are filtered out by the console fixture.
"""

import re

import pytest


def test_version_banner(console):
    text = console.command("version")
    match = re.search(r"OpenDiag\s+(\S+)\s+\(RejsaCAN v3\.4\)", text)
    assert match, f"no 'OpenDiag ... (RejsaCAN v3.4)' banner in reply: {text!r}"
    banner = match.group(0)
    assert "RejsaCAN v3.4" in banner and "OpenDiag" in banner
    assert match.group(1).startswith("v"), f"version string does not start with 'v': {text!r}"
    assert re.search(r"^idf\s*:\s*v\S+", text, re.MULTILINE), f"no idf line: {text!r}"
    assert re.search(r"^uptime\s*:\s*\d+\s*s\b", text, re.MULTILINE), f"no uptime line: {text!r}"


def test_vbatt_reports_unconfirmed_sense(console):
    # Battery sense (GPIO 9) is not confirmed on this build, so -1 is the correct reply.
    text = console.command("vbatt")
    assert "VBATT: -1 mV" in text, f"vbatt reply: {text!r}"


@pytest.mark.parametrize("command", ["hsvsense", "vbattcal"])
def test_unsupported_commands_report_not_supported(console, command):
    text = console.command(command)
    assert "ERROR: ESP_ERR_NOT_SUPPORTED" in text, f"{command} reply: {text!r}"


@pytest.mark.parametrize("command", ["kline", "j1850"])
def test_bus_commands_without_argument_print_usage(console, command):
    # Only the usage text. No hex payload is ever sent to a bus.
    text = console.command(command)
    assert f"Usage: {command} <hex string>|stop" in text, f"{command} reply: {text!r}"


def test_mode_reports_link_and_modes(console):
    text = console.command("mode")
    assert "Link: elm327" in text, f"mode reply: {text!r}"
    assert "Modes: elm327 slcan j2534" in text, f"mode reply: {text!r}"


def test_mode_slcan_then_restored(console, elm327_restored):
    # elm327_restored puts the front end back to elm327 in teardown, even on failure.
    text = console.command("mode slcan")
    assert "MODE slcan" in text, f"mode slcan reply: {text!r}"


def test_mode_elm327_replies(console):
    text = console.command("mode elm327")
    assert "MODE elm327" in text, f"mode elm327 reply: {text!r}"


def test_vif_shows_elm327_front_end(console, elm327_restored):
    console.set_front_end("elm327")
    text = console.command("vif")
    assert "Vehicle interface" in text, f"vif reply: {text!r}"
    assert "link front-end elm327" in text, f"vif reply: {text!r}"


def test_help_lists_commands(console):
    text = console.command("help", timeout=10.0)
    for word in ("version", "vbatt", "mode", "vif"):
        assert word in text, f"'{word}' not in help reply: {text!r}"
