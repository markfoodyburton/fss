# 01 — Carrier

The **carrier** is the unit of FSS communication: a 40-byte header
referencing two byte regions — `data` (the payload) and `meta`
(a self-describing layout descriptor).

## Header

```c
typedef struct fss_carrier {
    uint64_t  id;
    void     *data_ptr;
    uint64_t  data_size;
    void     *meta_ptr;
    uint64_t  meta_size;
} fss_carrier_t;
```

`id` is at offset 0 so receivers can dispatch with one 8-byte load.
`data_ptr` and `meta_ptr` carry pointers in-process and offsets on
the wire — the same byte layout, two interpretations.

## Data region

`data_size` bytes pointed at by `data_ptr`.  The carrier mechanism
imposes no structure on this region; the meta describes which bytes
mean what.

## Meta region

`meta_size` bytes of canonical CBOR (RFC 8949 §4.2 deterministic
encoding) at `meta_ptr`.  The meta is a CBOR array; each element is
a CBOR map describing one atom's instance in the data region.

### Entry shape

Most entries describe one field in the carrier's single data region —
a **data-field entry**.  Such an entry MUST contain:

| key            | CBOR type | meaning                                                                            |
|----------------|-----------|------------------------------------------------------------------------------------|
| `qname`        | tstr      | Atom identifier (e.g. `"bus::Locator"`).  See [03-atoms.md](03-atoms.md).       |
| `version`      | uint      | Atom version (per its registration).                                               |
| `wire`         | tstr      | CDDL type fragment from the flat-layout profile in [02-encodings.md](02-encodings.md) (e.g. `"uint .size 8"`, `"bstr"`). |
| `byte_offset`  | uint      | Offset within the data region where this atom's bytes begin.                       |
| `byte_size`    | uint      | Number of bytes occupied in the data region.                                       |

One entry kind is the exception: a **sub-meta entry** (`layout` =
`"sub-meta"`, e.g. `core::Extensions`) carries a nested meta, not data.
It MUST contain `qname`, `version`, and `layout`; it MUST NOT contain
`wire`, `byte_offset`, or `byte_size`; and it MAY contain `submeta`
(the nested meta — a CBOR array of further entries describing more of
the **same** data region).  See [02-encodings.md](02-encodings.md)
§FSS layout constructs and [04-protocols.md](04-protocols.md)
§Extensions.

A data-field entry MAY also contain these optional keys:

| key            | CBOR type | meaning                                                                       |
|----------------|-----------|-------------------------------------------------------------------------------|
| `encoding`     | map       | Enum atoms only: the **effective** symbolic-name → wire-value map for this field (see [02-encodings.md](02-encodings.md) §Deferred enum encoding).  Present whenever the atom is an enum; it is the *physical* encoding this carrier uses, not the atom's default. |
| `layout`       | tstr      | FSS layout construct — `"fss:local-ptr"` / `"fss:local-ptr-nul"` (see [02-encodings.md](02-encodings.md)).  Absent ⇒ the `wire` type's natural byte mapping applies.  (`"sub-meta"` marks the sub-meta entry above, not a data field.) |
| `attrs`        | map       | Layout-specific parameters (e.g. `length_at` for `fss:local-ptr`).            |
| `serializable` | bool      | Present only when `false`: a wire transport drops this atom and re-hashes (see [02-encodings.md](02-encodings.md) §Atom attributes).  Absent ⇒ `true`. |
| `in_hash`      | bool      | Present only when `false`: transmitted but excluded from the carrier id (see [02-encodings.md](02-encodings.md) §Atom attributes, and §Identity below).  Absent ⇒ `true`. |

The sub-meta entry additionally MAY carry `submeta` (the nested meta, a
CBOR array of entries).

### What the meta carries — and what it does not (NORMATIVE)

The meta is the **physical** description of *this carrier's* bytes: where
each field sits (`byte_offset` / `byte_size`), how to decode it (`wire`,
`layout`, `attrs`), what its enum values mean here (`encoding`), and which
atom it is (`qname` / `version`).  A receiver can locate and decode every
field from the meta alone.

The atom's **documentation** — its prose `description` and its intrinsic
role/phase annotations (`mutable_by`, `read_by`, `phase`; see
[03-atoms.md](03-atoms.md)) — is NOT part of the meta and is never
transmitted.  It may live in a reference or independently agreed vocabulary
and be associated by `qname`.  This holds even for an atom defined inline in a composition: only the
field's physical description travels, not its prose.  Keeping the meta to
the physical essentials is what stops a carrier description from expanding
into the full atom records it references.

