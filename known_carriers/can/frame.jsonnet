// CAN carrier shape — classic CAN frame (in-process form).
//
// Spec:           FMI Layered Standard for Bus Communication (FMI-LS-BUS)
// Atom registry:  atoms/bus/ (shared bus-transaction roles) + one inline CAN flag
//
// A faithful in-process representation of the FMI-LS-BUS CanTransmit
// operation (op 0x0010, fmi3LsBusCanOperationCanTransmit): its semantic
// fields mapped to fixed byte offsets.  The FMI-LS-BUS operation header
// (opCode/length) is NOT carried; the FSS carrier id + meta play that role,
// per the same principle as the TLM GP carrier.
//
// REUSE, NOT a parallel vocabulary.  The identifier, operation, length and
// payload are the SAME roles a TLM transaction carries, so they are the SAME
// atoms (bus::Locator / Operation / Length / Data) — a consumer that walks the
// meta finds them by qname whether the carrier is a TLM txn or a CAN frame, at
// whatever byte_size the composition gives (a CAN id is 4 bytes here, a TLM
// address 8 — same atom, different width).  This is the whole point: shared
// atoms ARE the interop, so a SystemC/FMI/Rust/Python consumer needs NO bridge
// and NO per-language translation to accept a CAN frame.  Only the
// identifier-extension flag is genuinely CAN-specific with no cross-protocol
// counterpart, so — like tlm::GpOption — it is defined INLINE here (qname
// can::Ide) rather than minted as a reusable registry atom.
//
//   bus::Operation encoding is overridden to the FMI-LS-BUS rtr byte value
//   (data-frame = Write = 0, remote-request = Read = 1).
//
// Classic CAN payload is small and fixed (<=8 bytes), so bus::Data is INLINE
// (byte_size: 8) — no fss:local-ptr needed.
//
// Field map (byte_offset : atom : size):
//   0   bus::Locator    4   uint32 — CAN message identifier
//   4   can::Ide        1   uint8  — inline; Standard=0 (11-bit) / Extended=1 (29-bit)
//   5   bus::Operation  1   uint8  — enc:{Write=0, Read=1} (rtr: data=0, remote=1)
//   6   bus::Length     2   uint16 — valid byte count in bus::Data (0..8)
//   8   bus::Data       8   bstr   — inline payload (classic CAN <=8 bytes)
//       core::Extensions     sub-meta (no data-region bytes)
//
// Total core data region: 16 bytes.  CAN-FD / CAN-XL ride core::Extensions (future).

local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'can::frame',
  atoms: [
    // CAN message identifier (11-bit standard or 29-bit extended, per can::Ide).
    { atom: bus.Locator, byte_offset: 0, byte_size: 4 },

    // Identifier extension flag — CAN-specific, no cross-protocol counterpart,
    // so defined inline (not a reusable registry atom; cf. tlm::GpOption).
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
      byte_offset: 4, byte_size: 1 },

    // Remote transmission request: data=Write (0), remote=Read (1).
    // Encoding overrides bus::Operation default to match FMI-LS-BUS rtr byte value.
    { atom: bus.Operation, byte_offset: 5, byte_size: 1, encoding: { Write: 0, Read: 1 } },

    // Number of valid payload bytes in bus::Data (0..8 for classic CAN).
    { atom: bus.Length,    byte_offset: 6, byte_size: 2 },

    // Inline payload; classic CAN <=8 bytes, of which bus::Length are valid.
    { atom: bus.Data,      byte_offset: 8, byte_size: 8 },

    // Extension hook (sub-meta): no data-region bytes; excluded from the
    // id hash per docs/spec/01-carrier.md §Identity.
    { atom: core.Extensions },
  ],
}
