# config/ — parameter configuration protocol

Generic Get/Set/Describe of named parameters between an FSS host and
an artefact.  Not specific to any framework — SystemC-CCI is one
binding mapping, FMI variables are another.

## Canonical definition

The canonical protocol definition is [`param_request.jsonnet`](param_request.jsonnet).
There is no C binding yet; once concrete consumers exist, a
`bindings/c/` subdirectory will provide a hand-written binding tracking
the Jsonnet composition.

## Atom composition

| Offset | Size | Atom                 |
|--------|------|----------------------|
| 0      | 4    | `config::OpCode`     |
| 4      | 4    | (padding)            |
| 8      | 8    | `config::ParamName`  |
| 16     | 40   | `config::ParamValue` |
| 56     | 4    | `config::Status`     |
| —      | —    | `core::Extensions` (sub-meta; no data bytes) |

Atom definitions: [`atoms/config/`](../../atoms/config/) and
[`atoms/core/`](../../atoms/core/).

Per-field mutability, role, and phase are intrinsic to each atom; see
the atom catalogue.

The protocol id is the FNV-1a-64 of the canonical CBOR meta of this
composition, with the sub-meta exclusion rule applied
(see [docs/spec/01-carrier.md](../../docs/spec/01-carrier.md) §Identity).

## Sequence

```mermaid
sequenceDiagram
    participant Initiator as Initiator (host)
    participant Target as Target (artefact)

    Note over Initiator: build carrier:<br/>OpCode, ParamName,<br/>ParamValue (if Set);<br/>Status undefined
    Initiator->>Target: send carrier
    Note over Target: read OpCode, ParamName;<br/>Get: read param into ParamValue;<br/>Set: store ParamValue at ParamName;<br/>Describe: emit metadata into ParamValue;<br/>write Status
    Target->>Initiator: same carrier (mutated)
    Note over Initiator: read Status;<br/>read ParamValue (Get / Describe)
```

Single round-trip; mutable fields are written on the return path.

## Status

- Atom composition: defined.
- Jsonnet composition: present (this directory).
- C binding: **not yet** — a `bindings/c/` subdirectory will appear
  once a concrete consumer needs it.

## Design-pending

- **Parameter listing/enumeration** — for v0.1 the host iterates Get
  or Describe by known name.  A discovery mechanism (listing all
  parameter names, walking by prefix) is deferred until a concrete
  consumer requires it; it will likely be a separate protocol rather
  than an extension of this one.
- **Typed `config::ParamValue` shapes** — ParamValue is opaque bytes
  in v0.1; typed shapes (integer, float, string, boolean) are deferred
  and will be described by a sub-meta.
- **Lifecycle phasing** — when in the simulation lifecycle parameter
  Get/Set is valid (pre-elaboration vs running) is tied to the
  `sim-events` protocol and the simulation-lifecycle open issue
  (see `docs/design/open-issues.md` if present).
