# FSS — current state (as of 2026-10-05)

**This is the active status pointer for the FSS reference repo.**  Detailed design notes follow in `open-issues.md` and the spec at `docs/spec/`.

## Where the codebase is

- **Spec**: `docs/spec/` (7 flat files: README + `01-carrier.md` through `06-conformance.md`).  Normative.
- **Atoms**: `atoms/` (top-level).  6 namespaces: `core::`, `bus::`, `config::`, `sim::`, `time::`, `control::`.  Each atom is a Jsonnet data record (`*.libsonnet`).
- **Carrier shapes**:
  - `known_carriers/` — externally-defined protocols where FSS just packages the carrier.  Currently: `tlm/transaction.jsonnet` (IEEE 1666.1), `can/frame.jsonnet`.
  - `protocols/` — FSS-defined protocols.  Currently: `config/param_request.jsonnet`, `sim-events/event.jsonnet`, `time-sync/window.jsonnet`, and `control/{instr,plan}.jsonnet`.
- **Tooling**: `reference/tools/fss_compile.py` resolves a carrier shape to the canonical CBOR meta and computes the FNV-1a-64 carrier id.  Standalone.
- **Code**: `abi/` defines the header-only carrier, endpoint, transport-plugin,
  and artefact ABIs.  The transport vtable is `channel`/`attach`/`send_to`; an
  endpoint is `{self, receive, send, send_ctx}`.  The reference has in-process,
  shared-memory, and Zenoh transports; C helper, SystemC, Rust, Python, and FMI
  language paths.  The 576-cell cross-language sweep is the `agent_matrix`
  test label; targeted transport tests use `shm_multipoint_*`,
   `zenoh_multipoint_*`, and the fanout/fanin matrix labels.
  Binary-loader transports also export ABI-version and capability queries.  The
  loader checks compatible version, required channel/attach/send-to capability
  bits, and the selected transport name before creation.  This is a reference
  implementation ABI rule; the active paper and `docs/spec/` retain authority
  for published and normative claims.

## Normative carrier ids (working candidate)

Computed by `reference/tools/fss_compile.py`:

| Carrier shape           | Id                  |
|-------------------------|---------------------|
| `tlm::transaction` (live) | `b22e5166992195b9` |
| `tlm::transaction` (flat/wire) | `a16da8f9ada715fe` |
| `can::frame`            | `18c2d2e46a9a45c2`  |
| `config::param_request` | `1f3dd8fae7161678`  |
| `sim::event`            | `893d9179e7c99450`  |
| `time::window`          | `f13a7a68c87a6127`  |
| `control::instr`        | `779674b4d01c8829`  |

`tlm::transaction` has two ids: the **live** composition carries the
non-serialisable `core::TypeId` (the in-process zero-copy fast path); a wire
transport drops it and inlines the `fss:local-ptr` payloads, yielding the
**flat** id that crosses the wire (see [02-encodings.md](../spec/02-encodings.md)
§`core::TypeId`).  The other shapes carry no `core::TypeId`, so live == flat.

Hash function `FNV-1a-64` is the working candidate; subject to change before standardisation, at which point the ids change with it.

## Open work blocks (substantial)

