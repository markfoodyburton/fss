// rustreg::write — Rust CAN-controller REGISTER-WRITE carrier (hop1: initiator → controller).
//
// The Rust initiator's OWN native register-write package (a #[repr(C)] struct),
// NOT TLM.  Same roles as every register write — composed from the shared bus::
// atoms so any controller reads it by qname — but Rust's OWN byte layout
// (distinct offsets ⇒ distinct id).  Reuse is by shared atoms, not shared bytes.
//
// Field map (byte_offset : atom : size) — Rust's struct order (payload first):
//   0   bus::Data       8   bstr   — inline register payload (<=8 bytes)
//   8   bus::Locator    4   uint32 — TX message-id register
//   12  bus::Length     4   uint32 — payload byte count (0..8)
//   16  bus::Operation  4   uint32 — Read=0 / Write=1
//   20  bus::Status     4   int32  — write-back result (Ok=0)
//       core::Extensions     sub-meta (no data-region bytes)
//
// Total core data region: 24 bytes.

local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'rustreg::write',
  atoms: [
    { atom: bus.Data,      byte_offset: 0,  byte_size: 8 },
    { atom: bus.Locator,   byte_offset: 8,  byte_size: 4 },
    { atom: bus.Length,    byte_offset: 12, byte_size: 4 },
    { atom: bus.Operation, byte_offset: 16, byte_size: 4 },
    { atom: bus.Status,    byte_offset: 20, byte_size: 4 },
    { atom: core.Extensions },
  ],
}
