# sim-events/ — simulation lifecycle protocol

Coordinates phase transitions between an FSS host and the artefacts it has
loaded.  v0.1 specifies only individual single-carrier request/response events;
cross-carrier choreography is design-pending.  Time advance / quantum
negotiation is a SEPARATE protocol (a dedicated time-window carrier), not part
of the lifecycle.

## Canonical definition

The canonical protocol definition is [`event.jsonnet`](event.jsonnet).
There is no C binding yet.

## Atom composition

| Offset | Size | Atom              |
|--------|------|-------------------|
| 0      | 4    | `sim::EventCode`  |
| 4      | 4    | `sim::Status`     |
| —      | —    | `core::Extensions` (sub-meta; no data bytes) |

Atom definitions: [`atoms/sim/`](../../atoms/sim/) and
[`atoms/core/`](../../atoms/core/).

Per-field mutability, role, and phase are intrinsic to each atom; see
the atom catalogue.

## Sequence (single phase-transition)

```mermaid
sequenceDiagram
    participant Host as Initiator (host)
    participant Artefact as Target (artefact)

    Note over Host: build carrier:<br/>EventCode;<br/>Status undefined
    Host->>Artefact: send phase-transition carrier
    Note over Artefact: process event<br/>(advance phase);<br/>write Status
    Artefact->>Host: same carrier (mutated)
    Note over Host: read Status
```

Single round-trip per event.  The host issues one carrier per phase
transition; the artefact processes synchronously and returns Status.

## Federation startup barrier — the readiness wave

The lifecycle's first real choreography. A federation is a chain
(orchestrator → source → … → sink). Because a node may synchronously depend on
its downstream peer (e.g. a producer that polls a consumer), the federation
starts **sink-first** and readiness flows **UP**:

| Code | Flow | Meaning | Emit when |
|------|------|---------|-----------|
| `Bound`=0 | local | "you are wired" | whoever bound a node's endpoints tells it |
| `Elaborated`=1 | UP (to upstream) | "I am running" | wired AND every downstream peer sent `Elaborated` |
| `Stopped`=5 | UP (to upstream) | "done" | own work complete (sink: N frames; relay/source: + downstream `Stopped`) |

A node leaves its bind window — elaborates and runs — only once it is `Bound`
AND `downstream_ready == n_downstream` (the sink, with no downstream, runs on
`Bound` alone). Having armed itself it announces `Elaborated` up and only then
runs, so by the time its upstream sends it a window or a poll it is already
servicing — which is what lets `fss/time` be pure fire-and-forget (no readiness
probe on the data plane). The source's `Elaborated` tells the orchestrator the
federation is running; its `Stopped`, that the federation is done.

**Each protocol rides its own channel.** `sim::event` travels on the dedicated
`fss/sim` port; `time::window` on `fss/time`. Lifecycle is delivery-confirmed
and must not be lost to a busy data plane, so it is kept on its own link rather
than demultiplexed onto the fire-and-forget time link by `carrier.id`. A node
reports readiness UP on its own `fss/sim` send, bound to its upstream's
per-child sink (`fss/sim/<tag>`, e.g. `fss/sim/dev`, `fss/sim/ctrl`); the HEAD
is identified language-agnostically by the orchestrator installing a SEND on the
head's `fss/sim` port, so the head reports up to the orchestrator there. Time
windows flow independently on `fss/time`.

**Native phase → code.** Each binding maps its own lifecycle to these codes in
one place: SystemC gates `sc_start` on the wave and emits `Elaborated` after
`sc_start(SC_ZERO_TIME)`; the Rust/Python/FMI invented-clock bindings gate the
clock thread and emit `Elaborated` as its first act. Translation is confined to
the binding (e.g. an FMI node maps FMU instantiate/initialize/terminate), so a
mixed-language federation interoperates through these codes, not a shared ABI.

## Status

- Atom composition: defined.
- Jsonnet composition: present (this directory).
- C binding: **not yet**.

## Design-pending

- **Multi-message phase choreography.**  The startup/teardown barrier IS
  now specified and implemented — the readiness wave above
  (`Bound`/`Elaborated`/`Stopped` along the federation chain, demuxed off
  the time links).  What remains: conditional branches (`Pause`/`Resume`),
  `Status=Failed` propagation, and a *federation-wide* coordinator barrier
  for arbitrary DAGs rather than a single chain.
- **Time-advance / quantum negotiation** is a SEPARATE protocol — a
  dedicated time-window carrier `(t_lower, t_upper)` meaning "run to at
  least `t_lower`, do not pass `t_upper`", sent one-way and answered by
  the peer's own window — NOT a `sim::event`.  See the time-sync design.
- **psystemc support for mid-elaboration pause.**  The current FSS
  SystemC artefact wrapper runs `sc_start(SC_ZERO_TIME)` straight
  through `before_end_of_elaboration → end_of_elaboration →
  start_of_simulation → first delta` in one monolithic step, leaving
  no window for the host to do binding work.  A `Bound` event
  precedes `Elaborated` in the design, but enforcing that ordering
  requires psystemc support that does not yet exist.
- **Out-of-band stop.**  An artefact may need to signal `Stopped`
  asynchronously (e.g., on internal error).  The relationship between
  host-initiated and artefact-initiated state changes is not yet
  specified.

These items are tracked in any active design-issues log; they're the
substance of the simulation-lifecycle protocol design that this
directory will eventually become.