A transmitted data-field entry's `byte_size` is always present; the compiler
(e.g. `fss_compile.py`) resolves it from the source-side declaration — from
the `wire` type's width for fixed-size CDDL (`uint .size N`, `float64`,
`bool`, …), or as the implicit 8 for `fss:local-ptr` / `fss:local-ptr-nul`
(= sizeof(void*) on 64-bit).  `bstr` and `tstr` are variable-width and
MUST carry an explicit `byte_size`.  (Sub-meta entries carry no
`byte_size` — they are not data fields.)

Protocol authors typically write a more compact **source-side
declaration** (a Jsonnet composition under `protocols/<name>/`) that uses
atom records from a reference or independently agreed vocabulary, or inline
records.  A meta-builder expands this declaration using those records (see
[03-atoms.md](03-atoms.md)) to produce
the wire-format meta described here.  The expansion is deterministic:
both ends of a binding produce byte-identical wire-format metas from
the same composition + registry, and therefore compute the same id.
See [04-protocols.md](04-protocols.md) for the source-side shape.

Entries are explicitly self-describing: **position in the meta array
does NOT determine layout**.  Atoms may be reordered or padded; layout
is governed by `byte_offset` + `byte_size`.  Two implementations that
derive both halves of an FSS link from the same atom composition
typically produce byte-identical metas, but the spec only requires
that they hash identically (see below).

## Identity

The carrier id is a 64-bit hash of the canonical CBOR meta_bytes:

```
id = hash(canonicalize(meta_bytes))
```

The hash function is not fixed by this draft.  **FNV-1a-64 is a working
candidate** for the reference profile.  Parties that exchange ids must agree
on a hash profile and MUST treat an id as opaque except for equality comparison.

### Sub-meta contents exclusion (NORMATIVE)

When hashing a meta, a sub-meta entry participates in the hash as its
ordinary entry fields (`qname`, `layout`, and any other applicable entry
attributes), but its `submeta` value is **excluded**. The rule applies at
every nesting level. This includes `core::Extensions`.

Thus, the bare `core::Extensions` marker is part of the base carrier
identity: a carrier composition with the marker has a different id from one
without it. Its nested extension contents (see
[04-protocols.md](04-protocols.md)) do not perturb that id. An extensible
protocol therefore hashes the same before and after nested extension content
is added, but not before and after the marker is added. Without the contents
exclusion, extension activity would change the id and break
content-addressed dispatch.

### `in_hash` exclusion (NORMATIVE)

An entry with the attribute `in_hash: false` (see
[02-encodings.md](02-encodings.md) §Atom attributes) is **excluded from
the hash entirely** — treated as absent — even though it is still
transmitted in the meta.  This is the per-atom generalisation of the
sub-meta rule: it lets an atom be carried without making it
identity-defining (e.g. an extension field whose presence must not change
the carrier id).  So the id is the hash of the meta with every
`in_hash: false` entry dropped and every remaining sub-meta value removed.

(A `serializable: false` atom — e.g. `core::TypeId` — remains
identity-defining unless its `in_hash` attribute excludes it.  See
[02-encodings.md](02-encodings.md) §core::TypeId.)

## Bind / send

An **endpoint** (see [05-abi-conventions.md](05-abi-conventions.md))
is a `{self, receive, send, send_ctx}` quad.  `receive` is the inbound
delivery callback, installed by the artefact that owns the endpoint.
`send` / `send_ctx` are the outbound channel, installed by the transport
at `attach` time (as an initiator).

A transport's `attach(ch, local, INITIATOR)` installs the outbound
channel on `local` — `local->send` and `local->send_ctx`.
For an in-process 1:1 channel this is the identity wiring
`local->send = peer->receive; local->send_ctx = peer->self`, so the
transport leaves the steady-state path entirely.

Thereafter a model delivers one carrier with the installed callback —
`local->send(local->send_ctx, carrier)` — a single indirect call, no
destination argument and no per-send lookup.  Carriers are mutable:
a receiver MAY write a field whose atom role permits it (`mutable_by`; as
documented by the applicable atom record).  For round-trip protocols
the same carrier returns to the sender with mutated bytes; for one-way
protocols the sender does not observe the carrier after dispatch.  A
`send` of `NULL` means the endpoint is unbound (`FSS_SEND_UNBOUND`).

For occasional traffic with no standing attach — e.g. lifecycle pings to
the `fss/sim` standard endpoint — a transport also offers
`send_to(ch, carrier)`: a one-shot deliver-to-channel that installs
nothing.  There is no `transport->send` verb; attached-channel delivery
is the installed callback, not a transport call.  See
[05-abi-conventions.md](05-abi-conventions.md) for the full ABI.
