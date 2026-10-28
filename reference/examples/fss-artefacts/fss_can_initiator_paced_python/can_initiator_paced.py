# This file is part of fss
# Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
# Author: Mark Burton
#
# SPDX-License-Identifier: BSD-3-Clause

"""can_initiator_paced.py — TIME-SYNCED paced CAN source (python-loader artefact).

The Python counterpart of fss_can_initiator_paced_systemc: a kernel-less producer
that emits 16 CAN TX register-WRITEs on "socket", one every 12 quanta (12 µs).
Back-pressure is pacing — the producer is SLOWER than the controller's 10-quantum
drain, so the FIFO (cap 4) never overflows and all 16 frames are delivered.

R12 — hop1 is a REGISTER WRITE into the controller, and this initiator emits its
OWN native carrier for it: pyreg::write ({bus::Locator u64@0, bus::Operation u64@8,
bus::Length u64@16, bus::Data@24 inline(8)}).  All fields are bus:: atoms, so the
controller reads it by meta-resolving qnames with no bridge.  bus::Data is carried
INLINE (the classic register form: ≤8 bytes), so there is no payload pointer to
keep alive.  pyreg::write is ONE-WAY (no bus::Status): a paced producer never polls.

Loader contract (tick extension): tick(now_ps) is called once per quantum (1 µs)
by the loader's clock thread.  Returns an iterable of send-specs
(out_name, carrier_id, data, meta) — same tuple shape as start().  Has tick() →
loader gives it stop-on-detach (no done() needed; the controller's detach stops us).
"""
import json
import struct

from pyreg_write import (
    FSS_PYREG_FLAT_ID,
    FSS_PYREG_FLAT_META,
    FSS_PYREG_OFF_bus_Locator,
    FSS_PYREG_OFF_bus_Operation,
    FSS_PYREG_OFF_bus_Length,
    FSS_PYREG_OFF_bus_Data,
)

CAN_ID = 0x100
DLC = 8
REG_BYTES = 32          # pyreg::write core data region (Locator/Operation/Length/Data)
DEFAULT_FRAMES = 16
DEFAULT_PRODUCE_EVERY = 12   # quanta between frames (strictly > drain's 10 → no overflow)


def fss_create(name):
    return CanInitiatorPaced(name)


class CanInitiatorPaced:
    def __init__(self, name):
        self.name = name
        self._ticks = 0
        self._sent = 0
        self._frames = DEFAULT_FRAMES
        self._produce_every = DEFAULT_PRODUCE_EVERY

    def endpoints(self):
        return [("socket", "out")]

    def config(self, s):
        # fss/config JSON pushed before SIM_STARTED: "frames" = total frames to
        # emit; "period_us" = send period in µs (== quanta between frames, 1 µs/quantum).
        cfg = json.loads(s)
        if "frames" in cfg:
            self._frames = int(cfg["frames"])
        if cfg.get("period_us", 0) > 0:
            self._produce_every = int(cfg["period_us"])

    def tick(self, now_ps):
        self._ticks += 1
        if self._ticks % self._produce_every != 0 or self._sent >= self._frames:
            return []

        i = self._sent
        # Build a pyreg::write register-write; bus::Data carried INLINE.
        reg = bytearray(REG_BYTES)
        struct.pack_into("<Q", reg, FSS_PYREG_OFF_bus_Locator, CAN_ID)
        struct.pack_into("<Q", reg, FSS_PYREG_OFF_bus_Operation, 1)  # Write
        struct.pack_into("<Q", reg, FSS_PYREG_OFF_bus_Length, DLC)
        # Payload: byte0 = frame index, bytes 1..7 = 1..7 (matches can_initiator.py).
        reg[FSS_PYREG_OFF_bus_Data] = i & 0xFF
        for b in range(1, 8):
            reg[FSS_PYREG_OFF_bus_Data + b] = b
        self._sent += 1
        return [("socket", FSS_PYREG_FLAT_ID, bytes(reg), FSS_PYREG_FLAT_META)]
