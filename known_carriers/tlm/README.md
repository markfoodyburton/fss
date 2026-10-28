# tlm/ — TLM carrier shape

Read/write memory-mapped transactions over FSS.  Carrier shape for the
TLM-2.0 base protocol (IEEE 1666.1 §6); FSS packages the carrier without
owning the protocol design.

## Canonical definition

The canonical carrier definition is [`transaction.jsonnet`](transaction.jsonnet) plus the
sequence diagram in this README.  There is no hand-written C accessor: in
SystemC the carrier id is recognised and the live `tlm_generic_payload`
is used natively; other consumers resolve fields by qname from the carrier
meta at runtime.  `CMakeLists.txt` generates only the id + CBOR meta
(`fss/known_carriers/tlm_id.h`) from the composition.

## Atom composition

This is the COMPLETE (live) carrier — the real in-memory layout of a
`tlm_generic_payload`, so an in-process peer can take the zero-copy fast path
(`core::TypeId` marks the vtable pointer at offset 0):

| Offset | Size | Atom                    |
|--------|------|-------------------------|
| 0      | 8    | `core::TypeId` (vtable ptr; native-object marker, `non_serializable`) |
| 8      | 8    | `bus::Locator`          |
| 16     | 4    | `bus::Operation` (`Read=0, Write=1, Ignore=2`) |
| 24     | 8    | `bus::Data` (local-ptr) |
| 32     | 4    | `bus::Length`           |
| 36     | 4    | `bus::Status` (signed `tlm_response_status`) |
| 40     | 1    | `bus::DmiAllowed`       |
| 48     | 8    | `bus::ByteEnable` (local-ptr) |
| 56     | 4    | `bus::ByteEnableLength` |
| 60     | 4    | `bus::StreamingWidth`   |
| 64     | 4    | `tlm::GpOption` (inline; not a generic `bus::` atom) |
| —      | —    | `core::Extensions` (sub-meta; no data bytes) |

(Offsets are the pinned `tlm_generic_payload` layout — vtable at 0, `sizeof` 136;
`m_extensions`/`m_mm`/`m_ref_count` are not described, not serialisable.)

Per-field mutability, role, and phase are intrinsic to each atom; see [`atoms/`](../../atoms/) for the atom catalogue.

Atom definitions: [`atoms/bus/`](../../atoms/bus/) and
[`atoms/core/`](../../atoms/core/).

`bus::Data` uses layout `fss:local-ptr` in-process (declared in
`transaction.jsonnet` as a per-entry layout override; the atom's registry wire type
is `bstr`).
`core::Extensions` permits hops to wrap the carrier without changing
the carrier id; it is a sub-meta (nested meta over the one data
region; no data bytes of its own) (see
[docs/spec/04-protocols.md](../../docs/spec/04-protocols.md) §Extensions).

### Live form vs flat form

This authored carrier is the **live** form: `data_ptr` references a real
`tlm_generic_payload`, and `core::TypeId` marks the vtable so an in-process peer
that recognises the carrier id may cast `data_ptr` directly (zero-copy) — the
binding confirms with a runtime `typeid==` through the vptr.  The **flat**
(portable) form is **not authored**: a wire transport derives it at runtime by
dropping the `non_serializable` `core::TypeId` atom, inlining the `fss:local-ptr`
payloads, and re-hashing — yielding a distinct flat carrier id.  See
[docs/spec/02-encodings.md](../../docs/spec/02-encodings.md) §core::TypeId.

The carrier id is the FNV-1a-64 of the canonical CBOR meta of this
composition, with the sub-meta exclusion rule applied
(see [docs/spec/01-carrier.md](../../docs/spec/01-carrier.md) §Identity).

## Sequence

```mermaid
sequenceDiagram
    participant Initiator
    participant Target

    Note over Initiator: build carrier:<br/>fill Locator, Operation, Length,<br/>Data (if Write); leave Status undefined
    Initiator->>Target: send carrier
    Note over Target: read Locator, Operation, Length;<br/>execute the transaction;<br/>write Data (if Read), write Status
    Target->>Initiator: same carrier (mutated)
    Note over Initiator: read Status; read Data (if Read)
```

Single round-trip; mutable fields are written on the return path.
No queueing at the carrier level — queueing is the implementation's
concern (e.g., the SystemC artefact wrapper queues and dispatches
on the kernel thread).
