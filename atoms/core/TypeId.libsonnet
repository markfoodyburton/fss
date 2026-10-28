{
  qname:       'core::TypeId',
  version:     1,
  // Points at the C++ vtable pointer (8 bytes at the start of a polymorphic
  // object).  It is NOT a value to copy or use: its mere PRESENCE in a carrier
  // says "data_ptr references a LIVE in-memory C++ object" — a vtable sits here,
  // so an attempted dynamic_cast / typeid on data_ptr will find RTTI and can
  // verify the real type.  The atom carries no portable datum; the vptr value
  // is process-local (different per run / per .so), so it is non_serializable.
  //
  //   in_hash: true        — it defines the live carrier identity.  The
  //                          definitive same-type/same-ABI check is a runtime
  //                          typeid== through this vptr.
  //   serializable: false  — a wire transport DROPS this atom when it serialises
  //                          (the vptr is meaningless to a peer), then re-hashes
  //                          the reduced meta to a distinct flat carrier id.  A
  //                          receiver never casts flat bytes as a live object.
  //
  // A protocol does NOT need core::TypeId; without it a carrier is purely flat
  // bytes.  WITH it, an in-process peer that recognises the id may take the
  // zero-copy fast path (cast data_ptr to the live object) instead of walking
  // the meta — see the SystemC-TLM binding's tlm_generic_payload fast path.
  wire:        'uint .size 8',
  serializable: false,
  in_hash:       true,
  description: 'Native-object marker: points at the C++ vtable pointer (offset 0 of a polymorphic object).  Presence means data_ptr references a live in-memory C++ object whose real type is reachable via RTTI (dynamic_cast/typeid) through this vptr; it carries no portable value (non_serializable) and is excluded from a serialised carrier.  It defines the live carrier identity; flattening re-hashes without it.  Enables the in-process zero-copy fast path; a protocol without it is purely flat bytes.  See docs/spec/02-encodings.md §core::TypeId.',
  mutable_by:  'neither',
  read_by:     'both',
  phase:       'both',
}
