"""SLCAN front end checks on the RejsaCAN v3.4 board (no vehicle, no bus traffic asserted).

The slcan_port fixture switches the console to `mode slcan`, opens the data port once
per test, sends C and closes in teardown, then restores `mode elm327`.

Replies: CR (\\r) for OK, BELL (\\x07) for refused. Frames received (t/T/r/R lines)
are not asserted on.
"""

BELL = b"\x07"
CR = b"\r"


def test_version_reply(slcan_port):
    reply = slcan_port.send("V")
    assert reply.startswith(b"V"), f"V reply: {reply!r}"
    assert reply.endswith(CR), f"V reply not CR-terminated: {reply!r}"
    assert BELL not in reply, f"V refused with BELL: {reply!r}"


def test_serial_number_reply(slcan_port):
    reply = slcan_port.send("N")
    assert reply.startswith(b"N"), f"N reply: {reply!r}"
    assert reply.endswith(CR), f"N reply not CR-terminated: {reply!r}"
    assert BELL not in reply, f"N refused with BELL: {reply!r}"


def test_speed_open_close_and_status(slcan_port):
    assert slcan_port.send("S6") == CR, "S6 must reply a single CR"
    assert slcan_port.send("O") == CR, "O must reply a single CR"
    assert slcan_port.send("C") == CR, "C must reply a single CR"
    assert slcan_port.send("F") == b"F00\r", "F must report no overrun (F00)"


def test_listen_only_open_refuses_transmit(slcan_port):
    # S6 first so the channel has a rate; the spec sequence is L, then the refused t1230.
    assert slcan_port.send("S6") == CR, "S6 must reply a single CR"
    assert slcan_port.send("L") == CR, "L must open listen-only with a single CR (no BELL)"
    assert slcan_port.send("t1230") == BELL, "t1230 must be refused with BELL in listen-only"
    assert slcan_port.send("C") == CR, "C must close the listen-only channel with CR"
