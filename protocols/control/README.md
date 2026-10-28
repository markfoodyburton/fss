# control/ — topology-control protocol

The control carriers let the reference orchestrator realise a topology:
construct a node, create or attach data channels, direct-wire local in-process
edges, arm, stop, and finish.  They are carried as ordinary FSS carriers by the
reference runtime (`reference/runtime/`).

`control::plan` is the active bootstrap carrier.  An agent sends an empty
round-trip request; the orchestrator returns its complete JSON plan in the
`PlanJson` field.  The JSON step vocabulary and step order are reference-runtime
details, not a topology standard.  `control::instr` remains the individual-step
carrier used by the reference control implementation, including lifecycle
traffic and direct in-process control calls.

These carriers are foundational by role: most carriers are carried *by* the
fabric, while control helps create and configure that fabric.  They remain
ordinary described carriers.

## Canonical definition

The canonical definitions are [`plan.jsonnet`](plan.jsonnet) and
[`instr.jsonnet`](instr.jsonnet).  The runtime C spelling of an individual
instruction (`fss_ctrl_instr`) lives in
[`reference/runtime/control-protocol/`](../../reference/runtime/control-protocol/).

`control::plan` has `PlanJson` (3072 bytes), `Status`, and
`core::Extensions`.  `control::instr` has the composition below.

## Atom composition

| Offset | Size | Atom                 |
|--------|------|----------------------|
| 0      | 4    | `control::OpCode`    |
| 4      | 4    | `control::Status`    |
| 8      | 4    | `control::Roundtrip` |
| 12     | 4    | `control::Flags`     |
| 16     | 64   | `control::Node`      |
| 80     | 128  | `control::Locator`   |
| 208    | 48   | `control::PeerEp`    |
| 256    | 32   | `control::Transport` |
| —      | —    | `core::Extensions` (sub-meta; no data bytes) |

Atom definitions: [`atoms/control/`](../../atoms/control/) and
[`atoms/core/`](../../atoms/core/).

Per-field mutability, role, and phase are intrinsic to each atom; see
the atom catalogue.  v0.1 keeps every field inline (fixed byte_size), so
the data region is byte-identical to the flat runtime struct.

The protocol id is the FNV-1a-64 of the canonical CBOR meta of this
composition, with the sub-meta exclusion rule applied
(see [docs/spec/01-carrier.md](../../docs/spec/01-carrier.md) §Identity).

## Sequence

```mermaid
sequenceDiagram
    participant Agent as Initiator (agent)
    participant Orchestrator as Target (orchestrator)

    Agent->>Orchestrator: empty control::plan request
    Note over Orchestrator: write JSON steps into PlanJson;<br/>write Status
    Orchestrator->>Agent: same carrier (mutated)
    Note over Agent: parse and run the complete plan
```

Bootstrap is one round-trip; `control::Status` is written on the return path.
The `control::instr` request/response shape uses the same status rule.

## Status

- `control::plan` and `control::instr` compositions: present in this directory.
- Runtime C spelling: [`reference/runtime/control-protocol/`](../../reference/runtime/control-protocol/)
  (`fss_ctrl_instr`, byte-compatible with `control::instr`).

## Design-pending

- **Topology schema** — `PlanJson` is a bounded, reference-specific JSON
  encoding.  A portable, ratified topology schema is deferred.
- **Transport selection** — the reference selects a named plugin per cross
  edge.  Richer per-interface policy and transport admission/refusal are
  deferred.