1. Hand-written accessors are the intended approach for C bindings.  This is guarded by the `c_meta_resolver` test, which checks the meta resolver's Path-B offsets against the hand-written Path-A offsets (see `open-issues.md` §10).
2. Carrier-id validation in dispatch — RESOLVED: identify-or-reject is now a spec conformance point (see `docs/spec/06-conformance.md` req. 7 and `docs/spec/05-abi-conventions.md` §Consuming a carrier's fields).
3. Simulation-lifecycle protocol design — multi-message phase choreography for `sim-events`; needs psystemc support for mid-elaboration pause.
4. Wire transports — in-process, shm, and Zenoh are implemented.  FMI-binary
   remains deferred.
5. Typed `config::ParamValue` shapes (currently opaque bytes; typed shapes deferred to a sub-meta).
6. Multi-artefact-per-library hook (`fss_artefact_manifest()` forward-compat).

---

# What goes in the FSS standard vs everywhere else

Concrete answer to "if I clone the standard, what's in it?"

## The standard (tiny, slow, normative)

```
spec/                  ★ NORMATIVE — 7 markdown files (01-carrier … 06-conformance)
atoms/core/            ★ NORMATIVE — the core:: namespace (Extensions; the reserved namespace)
```

The normative core is the carrier mechanism, the CDDL flat-layout
profile, the atom-definition shape + qname convention, protocol identity
(the canonical-meta hash + sub-meta exclusion), and the `core::` reserved
namespace.  ~670 lines of spec plus a one-atom namespace.  It depends on
RFC 8610 (CDDL) and RFC 8949 (CBOR); it invents no schema or wire format
of its own.

Everything below depends on the standard; the standard depends on
nothing.  That asymmetry is what makes it a standard.

## Atom registries (semantics, evolve independently)

The atom namespaces beyond `core::` (`bus::`, `config::`, `sim::`,
`time::`, and any community namespace) are catalogues of named field
shapes.  They use
the normative atom shape but are not themselves frozen — anyone may
publish a namespace.  This repo curates a few; it is not authoritative
over all of them.

## Carrier shapes (compositions)

`atoms/` is the **logical layer**: reusable semantic roles and meaning
vocabulary (`bus::Operation`, `core::Extensions`, …), width-agnostic, each
declaring a `default_encoding`.  `known_carriers/` and `protocols/` are
the **physical layer**: concrete compositions that fix byte offset, width, and
the effective encoding (a composition MAY override an atom's `default_encoding`
via `encoding:{}`; otherwise it inherits it).

- `known_carriers/<name>/` — externally-defined protocols where FSS only
  packages the carrier shape + magic id (e.g. `tlm/` for IEEE 1666.1).
- `protocols/<name>/` — FSS-defined protocols where FSS owns both the
  shape and the behavioural spec (`config/`, `sim-events/`, `time-sync/`, `control/`).

Each is a Jsonnet composition over atoms plus a `README.md` (atom table,
Mermaid sequence diagrams, prose).  Two ends of a binding compile the
same composition independently and compute the same id — no registry
exchange, no negotiation.

## Tooling

- `reference/tools/fss_compile.py` — resolves a composition via `jsonnet`, builds
  the canonical CBOR meta, applies the sub-meta exclusion, computes the
  carrier id.  Standalone reference compiler.
- Typed-accessor codegen (`gen-typed-c`) — removed (in git history),
  deferred indefinitely (open issue #10).  Hand-written accessors are
  the intended approach; generated headers (`fss/known_carriers/tlm_id.h`,
  `fss/protocols/sim.h`) cover only the drift-prone constants
  (ID + canonical meta bytes).

## Runtime / transports / language bindings

- `abi/` — binary-loader C ABI (carrier, endpoint, transport vtable
  `channel`/`attach`/`send_to`, artefact entry points).  Its plugin-loader
  version/capability handshake is implementation-family detail, not Tier-1
  carrier semantics.
- `reference/transports/<name>/` — one per wire.  `inproc`, `shm`, and `zenoh`
  are implemented; FMI-binary is deferred.
- `reference/languages/<lang>/` — one per language binding (SystemC, Rust,
  Python, and FMI artefacts; all four interoperate in the matrix).

Languages and transports never see each other; both are clients of the
C ABI.  A generic agent (owns `main`, artefact-agnostic) loads its control
transport from a standard breadcrumb (`--fss-parent '<json>'` on the CLI, or
`fss_topology_transport_init` for a dlopen'd model), phones home, and
constructs and attaches artefacts only as instructed by the pulled
`control::plan` carrier.  `control::instr` remains the individual-step and
lifecycle control form.  The orchestrator drives agents and selects a transport
per edge.

## Property worth keeping

The normative core is small and stays small.  Atom registries, carrier
shapes, codegen, and transport glue all move at their own pace outside
it.  Keep that boundary sharp: if something changes often, it does not
belong in the normative core.
