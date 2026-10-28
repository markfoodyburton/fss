# 02 — Field types (CDDL flat-layout profile)

A carrier's data region is a flat byte buffer.  Each atom occupies a
fixed `[byte_offset, byte_offset + byte_size)` span.  This document
defines how a field's **type** — authored as a CDDL fragment in the
atom's `wire` field (see [03-atoms.md](03-atoms.md)) — maps to those
bytes, plus the three flat-layout constructs CDDL cannot express.

FSS reuses CDDL (RFC 8610) for the data-model type and CBOR (RFC 8949)
for the meta; it does not invent a type vocabulary.  CDDL describes
values, not byte layout (RFC 8610 §2.2.3 — *"CDDL does not provide any
language means to restrict the choice of serialization variants"*).
The byte mapping below is the FSS contribution.

## CDDL profile

An atom's `wire` MUST be one of the following CDDL fragments.  The set
is closed: a conforming compiler rejects anything else.

| `wire` (CDDL)   | bytes                    | data-region interpretation                                                               |
|-----------------|--------------------------|------------------------------------------------------------------------------------------|
| `uint`          | `byte_size` (REQUIRED)   | unsigned integer, little-endian; width fixed by the composition entry                   |
| `int`           | `byte_size` (REQUIRED)   | signed integer, two's-complement, little-endian; width fixed by the composition entry   |
| `uint .size N`  | N                        | unsigned integer, little-endian; width fixed                                             |
| `int .size N`   | N                        | signed integer, two's-complement, little-endian; width fixed                            |
| `float32`       | 4                        | IEEE 754 binary32, little-endian                                                         |
| `float64`       | 8                        | IEEE 754 binary64, little-endian                                                         |
| `bool`          | 1                        | `0` is false; any non-zero byte is true                                                  |
| `bstr`          | `byte_size` (REQUIRED)   | opaque byte sequence                                                                     |
| `tstr`          | `byte_size` (REQUIRED)   | UTF-8; need not be NUL-terminated                                                        |

**Width-agnostic role types.** Plain `uint` and `int` (without `.size`) are
**width-agnostic**: the byte width is not fixed by the atom but is REQUIRED in
the composition entry's `byte_size`.  This lets one shared role atom serve
different widths per carrier — for example `bus::Locator` is `byte_size 8` in
TLM (a 64-bit memory address) and `byte_size 4` in a CAN frame (a 32-bit
message id), under the same qname.  Use `uint .size N` / `int .size N` only
when the width is an intrinsic property of the field, not of the protocol.

`.size N` constrains an integer's value range in CDDL (RFC 8610 §3.8.1);
FSS additionally fixes `N` as the field's exact width in the data region
(the idiom of RFC 8610's own `audio_sample = uint .size 3` example).
Every transmitted data-field meta entry MUST carry an explicit `byte_size`.
For fixed-width types, a source composition may omit it and the compiler emits
the derived value.

## Deferred enum encoding

An atom MAY declare `default_encoding: { Meaning: wire_value, ... }` — the
shared *meaning vocabulary* plus a canonical wire encoding.  A composition
entry MAY override with `encoding: { ... }` to match a foreign struct's exact
byte values while keeping the shared meaning names.  The **effective encoding**
(override if present, else `default_encoding`) is recorded in the carrier
meta — making the carrier self-describing — and therefore contributes to the
carrier id.

Two carriers that share an atom's meaning names but differ in wire encoding are
still bridgeable: a bridge translates through the meaning names without loss.

Example: `bus::Operation` declares `default_encoding: { Read: 0, Write: 1,
Ignore: 2 }`.  The CAN frame composition overrides to `encoding: { Write: 0,
Read: 1 }` to match the FMI-LS-BUS rtr byte value, while retaining the
`Read`/`Write` meaning vocabulary shared across protocols.

> **Reading the effective encoding.** A composition entry shows an
> `encoding:` only when it *overrides* the default, so a carrier that
> inherits (e.g. TLM keeps `bus::Operation`'s default) shows nothing in
> its `.jsonnet`.  The effective encoding is never absent from the
> *compiled meta*, though: the compiler always writes it in.  To see the
> resolved layout + encoding for any shape, run
> `reference/tools/fss_compile.py <shape>.jsonnet` — it prints each field's offset,
> width, and effective `enc={…}`.  The source file is the delta from the
> atom's defaults; the compiled meta is the full truth.

## FSS layout constructs

Three constructs have no CDDL spelling.  They are named in the meta
entry's `layout` field (see [01-carrier.md](01-carrier.md) §Entry
shape), never in `wire`.

**`sub-meta`** is *not a data field*.  It is a meta-structural slot
whose value is a nested meta — a "sub-meta": a complete meta whose
entries describe further bytes in the carrier's **single** data region.
A sub-meta entry occupies no data-region bytes of its own; it has no
`wire`, `byte_offset`, or `byte_size`.  The bare sub-meta marker (for
example, `core::Extensions`) participates in the parent meta's base
identity; only its nested `submeta` content is excluded from that identity
(see [01-carrier.md](01-carrier.md) §Identity).  Therefore, metas with the
marker absent and present have different base ids, while metas with the same
marker but different nested content share a base id (unless other
identity-defining content differs).
`core::Extensions` is the canonical sub-meta; see
[04-protocols.md](04-protocols.md) §Extensions.

**`fss:local-ptr` / `fss:local-ptr-nul`** are in-process data-field
layouts: an 8-byte pointer to bytes *outside* the data region, valid
only when sender and receiver share an address space.  They override
the in-process byte representation of a field whose portable `wire`
type is `bstr` / `tstr`.

| `layout`             | bytes | wire   | interpretation                                                                  |
|----------------------|-------|--------|---------------------------------------------------------------------------------|
| `sub-meta`           | —     | (none) | nested meta describing more of the one data region; carries no data bytes; the marker is identity-defining, but its nested content is excluded. |
| `fss:local-ptr`      | 8     | `bstr` | pointer to bytes outside the data region; the companion atom named by `attrs.length_at` holds the length. |
| `fss:local-ptr-nul`  | 8     | `tstr` | as `fss:local-ptr`, but the pointed-to bytes are NUL-terminated.                |

## In-process pointer inlining

`fss:local-ptr` / `fss:local-ptr-nul` are valid only when sender and
receiver share an address space.  A wire transport that encounters one
MUST inline the pointed-to bytes, drop the `layout` field, and set
`byte_size` to the inlined length; the field reverts to its `wire` type
(`bstr` / `tstr`).  Inlining changes the carrier id — the wire form and
the in-process form are distinct carriers.

## Atom attributes

Each meta entry has two boolean attributes that govern serialisation and
identity.  Both **default to true** and are emitted into the meta only when
opted out, so a plain field carries neither and existing carrier ids are
unaffected.

| attribute | default | meaning when **false** |
|-----------|---------|------------------------|
| `serializable` | `true` | A wire transport **drops this atom** when it serialises the carrier, then re-hashes the reduced meta.  Used for process-local atoms such as `core::TypeId` and native-handle slots.  A non-serialisable atom remains identity-defining unless also `in_hash: false`. |
| `in_hash`      | `true` | The atom is **excluded from the carrier-id hash** — transmitted, but its presence/value does not change the id. Used for nested or other non-identity runtime content, e.g. a TLM extension within `core::Extensions`. It does not apply to the bare `core::Extensions` marker: that marker remains identity-defining, while its nested `submeta` contents do not. |

The two are orthogonal:

| atom kind | `in_hash` | `serializable` |
|-----------|-----------|----------------|
| ordinary field (`bus::Locator`) | true | true |
| pointer field (`bus::Data`, `fss:local-ptr`) | true | true (inlined — see above) |
| TLM extension | **false** | true |
| `core::TypeId` (native-handle marker) | true | **false** |

### `core::TypeId` and the live-vs-flat distinction

#### Atom definition

`core::TypeId` is a process-local `core::` atom with:

| field | value |
|---|---|
| `wire` | `uint .size 8` |
| `serializable` | `false` |
| `in_hash` | `true` (default) |

(`byte_offset` is not an atom field — it is set per composition.  A composition
that uses `core::TypeId` MUST place it at `byte_offset: 0`: the vtable pointer
occupies the first 8 bytes of every polymorphic C++ object.)  Its
`wire: 'uint .size 8'` is the physical
description — the field covers those 8 bytes — but the **value is process-local
and uninterpretable outside the originating process**.  It is not a datum to copy,
compare numerically, or transmit; its purpose is purely structural (see below).

#### Semantics

The **mere presence** of `core::TypeId` in a carrier meta means:

> `data_ptr` references a live in-memory C++ polymorphic object; a vtable sits
> at offset 0, so the real dynamic type is reachable via RTTI (`dynamic_cast` /
> `typeid`) through that vptr.

No value is being asserted.  The atom carries no portable datum; its role is to
mark the live form as distinct from the flat form.

A protocol does **not** need `core::TypeId`.  Without it, a carrier is purely
flat bytes — self-contained, portable, processable by any receiver that can
parse the meta.  Adding `core::TypeId` opts into the in-process live-object
fast path at the cost of restricting the carrier to in-process delivery.

#### Live id vs flat id

Because `core::TypeId` is `in_hash: true`, its presence changes the carrier id:

```
live_id  = hash(meta_with_TypeId)
flat_id  = hash(meta_without_TypeId)   ≠ live_id
```

These are distinct identifiers for the two forms under the selected hash
profile.  A receiver checking `carrier->id == known_live_id` is therefore
asking "is this the live form of the protocol I know?" — without touching
`data_ptr`.  The id check alone is the discrimination gate; it requires no
dereference and no meta parse.

#### Zero-copy fast path

When a receiver confirms `carrier->id == known_live_id`, it MAY take the
**zero-copy fast path**: cast `data_ptr` directly to the known live type and
read/write its fields natively, bypassing the meta-resolver entirely.

Example: the SystemC-TLM binding casts `data_ptr` to `tlm_generic_payload*`
when `carrier->id == FSS_TLM_ID` (the live id).  The cast is valid because the
id equality proves both ends compiled against the same carrier composition
(same atom layout) and the same C++ ABI (same vtable layout).

#### Safe discrimination (NORMATIVE)

Id equality is a necessary condition but not sufficient on its own in a
multi-library deployment.  Two shared libraries compiled against different
versions of the same type can coincidentally agree on the carrier shape (same
atom layout ⇒ same hash) while disagreeing on the C++ ABI (different vtable
offsets, different RTTI pointers).  A receiver MUST therefore apply **both**
gates before casting:

1. **Id check** — `carrier->id == known_live_id`.  A cheap, non-dereferencing
   equality test.  This is the primary dispatch gate.

2. **Runtime type check** — `typeid(*static_cast<T*>(data_ptr)) == typeid(T)`.
   Confirms that the vptr at offset 0 leads to the expected RTTI record — the
   definitive same-type / same-ABI test.  This gate catches the dangerous case
   where two separately compiled libraries share the same carrier shape but
   different `T` layouts.

Both gates MUST pass before the receiver treats `data_ptr` as a live `T*`.
A receiver that passes only gate 1 and skips gate 2 is not conforming with
respect to the zero-copy path.

If either gate fails, the receiver MUST fall back to the meta-resolver path
(Path B, [05-abi-conventions.md](05-abi-conventions.md) §Consuming a
carrier's fields) or reject the carrier.

#### Flat form: the wire transport's product (NORMATIVE)

A wire transport that encounters a carrier bearing `core::TypeId` (or any
`fss:local-ptr` field — see §FSS layout constructs) MUST produce the **flat
form** before transmitting:

1. Drop the `core::TypeId` entry from the meta (and any other
   `serializable: false` entries).
2. For each `fss:local-ptr` / `fss:local-ptr-nul` field, inline the
   pointed-to bytes into the data region and update the entry to its plain
   `bstr` / `tstr` `wire` type with the inlined `byte_size`.  See
   §In-process pointer inlining.
3. Re-hash the reduced meta to obtain `flat_id`.
4. Transmit `{flat_id, flat_data_region, flat_meta}`.

A receiver of a serialised carrier sees `flat_id` — not `live_id` — so
`carrier->id == known_live_id` is **false**, and the receiver never attempts a
live-object cast of wire bytes.  The id split is the mechanism that makes the
zero-copy path safe by construction: there is no code path by which a receiver
casts flat bytes as a live object while following the normative id-check
protocol.

See `docs/design/shm-transport.md` (§1, §4 `shm_send_fn`) and
`docs/design/zenoh-transport.md` (§Wire framing, §TLM over the wire) for the
reference implementations of the serialisation step.

## Endianness

Multi-byte numeric fields are little-endian in the data region.
Bridging transports convert as required.
