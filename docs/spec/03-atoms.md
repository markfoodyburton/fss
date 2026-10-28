# 03 — Atoms

An **atom** is a named, versioned semantic field shape — a reusable
building block for protocols.  Atoms sit between the flat-layout
profile (mechanical byte interpretation, see [02-encodings.md](02-encodings.md))
and protocols (atom compositions, see [04-protocols.md](04-protocols.md)).

Atoms are the **logical** layer of FSS: an atom names a semantic *role*
(what a field *means* — a locator, an operation, a length) independent
of how it is laid out in bytes.  The **physical** layer is the carrier
composition (see [04-protocols.md](04-protocols.md)), which pins each
atom to a concrete `byte_offset`, `byte_size`, and — for enum atoms —
an effective wire `encoding`.  One logical role can therefore appear in
many physical layouts: `bus::Operation` means Read/Write/Ignore whether
it is a 4-byte TLM field or a 1-byte CAN field.  This split is what lets
a TLM carrier and a CAN frame share one vocabulary and be bridged.

The **normative mechanism** here is the atom *definition shape* (the
object literal structure defined below in §Atom definition) and the
*qname discipline*: use shared qnames consistently; a qname appears at most
once per carrier meta.

The **atom set** under [`atoms/`](../../atoms/) is a versioned reference
vocabulary.  It aids reuse, but Tier-1 does not mandate a global registry,
qname namespace, or registry lookup.  Interoperating parties must preserve the
qnames they agree to place in a meta.

## Atom definition

Atom definitions are authored as Jsonnet (`.libsonnet`).  Each
file evaluates to a single object whose fields match the
definition shape below.  Jsonnet is a JSON-superset configuration
language with imports and value references; running
`jsonnet <file>` resolves all imports and emits standard JSON
for downstream tooling (validators, code generators).

Each atom record is an instance of the following shape:

```jsonnet
{
  qname:             '<namespace>::<Name>',  // string
  version:           1,                       // integer; bumped on incompatible change
  wire:              '<CDDL fragment>',       // flat-layout profile; see 02-encodings.md
  description:       '<prose>',
  default_encoding:  { Name: 0, Other: 1 },  // optional; enum atoms only (see 02-encodings.md §Deferred enum encoding)
  layout:            '<fss-layout>',          // optional; an FSS construct CDDL can't express
  serializable:      true,                    // optional, default true; false ⇒ a wire transport drops this atom and re-hashes (see 02-encodings.md §Atom attributes), e.g. core::TypeId
  in_hash:           true,                    // optional, default true; false ⇒ excluded from the carrier id hash (transmitted but not identity-defining)
  mutable_by:        'initiator' | 'target' | 'either' | 'neither',
  read_by:           'initiator' | 'target' | 'both',
  phase:             'request' | 'response' | 'both',
}
```

Mutability, role, and phase are intrinsic properties of the atom — they
describe how the atom is **always** used.  Protocols using an atom inherit
these values; they are not specified per-protocol unless a protocol
genuinely needs to override (which is rare and indicates the atom may be
misused).

- `qname` identifies the atom within the vocabulary or composition that
  defines it.  Parties using qname-based field resolution agree on its
  meaning; the transmitted meta remains sufficient to locate and decode the
  field physically.
- `version` is bumped when semantics change incompatibly.  Type
  (`wire`) changes and width changes are always incompatible.
- `wire` is the atom's type, authored as a CDDL fragment from the
  closed flat-layout profile in [02-encodings.md](02-encodings.md):
   `'uint'` (width-agnostic unsigned; `byte_size` REQUIRED in the transmitted
   meta), `'uint .size 8'` (fixed 64-bit), `'bstr'` (variable bytes),
  `'tstr'` (UTF-8 string), and so on.  It is the single authored source of the
  field's type; the compiler derives byte width and wire-meta type
  from it.  FSS adds no parallel type vocabulary.
- `layout` is optional and names an FSS construct CDDL cannot express —
  `'sub-meta'`, `'fss:local-ptr'`, `'fss:local-ptr-nul'` (see
  [02-encodings.md](02-encodings.md)).  Most atoms omit it; the byte
  mapping then follows from `wire`.  `core::Extensions` sets
  `layout: 'sub-meta'` and has **no `wire`** — it carries a nested meta,
  not data-region bytes.  The pointer layouts are normally a per-protocol
  override, not an atom-intrinsic value.
- `default_encoding` is optional; present only for enum atoms.  It is a
  Jsonnet object from symbolic meaning name to canonical wire integer value
  (see [02-encodings.md](02-encodings.md) §Deferred enum encoding).  A
  composition entry MAY override with `encoding: { ... }` to match a foreign
  struct's byte values while retaining the shared meaning vocabulary.

- `mutable_by` / `read_by` / `phase` are intrinsic to the atom.  Most
  atoms have a fixed role (e.g., an address is always set by the initiator,
  a status is always written by the target).  Protocols inherit these
  values from the atom registration.  They are atom **documentation**,
  resolved by `qname` — they are NOT part of the transmitted carrier meta
  (see [01-carrier.md](01-carrier.md) §What the meta carries).  Likewise
  `description` never travels.

