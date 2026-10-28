// TLM carrier shape — the tlm_generic_payload (IEEE 1666.1 §6) as an FSS carrier.
//
// Spec:           docs/spec/04-protocols.md
// Atom registry:  atoms/{bus,core}/
//
// This is the COMPLETE (live) carrier: it describes the bus:: fields at their
// REAL offsets inside a live tlm_generic_payload object, and bears a
// core::TypeId atom (the vtable-pointer marker at offset 0).  Because it carries
// core::TypeId, an in-process peer that recognises the carrier id may take the
// ZERO-COPY fast path — cast data_ptr straight to the live GP (confirmed by a
// runtime typeid== through the vptr) — instead of walking the meta.  See
// docs/spec/02-encodings.md §core::TypeId and the SystemC-TLM binding.
//
// The FLAT form (purely portable bytes) is NOT authored here: a transport
// derives it at serialise time by dropping the non_serializable core::TypeId
// atom, inlining the fss:local-ptr payloads, and re-hashing the reduced meta to
// a distinct flat id (docs/spec/02-encodings.md §"In-process pointer inlining").
//
// Offsets are the pinned tlm_generic_payload layout (SystemC 3.0.2, this ABI;
// vtable at 0, sizeof 136), obtained by probing the real object.  A peer
// compiled against a different SystemC has a different vtable/layout — but also
// a different runtime type, caught by the typeid== confirm — so a mismatch fails
// safe (no fast-path cast), never corrupts.  The planned successor to this hand
// table is a friend layout-helper on the GP class that derives the offsets.
//
// Real GP layout (offset : member : atom):
//    0  vptr                 core::TypeId        (vtable ptr; native-object marker)
//    8  m_address            bus::Locator        u64
//   16  m_command            bus::Operation      u32 enum
//   24  m_data               bus::Data           fss:local-ptr (m_length bytes)
//   32  m_length             bus::Length         u32
//   36  m_response_status    bus::Status         i32
//   40  m_dmi                bus::DmiAllowed      bool
//   48  m_byte_enable        bus::ByteEnable     fss:local-ptr (m_byte_enable_length)
//   56  m_byte_enable_length bus::ByteEnableLength u32
//   60  m_streaming_width    bus::StreamingWidth u32
//   64  m_gp_option          tlm::GpOption       u32 enum
// (m_extensions / m_mm / m_ref_count are NOT described — not serialisable; the
//  extension chain rides core::Extensions, a sub-meta excluded from the id.)

local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'tlm::transaction',
  atoms: [
    // Native-object marker: the vtable pointer.  Presence ⇒ live C++ object,
    // enabling the zero-copy fast path; non_serializable (a transport drops it).
    { atom: core.TypeId,          byte_offset:  0, byte_size: 8 },

    // Target physical address.
    { atom: bus.Locator,          byte_offset:  8, byte_size: 8 },

    // Operation code (Read=0, Write=1, Ignore=2).
    { atom: bus.Operation,        byte_offset: 16, byte_size: 4 },

    // Payload bytes (fss:local-ptr; m_length bytes outside the object).
    { atom: bus.Data,             byte_offset: 24,
      layout: 'fss:local-ptr',
      attrs:  { length_at: 'bus::Length' } },

    // Number of bytes in bus::Data.
    { atom: bus.Length,           byte_offset: 32, byte_size: 4 },

    // Response code (TLM tlm_response_status; Ok=1, Incomplete=0 initial).
    { atom: bus.Status,           byte_offset: 36, byte_size: 4 },

    // Target DMI hint (bool; 1 byte).
    { atom: bus.DmiAllowed,       byte_offset: 40 },

    // Byte-enable mask pointer (fss:local-ptr; ByteEnableLength bytes).
    { atom: bus.ByteEnable,       byte_offset: 48,
      layout: 'fss:local-ptr',
      attrs:  { length_at: 'bus::ByteEnableLength' } },

    // Byte-enable mask length (0 = no mask).
    { atom: bus.ByteEnableLength, byte_offset: 56 },

    // TLM-2.0 streaming width; equals Length for linear transfers.
    { atom: bus.StreamingWidth,   byte_offset: 60 },

    // TLM generic-payload `option` — a TLM memory-management artifact, NOT a
    // generic transactional field, so it is defined inline here (not a reusable
    // bus:: atom).  No meaning on b_transport; carried for faithful GP image.
    { atom: {
        qname:            'tlm::GpOption',
        version:          1,
        wire:             'uint',
        default_encoding: { MinPayload: 0, FullPayload: 1, FullPayloadAccepted: 2 },
        mutable_by:       'initiator',
        read_by:          'target',
        phase:            'request',
        description:      'TLM-2.0 generic-payload option; DMI/debug-transport only, no meaning on b_transport; carried for faithful GP reconstruction.',
      },
      byte_offset: 64, byte_size: 4 },

    // Extension hook (sub-meta): no data-region bytes; excluded from the
    // id hash per docs/spec/01-carrier.md §Identity.
    { atom: core.Extensions },
  ],
}
