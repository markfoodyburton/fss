// fmican::frame — the FMI-LS-BUS classic-CAN transmit operation ITSELF, wrapped
// as an FSS carrier with NO repacking.  The carrier's data region is the packed
// fmi3LsBusCanOperationCanTransmit the FMU produces (fmi3LsBusCan.h): an 8-byte
// operation header, then the CAN fields.  The controller ships that struct
// verbatim (data_ptr = the FMU's Tx op); the meta below says where each field
// sits INSIDE it, so a device of ANY language reads it by bus:: qname with no
// sender-side translation.  This is the FMI analogue of tlm::transaction shipping
// the live generic payload: the sender emits its real native structure, the
// receiver reinterprets via the meta.
//
// fmi3LsBusCanOperationCanTransmit (#pragma pack(1), so byte-exact):
//   0   opCode      u32   fmi3LsBusOperationHeader.opCode   -> fmi::OpCode (inline)
//   4   length      u32   fmi3LsBusOperationHeader.length   -> fmi::OpLength (inline)
//   8   id          u32   CAN message id                    -> bus::Locator
//   12  ide         u8    Standard=0 / Extended=1           -> can::Ide (inline)
//   13  rtr         u8    data=0 / remote=1                 -> bus::Operation
//   14  dataLength  u16   valid payload byte count (0..8)   -> bus::Length
//   16  data        <=8   payload                           -> bus::Data (inline)
//
// A foreign-wrapping carrier over a fixed external format: there is no spare room
// in the op image for an FSS extension, so core::Extensions is omitted (the
// carrier is sealed; see 04-protocols.md §Inclusion).  One-way.

local bus = import '../../atoms/bus.libsonnet';

// The FMI-LS-BUS operation-header words are FMI-specific, not reusable bus::
// roles, so they are defined inline (03-atoms.md §Sharing principle).  Naming
// them makes the carrier a faithful description of the whole operation, header
// and all, rather than of a re-cut subset.
local fmiOpCode = {
  qname:       'fmi::OpCode',
  version:     1,
  wire:        'uint',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
  description: 'FMI-LS-BUS operation code (fmi3LsBusOperationHeader.opCode).',
};
local fmiOpLength = {
  qname:       'fmi::OpLength',
  version:     1,
  wire:        'uint',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
  description: 'FMI-LS-BUS operation total length (fmi3LsBusOperationHeader.length).',
};

{
  name: 'fmican::frame',
  atoms: [
    { atom: fmiOpCode,     byte_offset: 0,  byte_size: 4 },
    { atom: fmiOpLength,   byte_offset: 4,  byte_size: 4 },
    { atom: bus.Locator,   byte_offset: 8,  byte_size: 4 },
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
      byte_offset: 12, byte_size: 1 },
    { atom: bus.Operation, byte_offset: 13, byte_size: 1, encoding: { Write: 0, Read: 1 } },
    { atom: bus.Length,    byte_offset: 14, byte_size: 2 },
    { atom: bus.Data,      byte_offset: 16, byte_size: 8 },
  ],
}
