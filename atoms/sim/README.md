# sim:: — Simulation lifecycle atoms

Primitives for the simulation phase-transition handshake.  Used by the
sim-events protocol; reusable for any simulator lifecycle handshake.  Time
advance is a SEPARATE protocol (a dedicated time-window carrier), not a
lifecycle event.

## Catalogue

| Atom             | Version | wire           | byte_size | mutable_by | read_by   | phase    | Description                                                                                                                                         |
|------------------|---------|----------|-----------|------------|-----------|----------|-----------------------------------------------------------------------------------------------------------------------------------------------------|
| [`sim::EventCode`](EventCode.libsonnet) | 1       | `uint .size 4` | 4         | initiator  | target    | request  | Lifecycle phase event.  Allowed values: `Bound = 0`, `Elaborated = 1`, `Started = 2`, `Paused = 3`, `Resumed = 4`, `Stopped = 5`. |
| [`sim::Status`](Status.libsonnet)       | 1       | `uint .size 4` | 4         | target     | initiator | response | Response code, written by target.  Allowed values: `Ok = 0`, `Failed = 1`.                                                                          |

## Status

Minimal viable for individual phase-transition request/response
carriers.  Multi-message phase choreography — cross-carrier ordering,
time-grant negotiation, and barrier synchronisation — is
design-pending.  Tracked in
[docs/design/open-issues.md](../../docs/design/open-issues.md)
under the simulation-lifecycle issue.

## Future atoms

- `sim::BarrierId` — identifies a synchronisation barrier for multi-participant lifecycle steps.
- `sim::PhaseFlags` — modifier bits (e.g., non-blocking, advisory).

Time-advance / quantum negotiation lives in its OWN protocol (a dedicated
time-window carrier), not in `sim::` — see the time-sync design.
