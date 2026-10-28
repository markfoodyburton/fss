# This file is part of fss
# Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
# Author: Mark Burton
#
# SPDX-License-Identifier: BSD-3-Clause

"""can_device_paced.py — a TIME-SYNCED Python CAN device (python-loader artefact).

The Python counterpart of fss_can_target_paced_{systemc,rust}: the plain Python
CAN rx target plus participation in time-sync.  The loader owns the invented
clock (a C thread honouring the time::window protocol on fss/time) — engaged when
the host binds the node's fss/time port, which the loader allocates on demand;
the model declares nothing for it.  This model only adds a done() the loader polls
after each inbound frame.  Being the SINK, done() drives teardown: once all
expected frames have arrived the loader stops the clock, which sends an
open_window (detach) to the controller, cascading the federation stop.

R12 — the endpoint receive is the SHIM: it accepts ANY framework's CAN frame
(sccan/fmican/rustcan/pycan — four distinct byte layouts) by META-RESOLVING
bus::Locator/bus::Data from the carrier's own meta by qname (fss.meta_find), never
gating on a carrier id, never a fixed offset.  The model just counts.

Contract addition over the plain device:
  device.done() -> bool   # sink completion (polled by the loader after receive)
"""
import ctypes

import fss  # the shared Python binding: fss.meta_find (carrier-meta resolver)

FSS_SEND_OK = 0
DEFAULT_EXPECT_FRAMES = 16


def fss_create(name):
    return CanDevicePaced(name)


def _read_uint(data, off, sz):
    """Little-endian unsigned int of `sz` bytes at `off` from a memoryview."""
    return int.from_bytes(bytes(data[off:off + sz]), "little")


class CanDevicePaced:
    def __init__(self, name):
        self.name = name
        self.rx = 0
        self.last_id = 0
        self.last_b0 = 0
        self._expect = DEFAULT_EXPECT_FRAMES

    # No endpoints() → the loader defaults to a single inbound "can_rx".  The
    # fss/time timing port is allocated by the loader when the host binds it.

    def config(self, s):
        # fss/config JSON pushed before SIM_STARTED: "frames" = the count this
        # sink expects before signalling done().
        import json
        cfg = json.loads(s)
        if "frames" in cfg:
            self._expect = int(cfg["frames"])

    def receive(self, carrier_id, data, meta):
        # SHIM: meta-resolve the frame's fields by qname (id-agnostic) → native
        # (id, first payload byte).  bus::Data is INLINE in every CAN frame, but
        # resolve the layout and honour a by-pointer form too (uniform with hop1).
        loc_f = fss.meta_find(meta, "bus::Locator")
        data_f = fss.meta_find(meta, "bus::Data")
        if not (loc_f and data_f):
            return FSS_SEND_OK   # meta absent/foreign — drop, never guess
        self.last_id = _read_uint(data, loc_f[0], loc_f[1])
        d_off, _d_sz, d_inline = data_f
        if d_inline:
            self.last_b0 = data[d_off]
        else:
            ptr = _read_uint(data, d_off, 8)
            self.last_b0 = ctypes.string_at(ptr, 1)[0] if ptr else 0
        self.rx += 1
        return FSS_SEND_OK

    def done(self):
        # Sink complete → the loader stops the clock → detach → teardown cascade.
        return self.rx >= self._expect

    def report(self):
        print("PYTHON CAN TARGET PASS: rx={} last_id=0x{:X} last_b0={}".format(
            self.rx, self.last_id, self.last_b0), flush=True)
