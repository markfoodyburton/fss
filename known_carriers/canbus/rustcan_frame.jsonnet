// rustcan::frame — Rust CAN-frame carrier (hop2: controller → device).
//
// The Rust controller's OWN native CAN package (a #[repr(C)] struct).  Composed
// from the shared bus:: atoms so a device of ANY language reads it by qname;
// Rust's OWN (payload-first) byte layout gives it a distinct id from the SC / FMI
// / Python CAN frames.  can::Ide is the inline CAN-specific flag.
//
// Field map (byte_offset : atom : size) — Rust struct order (payload first):
//   0   bus::Data       8   bstr   — inline payload (classic CAN <=8 bytes)
//   8   bus::Locator    4   uint32 — CAN message id
//   12  bus::Length     2   uint16 — valid payload byte count (0..8)
//   14  can::Ide        1   uint8  — Standard=0 / Extended=1
//   15  bus::Operation  1   uint8  — rtr: Write(data)=0 / Read(remote)=1
//       core::Extensions     sub-meta (no data-region bytes)
//
// Total core data region: 16 bytes.  One-way.

local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'rustcan::frame',
  atoms: [
    { atom: bus.Data,    byte_offset: 0, byte_size: 8 },
    { atom: bus.Locator, byte_offset: 8, byte_size: 4 },
    { atom: bus.Length,  byte_offset: 12, byte_size: 2 },
    { atom: {
        qname:            'can::Ide',
        version:          1,
        wire:             'uint',
        default_encoding: { Standard: 0, Extended: 1 },
        mutable_by:       'initiator',
        read_by:          'target',
        phase:            'request',
        description:      'Identifier extension: Standard (11-bit) / Extended (29-bit).',
      },
      byte_offset: 14, byte_size: 1 },
    { atom: bus.Operation, byte_offset: 15, byte_size: 1, encoding: { Write: 0, Read: 1 } },
    { atom: core.Extensions },
  ],
}
