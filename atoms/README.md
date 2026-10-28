# atoms/

Canonical atom registry for FSS.  Each subdirectory is a namespace;
each namespace's `README.md` is the human-readable catalogue of atoms
in that namespace; each `<namespace>.libsonnet` barrel file is the
machine-readable mirror that imports all per-atom files in the namespace.

This registry is shared by both `protocols/` (FSS-owned protocols) and
`known_carriers/` (externally-defined protocol carriers).

## Layering

Atoms are the **logical** layer: each names a semantic *role* (a
locator, an operation, a status) independent of how it is laid out in
bytes.  They sit between the **flat-layout profile** (mechanical byte
interpretation, defined in
[docs/spec/02-encodings.md](../docs/spec/02-encodings.md)) and the
**physical** layer — **carrier shapes** (compositions of atoms with
magic ids and behaviour, defined in `protocols/` and
`known_carriers/`), which pin each role to a concrete offset, width,
and enum encoding.

The atom mechanism is normatively specified in
[docs/spec/03-atoms.md](../docs/spec/03-atoms.md).

## Namespaces

| Namespace  | Purpose                                                       |
|------------|---------------------------------------------------------------|
| `core::`   | FSS format primitives (Extensions, the sub-meta marker).      |
| `bus::`    | Transactional interaction: locator + operation + payload + result (TLM read/write is one instantiation). |
| `config::` | Configuration parameter exchange (generic; SystemC-CCI is one binding mapping). |
| `sim::`    | Simulation lifecycle: phase transitions and the readiness wave. |
| `time::`   | Simulation-time windows (`Lower`/`Upper` bound at a `Unit` resolution) for federation time-sync. |

Anticipated future namespaces (added when concrete consumers appear):
`debug::`, `routing::`, `tlm-events::`.

## Adding an atom

1. Pick a namespace; create a new one if no existing namespace fits.
   Reserve `core::` for FSS format primitives.  (Prefer reusing a shared
   `bus::` role over minting a near-duplicate atom; a field that is genuinely
   protocol-specific and not reusable vocabulary is defined *inline* in its
   carrier composition, not here — cf. `tlm::GpOption`, `can::Ide`.)
2. Choose the qname, `wire` type, and version (start at `1`).  A shared role
   atom uses a width-agnostic `wire` (`uint`/`int`) and takes its width from
   each carrier's `byte_size`.
3. Create `atoms/<namespace>/<LeafName>.libsonnet` with the
   atom object literal (fields: `qname`, `version`, `wire`,
   `description`, optional `default_encoding` (the meaning→wire-value map for
   an enum; a composition MAY override it per carrier), optional `layout`,
   optional `serializable` (default `true`; set `false` for an atom a wire
   transport must drop, e.g. a native-object handle like `core::TypeId`),
   optional `in_hash` (default `true`; set `false` for an atom excluded from the
   carrier id, e.g. an extension hook), `mutable_by`, `read_by`, `phase`).  See
   `docs/spec/02-encodings.md` §Atom attributes for `serializable` / `in_hash`.
4. Add an entry to the namespace barrel `atoms/<namespace>.libsonnet`:
   `LeafName: import '<namespace>/<LeafName>.libsonnet',`.
5. Add the human-readable entry to the namespace's `README.md`.

The barrel and the README MUST agree on every atom's qname, version, the `wire`
CDDL type (which fixes the byte width for fixed-size types), and any non-default
`serializable` / `in_hash` attributes.
