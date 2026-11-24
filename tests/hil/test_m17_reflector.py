# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The M17 reflector client (docs/spec/m17-ip.md) on the radio, read through the debug link's m17net section and driven
by RADIO op 44 sub-op 7. Nothing is transmitted on the air: no PTT is pressed.

test_m17_reflector_view: the m17net section answers with its keys (m17-ip.md 10). Skips on a build without
CONFIG_NET_M17. Contacts nothing.

test_m17_reflector_link (opt-in: RFOUNDRY_HIL_M17_REFLECTOR="ADDRESS[:PORT] NAME MODULE", e.g. "192.0.2.10 M17-XYZ C",
a reflector the tester chooses; there is no default host): the radio in USB network or PPP mode with the host routing
for it (skipped while `m17net.why` says `no_net`), M17 running on the channel on view, our M17 callsign set. Links
(sub-op 7), waits for `m17net.state=linked`, then stays linked past the reflector's 30 s silence timeout (Part II: it
drops a client it hears nothing from for 30 s, and PINGs every 3 s). A reflector that ignored our PONGs (gap 2, our
callsign without a module letter, or gap 3, PONG's callsign checked against CONN's: m17-ip.md 8) would have dropped us
by then and stopped its PINGs, which our side would notice only 30 s later: so the test reads the counters at
LATE_FROM_S and again at LATE_TO_S and wants PINGs between the two, PONGs sent, the link still up, no `losses` and no
DISC from the reflector. A pass settles gaps 2 and 3 for that reflector. Then it unlinks and waits for `off`. About a
minute. This contacts a network service: run it only against a reflector whose operator agrees.
"""
import os
import socket
import struct
import time

import pytest

from rfoundry_hil import as_int, skip
from test_m17 import M17On, m17_view

ENV = "RFOUNDRY_HIL_M17_REFLECTOR"
SUB_NET = 7
LINK_WAIT_S = 15.0      # five CONNs two seconds apart, and a margin
SILENCE_S = 30.0        # a reflector drops a client it heard nothing from this long (Part II)
LATE_FROM_S = SILENCE_S + 6.0   # after the linked state: past the reflector's timeout, with a margin ...
LATE_TO_S = LATE_FROM_S + 9.0   # ... and three of its 3 s PINGs later
KEYS = ("state", "why", "reflector", "module", "listen", "ip", "port", "rx", "tx", "rx_src", "rx_packets",
        "tx_packets", "conns", "ackns", "nacks", "pings", "pongs", "discs_tx", "discs_rx", "links", "losses",
        "rx_streams", "rx_frames", "tx_streams", "tx_frames", "pkt_rx", "pkt_tx", "bad", "crc_alt", "unknown",
        "foreign", "rx_dropped", "tx_full", "tx_dropped", "send_busy", "rx_late")


def net_view(radio):
    v = radio.view(["m17net"])
    if v.get("m17net.built") != "1":
        skip("not_built", "built without CONFIG_NET_M17")
    return v


def target():
    """(ip bytes, port, "NAME M") from the environment, or skip: never a host of our own choosing"""
    text = os.environ.get(ENV, "").strip()
    if not text:
        skip("opt_in", f"{ENV} not set: the reflector to link to (\"ADDRESS[:PORT] NAME MODULE\")")
    parts = text.split()
    if len(parts) != 3 or len(parts[2]) != 1:
        pytest.fail(f"{ENV}={text!r}: expected \"ADDRESS[:PORT] NAME MODULE\"")
    addr, _, port = parts[0].partition(":")
    return socket.inet_aton(addr), int(port or 0), f"{parts[1]} {parts[2]}"


@pytest.mark.feature("m17_reflector")
def test_m17_reflector_view(radio):
    v = net_view(radio)
    missing = [k for k in KEYS if f"m17net.{k}" not in v]
    assert not missing, f"m17net keys missing: {missing}"
    assert v["m17net.state"] in ("off", "wait", "connecting", "linked", "unlinking"), v["m17net.state"]
    print(f"\nm17net: {v['m17net.state']} ({v['m17net.why']}), {v['m17net.reflector']} {v['m17net.module']}")


@pytest.mark.feature("m17_reflector")
def test_m17_reflector_link(radio):
    ip, port, name = target()
    net_view(radio)
    v = m17_view(radio)
    if v.get("m17.call") in (None, "-"):
        skip("state", "the radio has no M17 callsign (the radio-wide record, or a radio name that is one)")
    with M17On(radio):
        c0 = radio.view(["m17net"])
        try:
            radio.link.m17(SUB_NET, bytes([1]) + ip + struct.pack("<H", port) + name.encode("ascii"))
            # Linked, refused (a NACK more than before), or no network: the request is taken at the host link
            # task's next poll, so the state before it doesn't count
            v = radio.wait_for(lambda x: x.get("m17net.state") == "linked" or x.get("m17net.why") == "no_net" or
                               as_int(x.get("m17net.nacks")) > as_int(c0["m17net.nacks"]), LINK_WAIT_S,
                               sections=("m17net",), what=f"linked to {name}")
            if v.get("m17net.why") == "no_net":
                skip("state", "the radio's network link is down: USB network or PPP mode with the host routing for it")
            assert v["m17net.state"] == "linked", f"not linked to {name}: {v.get('m17net.why')}"
            linked = time.monotonic()
            time.sleep(max(0.0, linked + LATE_FROM_S - time.monotonic()))
            a = radio.view(["m17net"])
            time.sleep(max(0.0, linked + LATE_TO_S - time.monotonic()))
            v = radio.view(["m17net"])
            for x in (a, v):
                assert x["m17net.state"] == "linked", x
                assert as_int(x["m17net.losses"]) == as_int(c0["m17net.losses"]), "the link was lost"
                assert as_int(x["m17net.discs_rx"]) == as_int(c0["m17net.discs_rx"]), "the reflector disconnected us"
            assert as_int(a["m17net.pongs"]) > as_int(c0["m17net.pongs"]), "no PONG sent"
            # Still PINGed past its timeout: the reflector kept us, our PONGs counted (gaps 2 and 3)
            assert as_int(v["m17net.pings"]) > as_int(a["m17net.pings"]), \
                f"no PING from the reflector after {LATE_FROM_S:.0f} s: it dropped us (gap 2 or 3, m17-ip.md 8)"
            print(f"\nm17net: linked to {v['m17net.reflector']} {v['m17net.module']} for {LATE_TO_S:.0f} s, "
                  f"{v['m17net.pings']} PINGs, bad {v['m17net.bad']}, crc_alt {v['m17net.crc_alt']}, "
                  f"unknown {v['m17net.unknown']}")
        finally:
            radio.link.m17(SUB_NET, bytes([0]))
        radio.wait_for(lambda x: x.get("m17net.state") == "off", 10.0, sections=("m17net",), what="unlinked")
