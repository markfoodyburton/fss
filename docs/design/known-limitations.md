# FSS reference implementation: known limitations

Non-critical gaps in the **reference orchestrator/agent** (the `matrix_host` +
`fss-agent-c` pull path). None affects the FSS ABIs or the standard itself: the
spec permits any transport and any wiring; the examples just wire them narrowly.
Recorded here so they are not forgotten.

## 1. Control-plane bootstrap transport is hard-coded to `shm`

- **What:** the breadcrumb's `transport` is the compile-time constant
  `FSS_CONTROL_XPORT = "shm"` (`reference/runtime/orchestrator/engine.c:37`). Both
  orchestrator&harr;agent channels (topology *pull* and lifecycle *push*) are
  created on it.
- **Impact:** a genuinely remote federation cannot work: a remote agent
  cannot attach an `shm` control channel. The data plane already picks a transport
  by name per edge (`inproc`/`shm`/`zenoh`); only the control bootstrap is fixed.
- **Why non-critical:** every current example co-locates the orchestrator with (or
  forks) its agents on one host, where `shm` is fine.
- **Fix (small):** transport is already resolved by name via
  `fss_load_channel_named`; thread a `--control <name>` through `matrix_host` into
  `plan` and replace the constant.

## 2. Lifecycle channel is a separate breadcrumb field, not part of the pulled plan

- **What:** the breadcrumb carries `{transport, control, lifecycle}`. The
  `lifecycle` channel (orchestrator&rarr;agent stop push, `control::instr{STOP}`)
  is named in the breadcrumb rather than delivered in the pulled topology plan.
- **Impact:** the breadcrumb carries a teardown concern that is not strictly
  bootstrap; inconsistent with configuration, which is *not* a breadcrumb field.
- **Why non-critical:** it works, and the hard constraint is satisfied: stop
  needs its own orchestrator-initiated channel because the topology channel is a
  *pull* (agent is the initiator, orchestrator the responder), so you cannot push
  a stop down it.
- **Fix:** the orchestrator already provisions the lifecycle channel up front
  (`engine.c:218`); move its *attach* into the pulled plan (an attach-as-TARGET
  step, like the data/sim/time edges), shrinking the breadcrumb to
  `{transport, control}`. Only *how the agent learns to attach it* moves onto the
  pull plane; the separate channel stays. Cost: loses the pre-pull kill-switch
  (mitigated by killing the process).

## 3. Cross-process federation throughput: two blockers fixed, one deep one remains

Surfaced while trying to build a language$\times$transport$\times$time result.
Three separate problems blocked cross-process measurement; **two are now fixed**
and the third (a deep time-sync limitation) is characterised precisely below.

- **Zenoh dropped frames at scale — FIXED.** Zenoh's default congestion control
  is `DROP` (best-effort), so under sustained one-way traffic the session buffer
  filled and frames were silently dropped (`dev count=175/200` at N=200). The
  one-way `z_put` now sets `Z_CONGESTION_CONTROL_BLOCK` + `is_express`
  (`reference/transports/zenoh/zenoh.c`), so a full buffer applies backpressure instead of
  losing the frame. Verified: N=200 delivers 200/200 (was 175/200).
- **Teardown sat inside the timing bracket — FIXED.** `platform_host` used to
  bracket the run including child fork/teardown (~250 ms fixed). The rate is now
  taken AT THE SINK (the device's counting shim stamps the 1st and Nth frame and
  reports `(t_last - t_first)/(N-1)`), printed BEFORE teardown by whichever
  process hosts the sink. This excludes both federation bring-up and the fixed
  shutdown, so the per-txn number is the true steady-state cost at any `N`. The
  parent's completion wait now scales with `N` (`completion_ms`) so a legitimate
  large-`N` run is not cut off mid-stream.
