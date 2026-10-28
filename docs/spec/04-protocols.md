# 04 — Protocols

A **protocol** is an ordered composition of atoms with an **id**
(the canonical-meta hash) and a behavioural specification.  Endpoints
attached to a transport channel exchange carriers matching one or
more protocols; receivers dispatch on the carrier's `id` field.

## Composition

A protocol's Jsonnet composition is the **source-side declaration**.  It
lists the atoms in the carrier with any protocol-specific layout
decisions; it does NOT duplicate the atom's intrinsic properties
(wire type, default size, mutability/role/phase) — those come from the
atom record, whether imported from a reference or independently agreed
vocabulary or defined inline (see [03-atoms.md](03-atoms.md)).  The `atom` field in
each entry is the full imported atom record; intrinsic data resolves at
Jsonnet compile time, so no separate registry lookup is needed by
downstream tools.

A composition entry MUST contain:

| key            | meaning                                                                                          |
|----------------|--------------------------------------------------------------------------------------------------|
| `atom`         | The full atom record from the imported barrel (e.g. `bus.Locator`), carrying `qname`, `version`, `wire`, `mutable_by`, `read_by`, `phase`, etc. |
| `byte_offset`  | Offset within the data region where this atom's bytes begin.                                     |

A composition entry MAY contain:

| key         | when to include                                                                                   |
|-------------|---------------------------------------------------------------------------------------------------|
| `byte_size` | REQUIRED for `bstr`, `tstr`, `uint`, and `int` (no implicit size; see [02-encodings.md](02-encodings.md) §CDDL profile).  A source declaration MAY omit it for fixed-width CDDL types or pointer layouts when the compiler can derive it; every transmitted data-field meta entry carries the resolved value.  MUST NOT be present on a `sub-meta` entry (it carries no data bytes). |
| `layout`    | In-process flat-layout override (e.g. `bus::Data` is a plain `bstr` in the reference vocabulary but `fss:local-ptr` in the in-process TLM composition), or `sub-meta` for an extension slot.  See [02-encodings.md](02-encodings.md) §FSS layout constructs. |
| `attrs`     | Layout-specific parameters (`length_at` for `fss:local-ptr`, etc.).                               |
| `encoding`  | Override the atom's `default_encoding` for this carrier's wire values, while retaining the shared meaning names.  The effective encoding (this field if present, else `default_encoding`) is recorded in the carrier meta and contributes to the carrier id (see [02-encodings.md](02-encodings.md) §Deferred enum encoding). |

The wire-format meta (defined in [01-carrier.md](01-carrier.md) §Entry shape) is the EXPANSION of the composition.  Each wire-format entry carries only the **physical** subset of the atom record — `qname`, `version`, `wire`, `byte_offset`, `byte_size`, and (where applicable) `encoding` / `layout` / `attrs` — layered with the per-protocol fields above.  The atom's documentation (`description`, `mutable_by`, `read_by`, `phase`) is dropped: it remains with the source atom record or vocabulary and may be associated by `qname` (see [01-carrier.md](01-carrier.md) §What the meta carries).  A meta-builder (the reference one is `reference/tools/fss_compile.py`) performs this expansion; both ends of a binding expand identically and therefore compute the same id.

```jsonnet
// known_carriers/tlm/transaction.jsonnet — the COMPLETE (live) carrier: the
// real in-memory layout of a tlm_generic_payload, so an in-process peer can take
// the zero-copy fast path (core::TypeId marks the vtable pointer).  The FLAT /
// portable wire form is NOT authored: a transport derives it at runtime by
// dropping the non_serializable core::TypeId atom, inlining the fss:local-ptr
// payloads, and re-hashing (see 02-encodings.md §core::TypeId, §Flat form).
local bus  = import '../../atoms/bus.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'tlm::transaction',
  atoms: [
    { atom: core.TypeId,          byte_offset:  0, byte_size: 8 },   // vtable ptr; non_serializable
    { atom: bus.Locator,          byte_offset:  8, byte_size: 8 },   // uint, 8 bytes
    { atom: bus.Operation,        byte_offset: 16, byte_size: 4 },   // uint, 4 bytes; default enc Read=0/Write=1/Ignore=2
    { atom: bus.Data,             byte_offset: 24,
      layout: 'fss:local-ptr',
      attrs:  { length_at: 'bus::Length' } },
    { atom: bus.Length,           byte_offset: 32, byte_size: 4 },   // uint, 4 bytes
    { atom: bus.Status,           byte_offset: 36, byte_size: 4 },   // int, 4 bytes; enc Ok=1/Incomplete=0/…
    { atom: bus.DmiAllowed,       byte_offset: 40 },
    { atom: bus.ByteEnable,       byte_offset: 48,
      layout: 'fss:local-ptr',
      attrs:  { length_at: 'bus::ByteEnableLength' } },
    { atom: bus.ByteEnableLength, byte_offset: 56 },
    { atom: bus.StreamingWidth,   byte_offset: 60 },
    // tlm::GpOption — TLM memory-management artifact; not a reusable bus:: atom;
    // no meaning on b_transport.  Defined inline (cf. 03-atoms.md §Sharing principle).
    { atom: {
        qname:            'tlm::GpOption',
        version:          1,
        wire:             'uint',
        default_encoding: { MinPayload: 0, FullPayload: 1, FullPayloadAccepted: 2 },
        mutable_by:       'initiator',
        read_by:          'target',
        phase:            'request',
        description:      'TLM-2.0 generic-payload option; DMI/debug-transport only.',
      },
      byte_offset: 64, byte_size: 4 },
    { atom: core.Extensions },             // sub-meta: no data bytes
  ],
}
```

