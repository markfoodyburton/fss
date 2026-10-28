// pyreg::write — Python CAN-controller REGISTER-WRITE carrier (hop1: initiator → controller).
//
// The Python initiator's OWN native register-write package (a struct.pack image),
// NOT TLM.  Same roles as every register write — composed from the shared bus::
// atoms so any controller reads it by qname — but Python's OWN byte layout
// (wider 8-byte scalar registers; distinct offsets ⇒ distinct id).  Reuse is by
// shared atoms, not shared bytes.
//
// Field map (byte_offset : atom : size) — Python's 8-byte-scalar image:
//   0   bus::Locator    8   uint64 — TX message-id register
//   8   bus::Operation  8   uint64 — Read=0 / Write=1
//   16  bus::Length     8   uint64 — payload byte count (0..8)
//   24  bus::Data       8   bstr   — inline register payload (<=8 bytes)
//       core::Extensions     sub-meta (no data-region bytes)
//
// Total core data region: 32 bytes.

local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'pyreg::write',
  atoms: [
    { atom: bus.Locator,   byte_offset: 0,  byte_size: 8 },
    { atom: bus.Operation, byte_offset: 8,  byte_size: 8 },
    { atom: bus.Length,    byte_offset: 16, byte_size: 8 },
    { atom: bus.Data,      byte_offset: 24, byte_size: 8 },
    { atom: core.Extensions },
  ],
}
