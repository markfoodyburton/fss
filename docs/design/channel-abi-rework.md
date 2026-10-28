# Channel/transport ABI rework (v0.2 design)

> **Historical design record; implementation completed.** This document was
> the v0.2 execution plan on `fss-poc-rework`.  Its channel/attach ABI is now
> implemented in `abi/include/fss/transport_abi.h` and the reference transports.
> Read the ABI header and `docs/design/repo-vs-impl.md` for current behavior.
> Remaining TODO and staged wording below are retained as history, not a current
> implementation plan.
> The normative specification remains the authority for published claims and
> semantics; implementation-specific details here do not amend it.

## Why

The v0.1 transport-plugin ABI accreted three things that have come apart, and
a review (see the reasoning below) showed the current shape encodes the wrong
model:

1. **Node construction/lookup leaked into the transport vtable.** `open` /
   `close` / `get_endpoint` let a transport `dlopen`+`fss_create`+register a
   node. But construction has already moved to the topology/agent path
   (`OP_CONSTRUCT` via the model loader). For shm/zenoh data edges these three
   members are vestigial: the agent constructs the node and hands the transport
   an already-resolved local endpoint. The transport does not need a node
   registry to do its job.

2. **`bind` + `set_sink` are two asymmetric verbs for one operation.** A
   cross-process edge is wired by the sender calling `bind(local, locator)` and
   the receiver calling `set_sink(local)`. This is really *one* operation
   ("attach my endpoint to this channel, in this direction") split by an
   accident: `bind` conflates "attach outbound" with "resolve the peer," and
   peer-resolution only means anything in-process (`send = peer->receive`).
   Better names would already have been `bind_initiator` / `bind_target`.