## Magic id

A protocol's id is the carrier id (see [01-carrier.md](01-carrier.md))
computed from the canonical CBOR meta of the protocol's composition.
Both ends of a binding compute the same id independently from the
same composition: no registry, no negotiation.

The id remains stable under extension activity by the `core::Extensions`
mechanism (see *Extensions* below).

## Behavioural specification

A protocol's behaviour is specified at two scales:

- **Per-field**, via `mutable_by` / `read_by` / `phase` attributes.
  Per-field annotations (`mutable_by`, `read_by`, `phase`) are intrinsic
  to atoms (defined in [03-atoms.md](03-atoms.md)) — they describe
  single-carrier semantics: who writes what, when in a request/response
  cycle.  Protocols using these atoms inherit the annotations; they appear
   with their source atom records, not in carrier metas or a per-protocol override.
- **Cross-carrier**, in protocol prose with Mermaid sequence diagrams.
  Multi-message phase ordering, conditional flows, and choreography
  live in each protocol's `README.md` rather than the meta.

## Roles

The `initiator` and `target` roles are the two sides of an `attach` —
the same `fss_role` a transport takes (`FSS_ROLE_INITIATOR` /
`FSS_ROLE_TARGET`).  The
protocol's behaviour determines the roles: the side that constructs
and sends a carrier is its initiator; the side that mutates and
returns it is its target.  Symmetric protocols use `either` and
`both` and have no role asymmetry.

## Directionality

A protocol is **round-trip** iff its composition contains a
`mutable_by:'target'` atom — the target writes a value the initiator
reads back, which is precisely what requires a return path.  Otherwise
it is **one-way** (send-only).  This is a derived property of the
carrier.  The composition compiler or binder computes it from the atom
information when it plans the channel; it is not a per-channel semantic
configuration flag.  The ABI `round_trip` argument is the resulting
compatibility/provisioning hint: it can select a reply-capable medium, but the
transport receives neither atom semantics nor a semantic proof and MUST NOT
try to infer directionality from carrier ids, hints, or traffic.  It is not
semantic authority. The same predicate permits fan-out for one-way
protocols and forbids it for write-back protocols (see
[00-object-model.md](00-object-model.md) §Fan-in and fan-out). By this
rule the register-write carriers that carry `bus::Status`
(`tlm::transaction`, `fmireg::write`, `rustreg::write`), `config::param_request`
(`config::Status`), the bootstrap `control::plan` (`control::Status`), the
individual-step representation `control::instr` (`control::Status`), and
`sim::event` (`sim::Status`) are round-trip,
while the CAN frames (`sccan::frame`, `fmican::frame`, `rustcan::frame`,
`pycan::frame`), a `bus::Status`-less register-write (`pyreg::write`), and
`time::window` are one-way.

## Extensions: `core::Extensions`

`core::Extensions` is the canonical mechanism for hops along an FSS
path to attach information to a carrier without changing its id.  It is
a **sub-meta** (see [02-encodings.md](02-encodings.md)): there is one
carrier, one data region, and one meta.  An extension is a sub-meta — a
meta whose entries describe *further bytes in that same data region*.  It
carries no data of its own and no nested data pointer (it is NOT a nested
carrier).

### Inclusion (NORMATIVE)

`core::Extensions` is OPTIONAL — its presence in a composition is a
per-carrier choice with a clear convention:

