# 06 — Conformance

A **conforming FSS implementation** correctly implements the
normative parts of this spec.  This section enumerates them.

## Normative requirements

A conforming implementation MUST:

1. **Carrier struct** — accept and produce 40-byte carrier headers
   with the field layout in [01-carrier.md](01-carrier.md): `id`,
   `data_ptr`, `data_size`, `meta_ptr`, `meta_size`.

2. **Meta format** — produce and parse meta regions as canonical
   CBOR (RFC 8949 §4.2 deterministic encoding) in the entry shape
   defined by [01-carrier.md](01-carrier.md) §Entry shape.

3. **Field types** — support the full CDDL flat-layout profile and the
   FSS `layout` constructs in [02-encodings.md](02-encodings.md).
   Implementations MUST NOT extend the profile or define new `layout`
   constructs.

4. **Identity computation** — compute the carrier id as the
    canonical-meta hash, retaining a sub-meta marker while excluding its
    nested `submeta` contents, and applying the
   `in_hash` exclusion rule (see [02-encodings.md](02-encodings.md)
   §Atom attributes).  Treat ids as opaque except for equality.

5. **Shared names** — preserve qnames in a meta when communicating
     parties use them for field resolution.  The relevant atom records MAY
     come from the reference vocabulary, an independently agreed vocabulary,
     or inline composition records; Tier-1 requires no global qname registry.

6. **Hash profile** — agree on a hash function before comparing carrier
    ids.  FNV-1a-64 is the reference working candidate, not a final
    standard value.

7. **Identify before consuming** — a receiver MUST identify a carrier
   before consuming its data-region bytes: it MUST either recognise the
   `id` (Path A: use the known layout) or resolve the needed fields by
   qname from the carrier meta (Path B).  A receiver MUST NOT dispatch
   a carrier it can neither recognise nor resolve — never reinterpret
   unknown bytes at guessed offsets.  See
   [05-abi-conventions.md](05-abi-conventions.md) §Consuming a
   carrier's fields.

A conforming FSS-native carrier SHOULD include `core::Extensions` so
that it is extensible by default.  A foreign-wrapping carrier MAY omit
it.  See [04-protocols.md](04-protocols.md) §Inclusion.

## Informative

The following parts of the spec are informative; conforming
implementations may follow other conventions provided they remain
interoperable at the carrier level:

- The transport ABI vtable shape ([05-abi-conventions.md](05-abi-conventions.md) §Transport).
- The artefact entry-point ABI ([05-abi-conventions.md](05-abi-conventions.md) §Artefact).
- The endpoint struct ([05-abi-conventions.md](05-abi-conventions.md) §Endpoint).
- Atom role/phase annotations (`mutable_by`, `read_by`, `phase`) and
  prose `description` — these are source atom-record documentation that may
  be associated by `qname`; they are never part of the transmitted meta (see
  [01-carrier.md](01-carrier.md) §What the meta carries) and a consumer
  need not act on them.

The three ABI shapes are informative *as universal requirements* — a
transport family may define its own in-process rendezvous and still
interoperate through the carrier.  Within the binary-loader (dlopen)
transport family, however, they are normative: artefacts and transports
built independently and loaded into one process MUST agree on these
struct layouts and symbol names to interoperate.

Within that binary-loader family, a transport plugin MUST export the packed ABI
version and capability queries as well as the create, destroy, and name
symbols.  Before calling a factory or dereferencing a vtable, a host MUST
check: matching ABI major; plugin minor at least its required minor; and all
bits in `FSS_TRANSPORT_CAP_CHANNEL_ABI`.  It MUST then verify the exported
name against the selected transport name.  These loader requirements apply to
the reference binary-loader ABI, not to Tier-1 carrier conformance.

## Working-candidate values

The following spec value is subject to change before standardisation:

- Hash function: FNV-1a-64 (working candidate; the single genuinely
  undecided open question).

Implementations targeting interoperability with the FSS reference stack
should use the working candidate until the value is standardised.

The `core::` namespace contents beyond `core::Extensions`, and the
temporal-annotation vocabulary (`mutable_by`, `read_by`, `phase` roles),
are reference vocabulary that may evolve independently of Tier-1.

## Out of scope of the Tier-1 spec

The Tier-1 mechanism (this spec document set) does NOT define:

- Specific atom values or protocols.  Atoms are defined in `atoms/`
  and protocols in `protocols/` and `known_carriers/` — Tier-2 WG
  artefacts that evolve on their own schedule.
- Specific transports.  Transports follow the informative
  conventions in [05-abi-conventions.md](05-abi-conventions.md).
- Language bindings.  C is the reference; Rust, Python, etc. are
  free to define their own equivalents.
- Discovery and configuration.  These are protocol concerns, not
  spec concerns.
- Typed configuration, BIDI channels, and the final hash-function choice.
- Lifecycle and time advancement beyond the carrier mechanism.
  The `sim::event` carrier (in `protocols/sim-events/`) is the
  defined vehicle; multi-message phase choreography is design-pending.
