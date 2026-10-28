// pycan::frame — Python CAN-frame carrier (hop2: controller → device).
//
// The Python controller's OWN native CAN package (a struct.pack image).  Composed
// from the shared bus:: atoms so a device of ANY language reads it by qname;
// Python's OWN (8-byte-scalar) byte layout gives it a distinct id from the SC /
// FMI / Rust CAN frames.  can::Ide is the inline CAN-specific flag.
//
// Field map (byte_offset : atom : size) — Python 8-byte-scalar image:
//   0   bus::Locator    8   uint64 — CAN message id
//   8   bus::Length     8   uint64 — valid payload byte count (0..8)
//   16  can::Ide        8   uint64 — Standard=0 / Extended=1
//   24  bus::Data       8   bstr   — inline payload (classic CAN <=8 bytes)
//       core::Extensions     sub-meta (no data-region bytes)
//
// Total core data region: 32 bytes.  One-way.

local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'pycan::frame',
  atoms: [
    { atom: bus.Locator, byte_offset: 0,  byte_size: 8 },
    { atom: bus.Length,  byte_offset: 8,  byte_size: 8 },
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
      byte_offset: 16, byte_size: 8 },
    { atom: bus.Data,    byte_offset: 24, byte_size: 8 },
    { atom: core.Extensions },
  ],
}
