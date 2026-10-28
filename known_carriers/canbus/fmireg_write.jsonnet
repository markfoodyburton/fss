// fmireg::write — FMI CAN-controller REGISTER-WRITE carrier (hop1: initiator → controller).
//
// An FMI CAN initiator does NOT emit a CAN frame; it WRITES the controller's
// transmit registers (message-id register, DLC register, data registers), and
// the CONTROLLER later assembles the frame.  FMI has no generic bus-transaction
// type, so this is a CAN-inspired register-write package — its OWN native shape,
// NOT TLM (TLM is SystemC's).
//
// Composed from the shared bus:: atoms, so a controller of ANY language reads it
// by qname (bus::Locator = the target register / message-id, bus::Operation =
// Write, bus::Length = payload byte count, bus::Data = the register payload) with
// NO bridge.  Its LAYOUT is FMI's own (distinct offsets ⇒ distinct id from the
// TLM / Rust / Python register carriers); reuse is by shared atoms, not shared
// bytes.
//
// Field map (byte_offset : atom : size) — FMI's compact register image:
//   0   bus::Locator    4   uint32 — TX message-id register
//   4   bus::Operation  4   uint32 — Read=0 / Write=1 (paced producer writes)
//   8   bus::Length     4   uint32 — payload byte count (0..8)
//   12  bus::Status     4   int32  — write-back result (Ok=0)
//   16  bus::Data       8   bstr   — inline register payload (<=8 bytes)
//       core::Extensions     sub-meta (no data-region bytes)
//
// Total core data region: 24 bytes.

local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'fmireg::write',
  atoms: [
    { atom: bus.Locator,   byte_offset: 0,  byte_size: 4 },
    { atom: bus.Operation, byte_offset: 4,  byte_size: 4 },
    { atom: bus.Length,    byte_offset: 8,  byte_size: 4 },
    { atom: bus.Status,    byte_offset: 12, byte_size: 4 },
    { atom: bus.Data,      byte_offset: 16, byte_size: 8 },
    { atom: core.Extensions },
  ],
}