## Naming

Qnames use the form `namespace::Name`:

- **`namespace`** is lowercase; multi-word names use hyphens
  (`tlm-events::`).  The namespace `core::` is reserved for atoms
  defined by the FSS standard itself.
- **`Name`** is CamelCase and expresses the atom's semantic *role*, not
  its wire width.  Width is a per-carrier property of width-agnostic
  atoms (`uint`/`int`), not part of the name.
- The separator is `::` (C++/Rust namespace style).

Qname strings identify fields in carrier metas.  Parties that use qname-based
field resolution must produce the same strings in the canonical meta.

### Multiple fields of the same role

A qname appears **at most once** per carrier meta: a field is resolved by
qname (Path B; see [05-abi-conventions.md](05-abi-conventions.md)), so two
entries sharing a qname would be indistinguishable.  When a protocol
genuinely needs two fields of the same role — say a general and a specific
locator — give each a **qualified qname** of the form
`namespace::Role::qualifier`, e.g. `bus::Locator::general` and
`bus::Locator::specific`.  The leading `namespace::Role` still names the
shared role (so a tool keyed on `bus::Locator` can recognise both by
prefix); the trailing qualifier names the instance and is itself
documentation of *which* one it is.  This needs no new mechanism — each
qualified qname is a distinct, unique identifier, resolved exactly like any
other.

## Reference vocabulary layout

The reference vocabulary is published under [`atoms/`](../../atoms/):

```
atoms/
├── README.md                — namespace catalogue
├── core.libsonnet           — namespace barrel (imports all atoms in core/)
├── core/
│   ├── README.md            — human-readable catalogue (links to per-atom files)
│   ├── Extensions.libsonnet — one file per atom
│   └── TypeId.libsonnet     — native-object marker (see 02-encodings.md §core::TypeId)
├── bus.libsonnet
├── bus/
│   ├── README.md
│   ├── Locator.libsonnet
│   ├── Operation.libsonnet
│   ├── Length.libsonnet
│   ├── Data.libsonnet
│   ├── Status.libsonnet
│   ├── StreamingWidth.libsonnet
│   ├── ByteEnable.libsonnet
│   ├── ByteEnableLength.libsonnet
│   └── DmiAllowed.libsonnet
├── config.libsonnet
├── config/
│   └── ...
├── sim.libsonnet
└── sim/
    └── ...
```

Each namespace directory contains a markdown `README.md` (catalogue
with links to per-atom files for human readers) and **one `.libsonnet` file
per atom** (file name matches the atom's leaf name).  The namespace barrel
`<namespace>.libsonnet` at the `atoms/` level imports every
per-atom file in the namespace.

## Adding a reference atom

1. Pick a namespace (or create one).
2. Choose qname, `wire` type, version (start at 1).
3. Create `atoms/<namespace>/<LeafName>.libsonnet` containing
   the atom object literal with all required fields.
4. Add an entry to the namespace barrel
   `atoms/<namespace>.libsonnet`:
   `LeafName: import '<namespace>/<LeafName>.libsonnet',`.
5. Add a row to the namespace's `README.md` catalogue with a link to
    the new file.

These steps apply only when contributing to the reference vocabulary.  A
composition MAY instead use an atom record from a vocabulary independently
agreed by its parties, or define the record inline.  Tier-1 requires the
record shape and the resulting carrier meta; it does not require publication
in `atoms/`.

## Sharing principle

A field SHOULD reuse an existing shared role atom rather than define a
near-duplicate.  The shared meaning vocabulary — `bus::Locator`,
`bus::Operation`, `bus::Length`, `bus::Data`, etc. — is the interop key:
a generic consumer or bridge resolves a field by qname across protocols,
and tools that understand `bus::Locator` work on any carrier that carries
it, regardless of wire width or encoding.

**When to share:** an atom SHOULD be reused when the **role** and the
**meaning vocabulary** match.  Wire width (`byte_size`) and wire encoding
(`encoding`) MAY differ per carrier — those are deferred to the
composition, not the atom definition.

**When to define inline:** a field that is genuinely protocol-specific and
not a reusable semantic concept MUST be defined **inline** in its carrier
composition (the `atom:` value is an object literal), rather than added to
the reference vocabulary.  Examples:

- `tlm::GpOption` — a TLM memory-management artefact with no meaning on
  `b_transport`; defined inline in `known_carriers/tlm/transaction.jsonnet`.
- `can::Ide` — the CAN identifier-extension flag (Standard/Extended); defined
  inline in `known_carriers/can/frame.jsonnet`.

Consequently, the CAN frame composition adds **no** shared reference-vocabulary atoms:
its semantic fields are `bus::{Locator, Operation, Length, Data}` plus inline
`can::Ide` and `core::Extensions`.

## Versioning

Atom versions are independent across atoms.  Protocol compositions
pin specific atom versions; bumping an atom's version creates new
protocol ids for every composition that uses it.  This is how
incompatibility is signalled: a receiver expecting version N of an
atom will not match a sender producing version N+1.
