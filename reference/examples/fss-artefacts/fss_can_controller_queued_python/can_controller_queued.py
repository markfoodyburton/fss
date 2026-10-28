# This file is part of fss
# Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
# Author: Mark Burton
#
# SPDX-License-Identifier: BSD-3-Clause

"""can_controller_queued.py — TIME-SYNCED queued CAN relay (python-loader artefact).

The Python counterpart of fss_can_controller_queued_systemc: a bounded TX FIFO
(cap 4) filled by receive() and drained every 10 quanta in tick().

R12 — the endpoint receive is the SHIM: it accepts ANY framework's hop1 register
WRITE by META-RESOLVING bus::Operation/Locator/Length/Data (+ optional bus::Status)
from the carrier's own meta by qname (fss.meta_find), NEVER gating on a carrier id,
honouring inline vs by-pointer bus::Data.  So a SystemC (tlm::transaction, Data by
pointer), FMI (fmireg::write), Rust (rustreg::write) or Python (pyreg::write,
one-way) initiator all interoperate with no bridge.  Its OUTBOUND hop2 is Python's
OWN native carrier, pycan::frame.

READ polls (from an SC initiator) write the free-slot count into the payload
pointer and set bus::Status=Ok in the carrier itself.  The loader hands receive() a
WRITABLE memoryview over the in-proc gp, so the ack is a struct.pack_into into
`data`.  A Python/Rust initiator paces instead of polling, so it never needs the
status (and pyreg::write has none).

Has tick() → loader gives it stop-on-detach (no done() needed; device detach
cascades: device → controller → initiator teardown).
"""
import ctypes
import struct

import fss  # the shared Python binding: fss.meta_find (carrier-meta resolver)
from pycan_frame import (
    FSS_PYCAN_FLAT_ID,
    FSS_PYCAN_FLAT_META,
    FSS_PYCAN_OFF_bus_Locator,
    FSS_PYCAN_OFF_bus_Length,
    FSS_PYCAN_OFF_can_Ide,
    FSS_PYCAN_OFF_bus_Data,
)

FSS_SEND_OK = 0
FSS_SEND_NO_RECEIVER = 2
CAP = 4
DRAIN_EVERY = 10   # quanta between drain ticks (= 10 µs, matching SC model)
PYCAN_BYTES = 32   # pycan::frame core data region


def fss_create(name):
    return CanControllerQueued(name)


def _read_uint(data, off, sz):
    """Little-endian unsigned int of `sz` bytes at `off` from a memoryview."""
    return int.from_bytes(bytes(data[off:off + sz]), "little")


class CanControllerQueued:
    def __init__(self, name):
        self.name = name
        self._fifo = []   # list used as queue; no lock needed (GIL + cooperative)
        self._ticks = 0

    def endpoints(self):
        return [("tx", "in"), ("can_tx", "out")]

    def receive(self, carrier_id, data, meta):
        # SHIM: meta-resolve the register-write's fields by qname (id-agnostic).
        op_f = fss.meta_find(meta, "bus::Operation")
        loc_f = fss.meta_find(meta, "bus::Locator")
        len_f = fss.meta_find(meta, "bus::Length")
        data_f = fss.meta_find(meta, "bus::Data")
        if not (op_f and loc_f and len_f and data_f):
            return FSS_SEND_NO_RECEIVER   # required field absent — reject
        stat_f = fss.meta_find(meta, "bus::Status")   # optional (one-way pyreg has none)

        def write_status(s):
            if stat_f:
                struct.pack_into("<i", data, stat_f[0], s)

        op = _read_uint(data, op_f[0], op_f[1])
        d_off, _d_sz, d_inline = data_f

        if op == 0:  # READ — an SC initiator polling free space
            free = CAP - len(self._fifo)
            if d_inline:
                struct.pack_into("<I", data, d_off, free)
            else:
                ptr = _read_uint(data, d_off, 8)   # by-pointer: the poll buffer address
                if ptr:
                    ctypes.memmove(ptr, struct.pack("<I", free), 4)
            write_status(1)   # ack OK so the poller trusts the free count
            return FSS_SEND_OK

        # WRITE — read the register values, then enqueue a derived pycan::frame.
        if len(self._fifo) >= CAP:
            write_status(0)   # full → not-Ok (paced producers won't hit this)
            return FSS_SEND_OK

        locator = _read_uint(data, loc_f[0], loc_f[1])
        length = _read_uint(data, len_f[0], len_f[1])
        n = min(length, 8)
        if d_inline:
            payload = bytes(data[d_off:d_off + n])
        else:
            ptr = _read_uint(data, d_off, 8)
            payload = ctypes.string_at(ptr, n) if (ptr and n) else b""

        # MODEL → native pycan::frame {Locator u64@0, Length u64@8, can::Ide u64@16,
        # Data@24 inline(8)}.
        frame = bytearray(PYCAN_BYTES)
        struct.pack_into("<Q", frame, FSS_PYCAN_OFF_bus_Locator, locator & 0xFFFFFFFFFFFFFFFF)
        struct.pack_into("<Q", frame, FSS_PYCAN_OFF_bus_Length, length)
        struct.pack_into("<Q", frame, FSS_PYCAN_OFF_can_Ide, 0)   # Standard
        frame[FSS_PYCAN_OFF_bus_Data:FSS_PYCAN_OFF_bus_Data + n] = payload
        self._fifo.append(bytes(frame))
        write_status(1)   # ack
        return FSS_SEND_OK

    def tick(self, now_ps):
        self._ticks += 1
        if self._ticks % DRAIN_EVERY == 0 and self._fifo:
            frame = self._fifo.pop(0)
            return [("can_tx", FSS_PYCAN_FLAT_ID, frame, FSS_PYCAN_FLAT_META)]
        return []
