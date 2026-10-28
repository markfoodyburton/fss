# FSS performance note

---

## What the numbers here represent

`reference/examples/platforms/can_bus` is a **correctness and interoperability sweep**,
not a throughput benchmark.  Each test instance runs exactly 16 frames (the
count is baked into each initiator artefact).  Wall-time is therefore dominated
by orchestration: process fork, `dlopen`, SystemC-kernel / CPython / FMU
instantiation, and Zenoh connection/readiness work. The current Zenoh startup
contract uses bounded readiness retries, not a deliberate discovery-settle
sleep. It does not measure per-frame cost.

Per-frame cost is measured separately by `reference/examples/tools/transport_bench` and
prior FSS_BENCH runs; those numbers appear in §Per-operation mechanism cost.

---

## End-to-end test time (orchestration, not throughput)

Historical source: `ctest -L agent_matrix -j6 --repeat until-pass:3` — 576
tests. Timing values below are a snapshot, not a current benchmark result.

Test name pattern: `agent_can_<hop1>_<hop2>_<init>_<ctrl>_<dev>`.

| hop1 × hop2 | mean test time |
|---|---|
| inproc × inproc | 0.05 s |
| inproc × shm | 0.05 s |
| shm × inproc | 0.05 s |
| shm × shm | 0.05 s |
| shm × zenoh | 1.15 s |
| zenoh × inproc | 2.17 s |
| inproc × zenoh | 2.18 s |
| zenoh × shm | 2.18 s |
| zenoh × zenoh | 3.11 s |

**Language axes are flat to ~1%.** Initiator / controller / device each average
1.21–1.24 s across sc / rust / python / fmi, regardless of language.  All
variance is the transport. The historical Zenoh cost includes connection and
startup readiness, not frame throughput. It must not be read as a fixed
settle-sleep cost.

---

## Per-operation mechanism cost (the real latency)

Sources: `reference/examples/tools/transport_bench` (fresh run); prior FSS_BENCH
measurements (labelled below).

| mechanism | cost / frame | note |
|---|---|---|
| inproc send | 1.96 ns | by-reference identity call; genuinely zero-copy |
| shm send | ~665 ns | cross-process; ~4 ns is the 16-byte memcpy; the rest is the POSIX named-semaphore doorbell (~2.4 µs blocking round-trip floor, halved one-way) |
| zenoh send | network / TCP-loopback | not micro-benchmarked here; per-frame sub-ms, while startup connection/readiness dominates this small test |
| SystemC cross-kernel dispatch | ≈ 2.5 µs / frame | queue + condvar hop into the model's kernel thread (PRIOR FSS_BENCH measurement) |
| live-gp C++ fast-path vs meta-walk | ≈ 0.5 µs / frame delta | non-C++ controller reading a live carrier through the meta pays almost nothing for declining the cast (PRIOR FSS_BENCH measurement) |
| controller derive (TLM → `can::frame`) | negligible | 16-byte memcpy + a few field reads |

**Standard-relevant takeaway.** The carrier path is effectively free in-process
(≈ 2 ns, by reference).  Cost appears only at a real boundary: shm doorbell
≈ 0.65 µs one-way, or a SystemC kernel-thread hop ≈ 2.5 µs.

**Not measured / future.** Steady-state per-language-combo throughput is not
captured here because initiators bake N = 16.  A real throughput harness would
need a configurable frame count.