- An **FSS-native carrier** (one this standard or a protocol built on it
  defines — e.g. `sim::event`, `config::param_request`,
  `tlm::transaction`) SHOULD include the `core::Extensions` slot, so that
  anything FSS itself defines is extensible by default.
- A **foreign-wrapping carrier** (one that maps a fixed external or
  third-party format onto a data region — e.g. a raw bus frame or a vendor
  struct) MAY omit it: there is no spare room in the foreign layout to
  carry an FSS extension, and the wrapper takes the bytes as given.

The bare `core::Extensions` marker affects identity: it is part of the base
composition and is included in the id hash (§Identity in
[01-carrier.md](01-carrier.md)). Its nested extension contents are excluded.
Therefore, a carrier hashes the same before or after extension content is
added to its existing marker, but a composition with the marker has a
different id from one without it.

### Adding an extension

A hop with, say, a 70-byte core carrier (data bytes `0..70`) attaches
information by:

1. Ensuring the data region has room for the new bytes (grow / realloc
   and copy if needed), writing them beyond the core — e.g. `71..100`.
2. Filling in a sub-meta whose entries describe those bytes (the same
   entry shape as the core meta: `qname`/`wire`/`byte_offset`/`byte_size`,
   with `byte_offset` into the *shared* region).

The sub-meta entries describe `71..100`; the core meta still describes
`0..70`. Extension content does not alter the carrier id, because the
already-present `core::Extensions` marker, rather than its nested contents,
defines the extensible base identity.

### Wrapping

A new extension typically nests the existing chain — its sub-meta
includes its own `core::Extensions` pointing at whatever extensions it
received from upstream:

```
core meta            → describes data[0..70]   (the carrier id)
└── core::Extensions  → sub-meta added by hop C  → describes data[71..85]
    └── core::Extensions  → sub-meta from hop B   → describes data[85..95]
        └── core::Extensions  → sub-meta from hop A → describes data[95..100]
```

The newest hop is outermost; the one you want is usually nearest the
top.  A hop that knows the existing chain MAY instead add a single flat
sub-meta combining several atoms.  A hop that adds nothing forwards the
carrier unchanged.

### Sealed protocols

A protocol that does NOT include `core::Extensions` in its composition
is **sealed**: hops MUST NOT attempt to add extensions to it.  Sealing is
the expected state of a foreign-wrapping carrier (§Inclusion) and a
deliberate choice for any FSS-native carrier that must not grow.

### Receiver behaviour

Dispatch is on the protocol id, which includes the bare extension marker but
is computed without its nested extension contents (see
[01-carrier.md](01-carrier.md) §Identity). After
dispatch, a receiver MAY walk the `core::Extensions` sub-meta (if
present) and recursively inspect inner extensions, reading their bytes
from the one data region.  Unknown extension atoms are skipped safely;
they remain in the carrier for downstream hops.

## Documentation convention

Protocol compositions are authored as Jsonnet (`.jsonnet`), importing
per-namespace barrel files from `atoms/` and referencing atoms
by field access (e.g. `bus.Locator`).  The `atom` field in each entry
holds the full atom record; intrinsic data resolves at compile time.

The reference vocabulary at `atoms/` is used by two kinds of carrier composition:

- **`protocols/`** — FSS-owned protocols where FSS defines both the carrier
  shape and the behavioural specification. Current protocols: `config/`
  (`config::param_request`, round-trip), `sim-events/` (`sim::event`,
  round-trip), `time-sync/` (`time::window`, one-way), and `control/`
  (`control::plan`, the round-trip bootstrap protocol that returns a build and
  wire plan; `control::instr`, the round-trip representation of one plan step;
  `control::Status` is `mutable_by:'target'`).
- **`known_carriers/`** — carriers for externally-defined protocols (IEEE, OSCI,
  vendor) where FSS provides only the atom composition and magic id.

Each carrier under `protocols/<name>/` or `known_carriers/<name>/` provides:

- `<name>.jsonnet` — the canonical composition.
- `README.md` — atom composition table with per-field annotations,
  Mermaid sequence diagram(s), and prose for any multi-carrier
  choreography.

Generated headers (e.g. `fss/known_carriers/tlm_id.h`,
`fss/protocols/sim.h`) are emitted by `reference/tools/fss_compile.py
--emit-c-header` and carry the carrier ID constant and canonical meta
bytes.  Hand-written field accessors (offsets against the composition)
are the intended approach; consumers use the known-id fast path (Path A)
or the meta resolver (Path B) as described in
[05-abi-conventions.md](05-abi-conventions.md) §Consuming a carrier's
fields.