- **shm/zenoh SC & Rust deadlock at high `N` — HANG FIXED, delivery cap remains
  (deep, psystemc-deferred).** The teardown *hang* is fixed: `dispatch_thread`
  now wakes every `endpoint_receive` waiter blocked in `cv.wait` before it stops
  (and refuses to enqueue once stopping), so a run always **terminates cleanly**
  instead of hanging the machine 3/3. What is NOT fixed is full delivery at high
  `N`: cross-process delivery still caps (~1200-1600 frames shm, ~350 zenoh; the
  cap is a probabilistic MTBF, not a fixed count). Root cause, diagnosed here:
    * **SystemC** — a *data-vs-time-suspend* deadlock. The fss/time window driver
      gates the kernel with `sc_suspend_all`; while suspended, the dispatch
      `SC_THREAD` is not scheduled, so an incoming (cross-process) `b_transport`
      whose doorbell wakes the kernel is still not serviced. The sender's shm
      ring then fills, its kernel blocks in the blocking send, and it can no
      longer emit the window the suspended peer needs. In-proc never hits this
      (the hand-off is synchronous and lock-stepped).
    * **Rust** — a *clock runaway*: a controller's `granted_upper` becomes
      unbounded under cross-process window lag and its clock races seconds ahead
      of its peers (`max_clock_gap` in the billions of ns), then it detaches and
      truncates the run.
  A localised SystemC fix (making `dispatch_thread` unsuspendable for its whole
  life) *did* let shm SC deliver 4000, but it unbalanced the time-gate's suspend
  accounting and regressed `fmi_sc_fmi` into a clock runaway, so it was reverted.
  The correct fix is the psystemc "foreign thread safely enters the suspended
  kernel" capability (the round-trip hop-1 path needs it too), which is out of
  scope for a localised change.

Net: cross-process throughput is now cleanly **measurable up to the per-transport
cap** (well over a thousand shm frames, a few hundred zenoh — ample steady-state
samples), and runs no longer hang. A full high-`N` language$\times$transport
surface still awaits the psystemc-level time-sync fix.

## 4. Model locator capped at 128 bytes — RESOLVED (models pass by name)

- **What it was:** in a cross-process launch the orchestrator marshalled each
  node's *absolute model path* into `fss_ctrl_instr.arg1`
  (`char arg1[128]`), which IS the 128-byte `control::Locator` field of the
  standard `control::instr` carrier (`protocols/control/instr.jsonnet:20`,
  `byte_size: 128`). A model whose absolute path exceeded 127 bytes was
  truncated and the agent failed to `dlopen` it — so on a machine with a long
  checkout path the longest reference artefacts (the Rust models under
  `…/target/release/deps/…`, ~143 chars) broke the corresponding cross-process
  matrix cells.
- **Fix:** the plan now carries a short model **name** (e.g.
  `fss_can_target_paced_rust`, ≤33 chars), never a path. Both the in-proc host
  and the remote agent resolve it to a plugin file against a model search dir
  (`--model-dir`) via `fss_model_plugin_path` — exactly the "name → plugin,
  never a path" discipline transports already use
  (`reference/runtime/orchestrator/orchestrator.c`, `fss_transport_plugin_path`).
  No carrier layout or `FSS_CONTROL_ID` change: `arg1` stays `char[128]` and now
  comfortably holds the name. Absolute paths no longer cross the FSS
  plan/host/agent boundary at all.

## Non-normative gaps (cleanup backlog)

- **`sim::Started` never emitted.** Only `Bound`, `Elaborated`, `Stopped` are
  live in the reference implementation. `Started` is *handled* as a `Bound`
  alias but never emitted (only a Rust unit test sends it,
  `reference/languages/rust/src/lib.rs:1516`). Defined in
  `atoms/sim/EventCode.libsonnet`.
- **`sim::Paused` / `sim::Resumed` not yet acted on.** These are standard
  lifecycle states (Tier-2 WG vocabulary in `atoms/sim/EventCode.libsonnet`);
  they have defined semantics in the carrier protocol. The reference
  implementation's `sim_receive` does not yet respond to them — a reference
  gap, not dead code to remove.
- **Vestigial `done` latch — REMOVED.** The orchestrator's federation-status
  handler previously mapped `Stopped` &rarr; `done`; `done` was never set
  because no artefact ever sends `Stopped` upward (`send_sim_up` only sends
  `Elaborated`; completion is detected by frame count / child exit). The `done`
  latch has been removed.

## Reference facts (so the above is unambiguous)

- `Bound` and `Stopped` are **local** signals: an artefact's agent raises them on
  its *own* models' `fss/sim`, never across a channel.
- `Elaborated` is the **only** `sim::event` that crosses between parties: it flows
  sink&rarr;source up the chain (device&rarr;controller&rarr;initiator), and the
  source's `Elaborated` reaches the orchestrator (how it learns the federation is
  running). It is the start barrier.
- **Stopping** is (a) the `fss/time` *open-window* detach cascade, model-driven,
  sink&rarr;source; and (b) the `control::instr{STOP}` lifecycle push,
  orchestrator-driven, to make process teardown prompt. Never a `sim::event`
  across parties.
