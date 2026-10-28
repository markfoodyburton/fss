# topology/ — FSS topology language

**Topology schema: deferred.  The reference plan format is implemented but
non-normative.**

The topology language is the weakest-defined part of the FSS standard today.
It is the statement of *which endpoints join which channels over which transport*,
plus node construction and placement.  It answers: "who is in this federation, how
are they wired, and where does each participant run?"  Everything else — carrier
types, time synchronisation, configuration — is already defined at Tier 1 and
Tier 2; topology is the remaining piece that a generic orchestrator needs in order
to bring up a federation without bespoke glue.

This directory will hold the normative topology schema once the WG ratifies it.
Today it is empty; the current state is documented below.

---

## What exists today

### 1. Reference C structs in the runtime orchestrator

`reference/runtime/orchestrator/include/fss/runtime/engine.h` contains the
**non-normative** C representation the reference orchestrator uses internally:

| Struct | Role |
|---|---|
| `fss_node_spec` | one node: `name`, `model` path |
| `fss_edge_spec` | one directed edge: `from_node`/`from_ep`, `to_node`/`to_ep`, `roundtrip` flag, `flags`, `kind` |
| `fss_plan_t` | complete plan: `nodes[FSS_PLAN_MAX_NODES=8]`, `edges[FSS_PLAN_MAX_EDGES=32]`, transport and orchestration parameters |

Edge `kind` encodes transport: `FSS_XK_INPROC=0`, `FSS_XK_SHM=1`, `FSS_XK_ZENOH=2`.

These caps and field names are **not normative**.  They are the reference
orchestrator's internal data structures, not the standard topology schema.

### 2. The `control::plan` carrier (implemented reference bootstrap)

`protocols/control/plan.jsonnet` defines the carrier the orchestrator sends to
each agent at startup.  The agent initiates one empty round-trip request; the
orchestrator returns the complete plan in `PlanJson`.  The carrier composition
is shared.  Its JSON step representation is reference-runtime data, not a
ratified topology schema:

| Slot | Bytes | Meaning |
|---|---|---|
| `PlanJson` | 0–3071 (3072 B) | UTF-8 JSON array of reference step objects `{op, a0, a1, a2, xport, rt, fl}`; ordering is orchestrator-defined |
| `Status` | 3072 | reply code |
| `core::Extensions` | — | sub-meta for future extensibility |

The JSON step vocabulary (`op`, `a0`, `a1`, `a2`, etc.) is the reference
orchestrator's non-normative affair; a standard topology schema will replace it.

### 3. `control::OpCode` enum

`atoms/control/OpCode.libsonnet` defines the operation codes that the reference
orchestrator places in the `op` field of each plan step:

| Code | Value | Meaning |
|---|---|---|
| `Ping` | 0 | connectivity check |
| `Construct` | 1 | load and construct a node |
| `Server` | 2 | bind a channel as server |
| `Client` | 3 | bind a channel as client |
| `Arm` | 4 | arm for simulation |
| `Bind` | 5 | bind a data edge |
| `Stop` | 6 | tear down |
| `Done` | 9 | plan end marker |

Their use in the JSON plan is reference behavior.  Their precise semantics in a
portable topology schema are deferred.

### 4. Plan parsing in the reference agent

`reference/runtime/agent-core/agent_core.c` parses the `control::plan` JSON
payload and dispatches the `OpCode` sequence.  This is non-normative reference
implementation behavior.

### Endpoint and edge meaning in the reference plan

An edge is directed: `from_node/from_ep` is the initiating endpoint and
`to_node/to_ep` is the target endpoint.  `roundtrip=1` selects a channel that
returns target-mutated bytes and receive status to the initiator; `roundtrip=0`
is one-way.  `kind` selects `inproc`, `shm`, or `zenoh`.  In-process edges are
local direct wiring.  Cross-process edges name and attach a transport channel.
`COUNT` marks the target delivery sink used for the run verdict; `OPTIONAL`
permits an absent client endpoint.  These fields describe the current
`fss_edge_spec`, not a standard schema.

---

## WG candidate topology schema

The architecture drafts define candidate CDDL schemas.  The 32v1 draft (§4
Topology Policy) is the current working candidate; 24v7 is retained as
historical reference.  Neither is implemented as the reference plan format.

### Working target schema (32v1)

```cddl
topology = {
  id: tstr,
  nodes: [* topology-node],
  edges: [* topology-edge],
}

topology-node = {
  id: tstr,
  type: tstr,
  placement: tstr / null,
  params: { * tstr => any },
  graph_id: tstr / null,
}

topology-edge = {
  id: tstr,
  source: tstr,
  sourceHandle: tstr,
  target: tstr,
  targetHandle: tstr,
  quantum: float64,
}
```

Key design points:

- `nodes[].placement`: explicit participant placement override (`participant_id`)
  or `null` for auto-resolve.  This is the hook for distributed deployments.
- `nodes[].graph_id`: composite node marker; a non-null `graph_id` means this
  node is itself a sub-topology (nested federation).  `null` for leaf nodes.
- `edges[].quantum`: minimum simulated-time lookahead bound for the edge.
  Cross-participant edges require a positive quantum for time-decoupled execution.
- `edges[]` are directed; `source`/`sourceHandle` and `target`/`targetHandle`
  name the endpoint on each side.

### Earlier version (24v7)

```cddl
topology = {
  graph_id: tstr,
  run_id: tstr,
  version: uint,
  nodes: [* topology-node],
  edges: [* topology-edge],
}

topology-node = {
  id: tstr,
  type: tstr,
  partition: tstr / null,
  params: { * tstr => any },
}

topology-edge = {
  id: tstr,
  source: tstr,
  sourceHandle: tstr,
  target: tstr,
  targetHandle: tstr,
  quantum: float64,
}
```

Differences from the 32v1 target: uses `partition` (not `placement`), adds
`run_id` and `version` to the top-level record, and has no `graph_id` on nodes
(no composite-node support in this version).

---

## Why this blocks external orchestrators

Today, any orchestrator other than the reference one must reverse-engineer the
`PlanJson` step vocabulary from `agent_core.c` and `control::OpCode` to bring up
a federation.  There is no schema document it can validate against, and the step
field names (`a0`, `a1`, `a2`) are positional and undocumented outside the source.

Until the WG ratifies a normative topology schema (and the reference orchestrator
migrates to it), external orchestrators cannot be written from the standard alone.
This is the principal remaining gap between "reference-only" and "genuinely
multi-implementation" FSS federations.

The candidate schema above is the WG's working target.  The migration path is:

1. Ratify the CDDL schema (possibly this directory's `topology.cddl`).
2. Rename `PlanJson` or add a new slot that carries the structured form.
3. Update `agent_core.c` to parse the structured form.
4. Deprecate the positional step vocabulary.