3. **The channel-vs-medium model is wrong.** v0.1 forces point-to-point by
   *naming convention* (a synthetic per-edge token `e3_5`, dressed in a
   `create-rr:`/`attach-rr:` dialect that leaks shm's create-vs-attach
   asymmetry into the orchestrator's vocabulary). But the domain has **buses**
   (our own worked example is a CAN bus, modelled as a chain of p2p hops, which
   does not capture broadcast). Point-to-point should be the *degenerate* case
   of a general multipoint channel, not the atom.

## The corrected model (the decisions)

These were settled in review. Read them as the spec for the rework.

### D1. Channels are general and multipoint; point-to-point is the 1:1 case.
A **channel** is a first-class object created on a transport. Endpoints
**attach** to it in a **role**. A 1-initiator/1-target channel is the
degenerate case; a bus (many initiators and/or many targets) is the general
case. We commit to multipoint channels as *the* model and retire the per-edge
p2p link as the unit of wiring.

### D2. Channels are one-directional, with an optional round-trip write-back.
This is unchanged from v0.1 and is load-bearing (`abi/include/fss/transport_abi.h`
return-path contract; `docs/spec/04-protocols.md` directionality). A channel
carries initiator → target. Round-trip is **not** bidirectional: it is the same
carrier returning to *its* originator with `mutable_by:'target'` bytes mutated
in place. True B-originates-to-A needs a *separate* channel. Because direction
is real, each attach must state its role. A `BIDI` role (a target that may also
originate) is **deferred to a later step**; start with `INITIATOR` / `TARGET`,
which covers everything we have plus fan-out.

`round_trip` is computed by the composition compiler or binder from the
planned carriers (`mutable_by:'target'`), then passed to `channel()` as a
medium-compatibility hint.  The transport does not receive the atom semantics;
carrier ids and optional id hints do not let it infer them.  It can provision a
reply path from the hint, but it is not the semantic authority.

### D3. Carrier type on a channel is INDICATIVE, not prescriptive.
**Correction to an earlier over-strong idea.** A channel may carry *many*
different carriers; different initiators may send *different* ones; the
orchestrator may not know the set at wiring time. So we do **not** gate wiring
on a declared carrier type. At most, channel creation MAY accept an *optional,
indicative* list of carrier ids to enable *best-effort* checking where the
knowledge exists. The list is a hint, never a contract.

### D4. The round-trip ⇒ single-target constraint is enforced where the
knowledge exists, not at orchestrator time.
A round-trip carrier's write-back must return unambiguously to its originator.
Therefore a round-trip carrier flowing on a channel with **more than one
target** is an **error** — you would need a content router, which is a *model*
concern, not a transport concern (we route by endpoint membership only, never
by content). Because of D3 the channel may not know at creation time whether a
round-trip carrier will flow, so this is enforced **when a round-trip carrier
is actually sent** on a channel that has >1 target (a runtime/attach-time check
where arity and carrier-directionality are both known), not as an
orchestrator-time type gate. One-way carriers have no such limit (broadcast /
fan-out / merge are all fine). This *generalises* the existing hand-stated
fan-in/out rule (`docs/spec/00-object-model.md`: "write-back protocols are
round-trip and MAY fan in, never broadcast") into a channel-arity property.

### D5. The transport-plugin ABI stays (it is the dlopen contract).
**Correction to an earlier over-collapse.** Something must `dlopen` a transport
and hand back its factory + vtable; that is the transport-plugin ABI and it does
not disappear. What *does* dissolve is **topology-as-a-separate-C-ABI**: topology
is not a fourth C contract, it is a **carrier** (`control::instr`, today) an agent
**pulls** from a control channel, whose opcodes drive transport-plugin calls.
The endpoint ABI (move a carrier) and the artefact ABI (plugin construction
symbols) also stay — they are irreducible and orthogonal to control.

**There is NO reserved topology endpoint on the artefact.** (Correction to the
earlier "well-known control endpoint" framing.) The agent is given a transport
and a **control channel name** (in the breadcrumb); it attaches to that channel
and **pulls** its instructions — it sends a request and receives the next
`control::instr` as the round-trip reply, applying each until told to run. This
makes control the SAME pull pattern as config (`docs/spec/...` config-pull, see
[[fss-config-pull-design]]): a puller and a single responder, fan-in of pullers.

Consequences:
- **The responder is stateful.** It holds the plan and a per-agent cursor,
  handing each agent its next instruction in order. This is where sink-first
  ordering (construct before attach, sinks before sources) now lives — moved
  out of the orchestrator's push loop and into the responder. It may reply "not
  yet, ask again" to gate ordering.
- **In-proc is the degenerate case:** the control channel collapses to the
  identity call, so the pull is an ordinary function return. The v0.1 direct
  in-proc drive falls out naturally, no special path.
- **Asker identity rides in the carrier, NOT the channel name — but the agent
  must be GIVEN that identity.** A shared control channel (many agents pulling
  one responder) is a round-trip fan-in (legal: round-trip limits *targets*, not
  initiators). The responder needs to know who is asking; that identity is a
  `control::` field on the request, so the **channel name stays an opaque token
  with no grammar**. The artefact cannot invent its own identity (the responder's
  per-agent cursor keys on an orchestrator-chosen value), so the identity is
  **orchestrator-assigned and handed over in the breadcrumb**: the breadcrumb
  grows a third field, `{ transport, channel, self-id }`. `self-id` is
  **optional** — needed only for a shared (N:1) channel; for a per-agent (1:1)
  channel the channel name already identifies the one asker and `self-id` is
  redundant. It stays its own breadcrumb field and its own `control::` carrier
  field (a value, not a naming convention), so the channel name needs no syntax.
  1:1 and N:1 control are then the same mechanism; the identity field is simply
  ignorable when the channel is already 1:1.

### D6. Orchestrator provisions the fabric up front; the agent pulls to attach.
The orchestrator builds out whatever transport fabric a federation needs
*beforehand*. An **attach instruction (pulled by the agent) pulls in the
transport plugin** (lazy `dlopen`) but does **not** try to build the whole
transport mechanism at that moment — it **assumes the fabric is there** and
obtains any location/rendezvous information from the control channel. So
"provision fabric" (orchestrator, ahead of time) and "attach an endpoint to a
channel" (per-endpoint, pulled over the control channel) are cleanly separate
steps. Because the agent pulls, it drives its own bootstrap timing; the
orchestrator does not retry-until-agent-exists.

### D7. Channel identity is an arbitrary name; type+arity does the policing.
Keep the arbitrary channel name (like today's `e3_5`). The earlier worry "why
can't 3 things attach to `e3_5`?" is answered by D1 (multipoint is *intended*)
plus D4 (round-trip ⇒ single target is the only hard limit, enforced by
directionality) — **not** by a naming trick (the rejected "name the channel
`a.foo:b.foo`" pair-scheme, which would have re-coupled the transport to
endpoint names and blocked legitimate buses).

## Target ABI shape

Reduced transport-plugin vtable (illustrative, C):

```c
/* factory symbols (unchanged in spirit) */
const char      *fss_transport_name(void);          /* short stable name */
uint32_t         fss_transport_abi_version(void);   /* packed major/minor */
uint64_t         fss_transport_capabilities(void);  /* required vtable bits */
fss_transport_t *fss_transport_create(const char *config);
void             fss_transport_destroy(fss_transport_t *t);

struct fss_transport {
    void *self;

    /* create/find a channel on this transport, by arbitrary name.
     * `carrier_hints` (may be NULL) is an INDICATIVE list (D3) for
     * best-effort checking, never a contract. create-vs-attach of the
     * underlying medium (shm ring owner vs attacher) is TRANSPORT-PRIVATE:
     * first participant up creates, the rest attach (retry-until-present). */
    fss_channel_t *(*channel)(void *self, const char *name,
                              const fss_carrier_id *carrier_hints, size_t n);

    /* attach a local endpoint to a channel in a role (D2).
     * role = INITIATOR: install `local->send`/`send_ctx` (outbound).
     * role = TARGET:    deliver channel arrivals to `local->receive`.
     * In-proc 1:1 fast path: INITIATOR attach resolves the single target and
     * sets send = peer->receive (transport steps off); TARGET attach is a
     * no-op.  On a SECOND target attach (one-way), the transport must tear
     * down the identity binding, reinsert itself as a fan-out shim, and route
     * every send through it thereafter.  On a round-trip carrier with >1
     * target, error (D4). */
    int (*attach)(void *self, fss_channel_t *ch,
                  fss_endpoint_t *local, fss_role role);

    /* punctual one-shot: deliver a single carrier to a channel with no
     * standing attach (lifecycle pings, config to a well-known port). */
    int (*send_to)(void *self, fss_channel_t *ch, fss_carrier_t *carrier);
};
```

For the implemented binary-loader ABI, a host admits a plugin by resolving and
calling the version and capability exports first, checking compatible major and
minor plus `FSS_TRANSPORT_CAP_CHANNEL_ABI`, then resolving the factories and
name, verifying the selected name, and only then creating or using its vtable.
This is an implementation ABI rule, not a new protocol-level rule.

Gone from the vtable: `open`, `close`, `get_endpoint` (node construction/lookup
— moves out, per issue 1 / D5). Gone from the orchestrator's vocabulary: the
`create-rr:` / `attach-rr:` dialect (create-vs-attach becomes transport-private,
per D6/target shape).

Topology is **not** in this struct. It is the `control::instr` carrier an agent
**pulls** from its control channel, whose opcodes (construct, attach-initiator,
attach-target, …) the agent interprets into the calls above. The agent is the
puller/initiator; a stateful responder holds the plan and replies with the next
instruction (D5).

## In-process implementation states (the tricky part, D2/D4)

The in-proc transport must track channel arity and transition:

- **unwired** → no attaches yet.
- **identity (1:1)** → exactly 1 initiator + 1 target, `send = peer->receive`,
  transport off the path (the v0.1 fast path, preserved).
- **broadcast-shim (≥2 one-way targets)** → on the 2nd target attach, tear down
  the identity binding, put the transport back in the path as a fan-out shim;
  every send iterates targets.
- **refuse** → a round-trip carrier on a channel with >1 target is an error
  (D4); the second target attach on an already-round-trip-carrying channel, or
  a round-trip send on a multi-target channel, fails.

This transition (identity → shim) is the real implementation cost of a true
general channel. It is unavoidable and must be implemented, not designed around.

**Resolved during execution (R4, post-R1).** R1 moved 1:1 in-proc wiring OUT of
the transport (the agent direct-wires `send = peer->receive` via the placement
facility; inproc's `channel`/`attach` are NULL), so there was no in-proc
transport left to host the shim. Decision: **reintroduce a routing in-proc
transport** carrying `channel`/`attach` with the arity state machine above. The
1:1 fast path stays the R1 agent-side direct-wire (identity, transport off the
path); a channel with a SECOND one-way target is realised THROUGH the
reintroduced inproc transport as a fan-out shim. So identity(1:1) = agent
direct-wire (unchanged), broadcast-shim(≥2) = inproc transport routes. The
existing 1:1 matrix edges keep the direct-wire path untouched (green preserved);
only the new multipoint bus test exercises the routing inproc transport.

## Execution increments (each: clean build; matrix green; oracle untouched; commit)

Order chosen so the tree stays green and each step is independently verifiable.
The v0.1 matrix (`ctest -L agent_matrix -E zenoh -j6 --repeat until-pass:3` =
256/256; `-R zenoh -j4 --repeat until-pass:4` green; `platform_matrix` oracle
untouched) is the guardrail for EVERY increment until the model changes; where an
increment changes wire behaviour, update the oracle expectation deliberately and
say so in the commit.

- **R0 — audit (read-only, do first).** Enumerate every call site of transport
  `open`/`close`/`get_endpoint`, `bind`/`set_sink`, and the `create-rr`/
  `attach-rr` dialect (`fss_chan_server_cfg`/`client_cfg`/`bind_loc`) across
  `reference/transports/{inproc,shm,zenoh}`, `reference/runtime/agent-core`, `reference/runtime/orchestrator`,
  `reference/runtime/launcher`, and the standalone/no-topology path. Classify each as
  (data-edge / control / construction / standalone-only / dead). Output the true
  blast radius. NO code change. This sizes R1–R4 and confirms `open` is dead for
  data edges before removing it.

- **R1 — remove node construction from the transport vtable (issue 1 / D5).**
  Delete `open`/`close`/`get_endpoint` from `struct fss_transport`. Move the one
  legitimate user (inproc-as-construction-transport / standalone path) to a
  separate placement/loader interface used by the topology layer (the agent
  already constructs via `A->loader->construct`; make that the sole path).
  Data-edge wiring already hands the transport a resolved local endpoint, so
  nothing on the data path should regret this. Matrix must stay green.

- **R2 — collapse `bind`+`set_sink` into `attach(local, role)` (issue 2 / D2).**
  Introduce the `attach` verb with `INITIATOR`/`TARGET` roles; reimplement the
  three transports on it (inproc: identity fast path for INITIATOR, no-op
  TARGET; shm/zenoh: outbound vs sink). Rewire `OP_SERVER`/`OP_CLIENT` in the
  agent to `attach(..., TARGET)` / `attach(..., INITIATOR)`. The control channel
  uses the SAME verb (orchestrator = INITIATOR, agent = TARGET), so this unifies
  control bootstrap and data edges. Keep `send_to`. Matrix green.

- **R3 — first-class channel object; create-vs-attach goes transport-private
  (D1/D6/D7).** Add `fss_channel_t` + the `channel(name, hints)` factory. The
  orchestrator names channels (arbitrary token) and stops emitting the
  `create-rr`/`attach-rr` dialect; the transport decides internally who creates
  the medium (first up creates, rest attach; the existing `create_retry` already
  provides the retry). `attach` takes a channel handle, not a locator string.
  Matrix green.

- **R4 — multipoint + arity enforcement (D1/D3/D4).** Implement the in-proc
  identity→broadcast-shim transition on 2nd one-way target; implement the
  round-trip⇒single-target error. Add the optional indicative `carrier_hints`
  and best-effort checking. Add a NEW multipoint test (e.g. a real CAN *bus*:
  one initiator, N targets, one-way `can::frame`) alongside the existing chain
  cells, and a negative test (round-trip carrier + 2 targets ⇒ refused). Update
  the oracle to include the bus topology deliberately.

- **R5 — topology as a pulled carrier; retire the reserved control endpoint
  (D5/D6).** Make explicit in code and docs that "topology" is `control::instr`
  an agent **pulls** from a control channel named in its breadcrumb, not a fourth
  C ABI and not a reserved endpoint the artefact must expose. Move the plan into
  a **stateful responder** that hands each agent its next instruction in order
  (this is where sink-first ordering now lives, moved out of the orchestrator's
  push loop). Put the **asking agent's identity in a `control::` request field**,
  and carry that identity to the agent as an **optional `self-id` breadcrumb
  field** (orchestrator-assigned; needed only for a shared N:1 channel, redundant
  for a per-agent 1:1 channel), so a shared control channel needs no channel-name
  grammar. In-proc control collapses to the identity call
  (pull = function return). This unifies control with the config-pull pattern.
  Fold the "four ABIs" framing to "endpoint + artefact + transport-plugin C ABIs,
  driven by pulled control carriers." Behavioural change (push→pull, stateful
  responder); update the oracle/expectations deliberately; matrix green.

  **Executable detail (refined post-R1..R4, incl. the "whole plan in one pull"
  decision — user, 2026-07-18).** The control carrier becomes a JSON PLAN
  document holding ALL of an agent's steps, pulled ONCE (not a drip-feed of
  per-op structs). This both flips push→pull and collapses N control round-trips
  to one. Concretely:
  - **New carrier `control::plan`** (`atoms/control/PlanJson.libsonnet` = a large
    inline char slot, e.g. 2048; `protocols/control/plan.jsonnet` =
    `{ control::PlanJson, control::Status }`, round-trip via Status
    `mutable_by:'target'`). Regenerate id/meta via `fss_compile.py`; add a build
    rule beside `protocols/control/`. The existing `control::instr` (288 B
    struct) STAYS as the EXECUTION representation — the plan JSON is parsed into a
    sequence of `fss_ctrl_instr`, each run through the UNCHANGED `agent_do_instr`.
  - **JSON plan schema** (compact keys): an array of step objects
    `[{op,a0,a1,a2,xport,rt,fl}, ...]` in sink-first order, terminated by a DONE
    step. Builder in the orchestrator (serialize each group's steps); a small
    fixed-schema parser in `agent_core.c` (array/object walk over the known keys;
    the breadcrumb reader is the seed).
  - **Role inversion (config-pull pattern).** Today the agent OWNS the control
    channel (TARGET, breadcrumb owner) and the orchestrator PUSHES (INITIATOR).
    For pull: the ORCHESTRATOR owns the control channel (TARGET) and installs a
    `control::plan` RESPONDER (analogous to `fss_config_responder`: on a pull
    request, write the agent's plan JSON into the `PlanJson` slot + `Status`=Ok);
    the AGENT attaches as INITIATOR and pulls its plan once, parses, executes via
    `agent_do_instr`, then arms/runs. Per-agent 1:1 control channel (as today) ⇒
    the channel name identifies the asker, so NO self-id field needed (D5's
    self-id is only for a shared N:1 channel, which we do not use).
  - **In-proc = pull is a function return.** The p0 agent (same process as the
    orchestrator) is handed its plan JSON by a direct call
    (`fss_agent_run_plan(A, json)`), no channel.
  - **Ordering becomes decentralised — the key risk.** With per-agent plans run
    independently, cross-agent ordering (dev's channel exists before ctrl
    attaches; sinks armed before sources) is NO LONGER orchestrator-sequenced. It
    must fall out of (a) the channel attach-RETRY (`fss_chan_open_retry`, attacher
    spins until owner up — already in place) and (b) the `fss/sim` readiness wave
    (a source waits for downstream readiness before running — already in place,
    [[fss-time-sync]]). This is the whole point of pull, but it leans entirely on
    retry+readiness to absorb races and may aggravate the known cross-process
    `b_transport`-vs-time-SUSPENDED transient; budget for `--repeat` and verify
    ordering carefully.
  - **shm slot cap.** Bump `SLOT_DATA_CAP` (shm.c) to fit the plan JSON (~2 KB);
    it currently notes it must fit the 288 B `fss_ctrl_instr`.
  - **Suggested low-risk order:** (1) plan format + parser + `fss_agent_run_plan`
    (additive, unit-testable); (2) orchestrator plan-builder; (3) switch the
    IN-PROC p0 agent to build-plan + `fss_agent_run_plan` (pull=function return,
    no channel change) and verify the matrix (p0 is in every cell); (4) the remote
    role-inversion cutover + `control::plan` carrier + responder + shm cap, then
    full-matrix verify. Steps 1–3 carry near-zero risk; step 4 is the risky cut.
  - **One-artefact-per-language (user goal, verified NOT yet true — 3 `.so`/lang
    today).** The JSON plan naturally lists multiple constructs for one agent, so
    it is the enabler for a single per-language artefact building 1/2/3 nodes —
    but merging the 12 CAN `.so` (init/ctrl/dev × 4 langs) into 4 is an ARTEFACT
    restructure that belongs in Stage 2 (R6+ native-interop), not R5.
  - **BLOCKER found while starting step 3 — the readiness-latch interpose.** The
    orchestrator installs the federation-readiness latch (`fss_fed_status_receive`)
    directly on the ROOT node's `fss/sim` endpoint MID-SEQUENCE — after the
    construct loop, before wiring/arm (`engine.c` ~line 165). Under pull, the p0
    agent runs its whole plan atomically (construct→wire→arm→done), so the
    orchestrator cannot interpose the latch install between p0's construct and
    p0's arm. Therefore the "switch in-proc p0 first" slice is NOT clean — it
    breaks the readiness latch. Options to resolve BEFORE implementing step 3/4:
    (a) split p0's plan into construct+wire | arm+done, letting the orchestrator
    install the latch between (a mild drip for p0 only); (b) make readiness a
    thing the agent reports over a channel rather than an orchestrator-installed
    send hook (bigger, cleaner — the source agent reports UP over `fss/sim` to a
    responder the orchestrator owns, same pull/responder pattern); (c) have the
    plan carry a "wire the root latch" step the p0 agent executes against an
    orchestrator-provided endpoint. (b) aligns best with the pull model. Pin this
    down first; it is why R5 is not a mechanical cut.

## Open questions to resolve during execution

- **BIDI role.** Deferred (D2). When added: is a BIDI channel two one-way
  channels under one handle, or a genuinely symmetric medium? Decide before
  implementing; do not let it complicate INITIATOR/TARGET.
- **Where exactly the round-trip⇒single-target check fires (D4).** attach-time
  (if the channel already knows a round-trip carrier flows) vs first
  round-trip send. Probably both: refuse the obvious case early, catch the rest
  at send. Pin this down in R4.
- **Standalone/no-topology path after R1.** Confirm the direct-`bind` integrator
  path (no agent) still has a construction route once `open` leaves the vtable.

## TODO (major, separate from the transport rework): native-construct interop

**The non-C++ bindings currently cheat.** The Rust, Python, and FMI CAN
initiators/controllers do *not* interoperate through their own native
constructs — they hand-build the **TLM generic-payload flat image**: they
hardcode `GP_BYTES = 72` and write the `bus::` atoms at the compiler-generated
`FSS_TLM_OFF_*` offsets, shipping `FSS_TLM_FLAT_ID`/`FSS_TLM_FLAT_META`
(`reference/examples/fss-artefacts/fss_can_initiator_paced_{rust/src/lib.rs,python/can_initiator_paced.py,fmi/can_fmi_tx_paced.c}`).
So all four languages emit the *same* TLM-derived carrier. That demonstrates
"four languages can produce/consume one agreed carrier," but **not** the actual
goal: that independently-modelled components interoperate from their *native*
representations. The FMI path especially should originate an **FMI-LS-BUS CAN
frame** (the native FMU bus construct) and let FSS carry/bridge it, rather than
faking a TLM GP.

Why it matters and why it's painful:
- **It is the real test of FSS.** Native-to-native (e.g. FMI-LS-BUS CanTransmit
  ↔ SystemC TLM ↔ a Rust-native struct) is what proves the meta/atom bridge
  earns its keep. Faking TLM everywhere sidesteps the bridge entirely.
- **Performance will move, likely for the worse.** Today the non-C++ side is a
  zero-bridge memcpy into a TLM-shaped buffer; genuine native carriers mean real
   per-hop translation (atom-resolve or an explicit bridge), so the current
   performance measurements will change. Budget for it; re-measure.
- **Painful:** each binding grows its own native carrier composition + a bridge
  to/from the neighbour's carrier; the CAN example's hop-1 may cease to be a
  single shared `tlm::transaction`. Touches the atom catalogue (CAN atoms as
  first-class, not inline `can::Ide`), the compositions, and every non-C++
  artefact.

When this lands, revise the worked-example framing: all four languages currently
emit `tlm::transaction`, which is the shortcut described above.

## Cross-references

- v0.1 code: `abi/include/fss/transport_abi.h`,
  `reference/transports/{inproc/inproc.cpp,shm/shm.c,zenoh/zenoh.c}`,
  `reference/runtime/agent-core/agent_core.c` (`OP_SERVER`/`OP_CLIENT`/`OP_CONSTRUCT`),
  `reference/runtime/orchestrator/{orchestrator.c,engine.c}` (`fss_chan_*_cfg`, `drive`).
- Existing rules this generalises: `docs/spec/00-object-model.md` (fan-in/out),
  `docs/spec/04-protocols.md` (directionality derived from `mutable_by:'target'`),
  `docs/spec/05-abi-conventions.md` (current ABI conventions).
