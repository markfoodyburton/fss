# Design notes

The reasoning behind FSS — decisions, rationale, and the honest list of
what is not yet settled. For the normative standard itself, see
[`../spec/`](../spec/); for the newcomer pitch, the
[top-level README](../../README.md).

## Where things stand

- [repo-vs-impl.md](repo-vs-impl.md) — **the live status pointer.** What
  is built, the current carrier ids, and the open work blocks. Read this
  first to see where the codebase actually is.
- [open-issues.md](open-issues.md) — what the design does *not* yet
  settle (hash choice, wire sub-meta framing, lifecycle choreography,
  trust, governance). Honest and current.

## How the pieces fit

- [channel-abi-rework.md](channel-abi-rework.md) — **implemented ABI record.**
  The channel/attach transport model now used by the reference ABI.
- [binding-and-phases.md](binding-and-phases.md) — **historical design note.**
  It records the pre-channel `open`/`bind` model; use `abi/include/fss/transport_abi.h`
  for the current ABI.
- [performance.md](performance.md) — what the numbers mean: per-operation
  mechanism cost vs. end-to-end orchestration time.

## Per-transport rationale

- [shm-transport.md](shm-transport.md) — the same-machine shared-memory
  fast path: segment layout, the doorbell choice, why we copy rather than
  chase zero-copy.
- [zenoh-transport.md](zenoh-transport.md) — the network / cross-host
  path: request/reply vs pub/sub, wire framing, discovery.

## Exploration

- [exploration/](exploration/) — half-formed ideas and spikes that have
  not earned a place in the spec (CDDL prior-art packages, the SHM
  platform spike, the CDDL RFC text).
