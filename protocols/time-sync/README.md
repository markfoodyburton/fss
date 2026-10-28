# time-sync/ — simulation-time window protocol

Bounds how far each node may advance simulated time, so a federation stays
synchronised.  A node sends its peer a **window** on the standard `fss/time`
control port; the peer runs within it and replies with its own window.

## Canonical definition

[`window.jsonnet`](window.jsonnet) — the `time::window` carrier.  Generated id +
meta: `fss/protocols/time_sync.h` (`FSS_TIME_ID` / `FSS_TIME_META`).

## Atom composition

| Offset | Size | Atom |
|--------|------|------|
| 0  | 4 | `time::Unit`  (sc_time_unit exponent) |
| 4  | 4 | (padding) |
| 8  | 8 | `time::Lower` (run at least to) |
| 16 | 8 | `time::Upper` (do not pass) |
| —  | — | `core::Extensions` (sub-meta; no data bytes) |

Atom definitions: [`atoms/time/`](../../atoms/time/).

## Semantics

A window `(Lower, Upper)` at resolution `Unit` means: **execute at least up to
`Lower`, and do not simulate past `Upper`**.

- `(0, MAX)` — run freely (no bound).
- `(0, 0)`   — stop (advance no further).

The exchange is **symmetric and one-way**: each node sends the other its window;
there is no request/grant pair.  A receiver replies with its own window **at the
latest when it reaches `Upper`** (earlier if it can), reporting how far it will
go.  The peer uses that to extend its own bound.  Across a chain the time skew is
bounded by `quantum × hop-count` — e.g. initiator→controller→device is two hops,
so nodes stay within `2 × quantum` of each other.

## Startup readiness — NOT here; it is a sim-status barrier

`fss/time` carries time-sync windows and **nothing else**: every window is pure
fire-and-forget.  A node must still not suspend at its first bound until its peer
is listening (else a first window lost to transport-discovery latency strands the
federation), but that readiness is established by the **sim-status lifecycle
protocol**, not by probing the data plane with a retried window.

The rule is the upward **readiness wave** (see
[`protocols/sim-events/README.md`](../sim-events/README.md)): the federation
starts sink-first, and a node only begins advancing — and only then sends its
first window — once every node it depends on downstream has announced
`Elaborated` ("running").  By the time a window flies, its peer is already running
and matched, so no window is lost to startup and no first-window retry is needed.
Earlier drafts confirmed the first window via the round-trip `send()`
(`FSS_SEND_OK`); that conflated the time window with a readiness signal and has
been removed in favour of the explicit sim-status barrier.

## Per-simulator realisation

| node | run to ≥ Lower | do not pass Upper | (0,0) stop |
|------|----------------|-------------------|------------|
| **SystemC** | run/`sc_start` to `Lower` | `sc_ob_event` at `Upper` → `sc_suspend_all` (fires when the kernel *reaches* `Upper` without scheduling work there) | `sc_sync_window` zero-window |
| **FMI** | `DoStep` until `current ≥ Lower` | clamp the step so `current + dt ≤ Upper` | step size 0 / hold |
| **Rust / Python** | process queued work, set local clock ≥ `Lower` | refuse to advance the invented clock past `Upper` | hold the clock |

SystemC's `sc_sync_window` (psystemc / qbox) already implements exactly this
windowing; the SystemC node reuses it.

## Status

- Atom composition + carrier: defined.
- Binding handlers + transport wiring: in progress.

## Design-pending

- Reply/ack shape beyond "send your own window" (explicit grant accounting).
- A *coordinator* role / multi-participant barrier (HLA-style TAR/TAG remains the
  reference point).  The point-to-point startup barrier (above) is settled; a
  federation-wide rendezvous is not.
- The acceptance test: skew across the full chain ≤ `quantum × max-path`.
