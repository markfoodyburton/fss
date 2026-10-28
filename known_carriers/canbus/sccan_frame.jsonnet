// sccan::frame — SystemC CAN-frame carrier (hop2: controller → device).
//
// SystemC has no standard CAN package, so its controller reuses the generic
// (TLM-style) transaction layout AS a CAN frame: bus::Locator carries the CAN
// message id (an address, in TLM terms) and bus::Data the payload.  Composed from
// the shared bus:: atoms so a device of ANY language reads it by qname; SystemC's
// OWN (GP-inspired, 8-byte-aligned) layout gives it a distinct id from the FMI /
// Rust / Python CAN frames.  can::Ide is the one inline CAN-specific flag.
//
// Field map (byte_offset : atom : size):
//   0   bus::Locator    8   uint64 — CAN message id (as a TLM-style address)
//   8   bus::Length     4   uint32 — valid payload byte count (0..8)
//   12  can::Ide        4   uint32 — Standard=0 (11-bit) / Extended=1 (29-bit)
//   16  bus::Data       8   bstr   — inline payload (classic CAN <=8 bytes)
//       core::Extensions     sub-meta (no data-region bytes)
//
// Total core data region: 24 bytes.  One-way (no write-back).

local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'sccan::frame',
  atoms: [
    { atom: bus.Locator, byte_offset: 0, byte_size: 8 },
    { atom: bus.Length,  byte_offset: 8, byte_size: 4 },
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
      byte_offset: 12, byte_size: 4 },
    { atom: bus.Data,    byte_offset: 16, byte_size: 8 },
    { atom: core.Extensions },
  ],
}
