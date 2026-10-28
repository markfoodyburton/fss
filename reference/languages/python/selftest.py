#!/usr/bin/env python3
# This file is part of fss
# Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
# Author: Mark Burton
#
# SPDX-License-Identifier: BSD-3-Clause

"""Self-test for the Python FSS binding.

Two checks:
  1. ABI layout — the ctypes structs match the C sizes exactly.
  2. Real interop — drive the in-proc transport plugin and the fss_minimal_echo
     artefact (both genuine FSS .so/.dylib modules) entirely from Python:
     construct the echo node (the placement role, via the artefact ABI), wire a
     local initiator to its "in" target over an inproc channel (channel/attach),
     send a 1-byte carrier, and confirm the echo artefact incremented the byte
     IN PLACE (a round-trip channel).

This is interop, not a loopback: Python is the host, the transport and artefact
are the real C modules the other examples use.

  usage: selftest.py <inproc-plugin> <fss_minimal_echo-module>
"""
import ctypes as C
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fss  # noqa: E402


def check_layout():
    assert C.sizeof(fss.Carrier) == 40, C.sizeof(fss.Carrier)
    assert C.sizeof(fss.Endpoint) == 32, C.sizeof(fss.Endpoint)
    assert C.sizeof(fss._Transport) == 32, C.sizeof(fss._Transport)
    assert fss._Transport.self.offset == 0
    assert fss._Transport.channel.offset == 8
    assert fss._Transport.attach.offset == 16
    assert fss._Transport.send_to.offset == 24
    print("[ok] ABI layout: Carrier=40 Endpoint=32 Transport=32")


def check_interop(inproc_path, echo_path):
    t = fss.Transport(inproc_path)          # inproc takes no config
    echo = fss.Artefact(echo_path, "echo")  # construct the node (placement role)
    echo_in = echo.endpoint("in")
    if not echo_in:
        raise RuntimeError("echo has no 'in' endpoint")

    # Wire a 1:1 round-trip channel: echo/in is the TARGET (its receive becomes
    # the sink), a local endpoint the INITIATOR (inproc's identity attach installs
    # local.send = echo_in.receive, so a send lands straight in the echo).
    ch = t.channel("echo", round_trip=1, owner=1)
    if not ch:
        raise RuntimeError("channel() returned NULL")
    if t.attach(ch, echo_in, fss.FSS_ROLE_TARGET) != fss.FSS_SEND_OK:
        raise RuntimeError("attach TARGET failed")
    local = fss.Endpoint()
    if t.attach(ch, local, fss.FSS_ROLE_INITIATOR) != fss.FSS_SEND_OK or not local.send:
        raise RuntimeError("attach INITIATOR failed")

    buf = (C.c_uint8 * 1)(10)
    c = fss.Carrier(
        id=0,
        data_ptr=C.cast(buf, C.c_void_p),
        data_size=1,
        meta_ptr=None,
        meta_size=0,
    )
    rc = fss.fss_send(local, c)  # echo: 10 -> 11, in place
    if rc != fss.FSS_SEND_OK:
        raise RuntimeError("send returned %d" % rc)
    if buf[0] != 11:
        raise RuntimeError("echo did not mutate: got %d, want 11" % buf[0])

    echo.destroy()
    t.destroy()
    print("[ok] interop: Python host wired inproc channel/attach to fss_minimal_echo (10 -> 11)")


def main():
    check_layout()
    if len(sys.argv) == 3:
        check_interop(sys.argv[1], sys.argv[2])
    else:
        print("[skip] interop — pass <inproc-plugin> <echo-module> to run it")
    print("PASS")


if __name__ == "__main__":
    main()
