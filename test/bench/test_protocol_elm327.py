"""ELM327 protocol port checks on the RejsaCAN v3.4 board (front end elm327). No bus.

Each test opens the data port once (elm_port fixture) and closes it in teardown.
Opening the data port restarts the firmware link.
"""

import time

from conftest import require_build


def test_ati_identifies_elm327(elm_port):
    reply = elm_port.at("ATI")
    assert reply.prompt_seen, f"no '>' prompt: {reply.raw!r}"
    assert any("ELM327 v2.3" in line for line in reply.lines), f"ATI reply: {reply.lines!r}"


def test_protocol_6_selects_can(elm_port):
    # ATSP6 is shared by both builds: the CAN selection has no build guard.
    ok = elm_port.at("ATSP6")
    assert ok.lines == ["OK"], f"ATSP6 reply: {ok.lines!r} raw={ok.raw!r}"

    dpn = elm_port.at("ATDPN")
    assert dpn.lines == ["6"], f"ATDPN reply: {dpn.lines!r} raw={dpn.raw!r}"


def test_rejsacan_refuses_non_can_protocols_and_keeps_can(elm_port, build_info):
    # The refusal is a RejsaCAN guard (elm327_at.c): only CAN exists on this board.
    require_build(build_info, "RejsaCAN", "ATSP1 and ATSP3 are refused with '?'")

    ok = elm_port.at("ATSP6")
    assert ok.lines == ["OK"], f"ATSP6 reply: {ok.lines!r} raw={ok.raw!r}"

    # Non-CAN protocols are refused with '?'; the CAN session (ATSP6) is kept.
    for protocol in ("ATSP1", "ATSP3"):
        refused = elm_port.at(protocol)
        assert refused.lines == ["?"], f"{protocol} reply: {refused.lines!r} raw={refused.raw!r}"

    dpn = elm_port.at("ATDPN")
    assert dpn.lines == ["6"], f"ATDPN reply: {dpn.lines!r} raw={dpn.raw!r}"


def test_0100_without_bus_searches_then_no_response(elm_port):
    start = time.monotonic()
    reply = elm_port.at("0100", timeout=20.0)
    elapsed = time.monotonic() - start

    assert reply.prompt_seen, f"no '>' prompt within 20 s: {reply.raw!r}"
    assert elapsed < 20.0, f"0100 took {elapsed:.1f} s"
    assert "SEARCHING..." in reply.lines, f"0100 reply: {reply.lines!r} raw={reply.raw!r}"
    data = [line for line in reply.lines if line != "SEARCHING..."]
    assert data == ["?"], f"expected no data bytes, got {data!r} raw={reply.raw!r}"
