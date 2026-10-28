# Binding and phases

> **Historical design note (2026-06).** This records the pre-channel transport
> vocabulary (`open`, `get_endpoint`, `bind`, and `set_sink`).  That ABI was
> superseded by the implemented `channel`/`attach`/`send_to` ABI.  Except where
> an item is explicitly labelled current, the text below — including its
> `open`/`bind`/`get_endpoint` flows and implementation increments — is
> historical, not a current ABI definition.  Preserve this note for its phase
> reasoning; use `abi/include/fss/transport_abi.h` and `channel-abi-rework.md`
> for current transport behavior.

Historical design note for the former bind mechanism and its relationship to
simulation phasing.  It records the phase model used by the former design. The
decisions and terminology below describe the superseded ABI, not the current
transport interface.  The retained phase reasoning must be translated to
`channel`/`attach` when used with the current ABI.

## Problem

Three things the former design did not handle:

1. **Chicken-and-egg.** If bind is a protocol (a carrier exchange), A must
   send a bind-carrier to B — but the channel to B is what bind creates.
2. **Multi-socket.** A `multi_passthrough_*` socket takes N bindings, N =
   number of FSS peers, unknown until binds are declared.  Sockets cannot
   be added after `end_of_elaboration`, so adapter lanes must be built
   *as each bind arrives*, inside the artefact, on its kernel thread.
3. **Atomic create+start.** At the time, the SystemC artefact constructed `top`
   and runs `sc_start(SC_ZERO_TIME)` inside `fss_create`
   (`reference/languages/systemc-tlm/include/fss/systemc_tlm/artefact.h`).  Elaboration is
   over before `get_endpoint` returns, so there is no window to bind in.

## The three relationships

Bind is well-founded only if exactly one connection is *given*, not
negotiated.  That connection is the endpoint's link to its own transport.

| Relationship            | How established                | When                     |
|-------------------------|--------------------------------|--------------------------|
| endpoint ↔ local transport | **primordial** (by construction) | birth — never negotiated |
| transport ↔ transport      | **configured** (deployment wiring) | launch — not model logic |
| endpoint ↔ peer endpoint   | **negotiated** (bind protocol over the above) | bind window |

- **Primordial.** An endpoint is *created by* its transport (in-proc:
  `dlopen` + `fss_create`; Zenoh: it joins a session).  There is always a
  local transport and the endpoint is always already attached.  This is
  the channel the bind protocol's first hop rides — it needs no prior
  bind, which is what dissolves the chicken-and-egg.
- **Configured.** Transport↔transport topology is a deployment concern
  (the in-proc transport sees everything it loaded; a Zenoh transport
  joins a session).  Endpoints never negotiate it.  This is the one place
  a configured topology legitimately exists; keeping it out of model logic
  keeps the endpoint ABI minimal.
- **Negotiated.** The peer relationship is the *outcome* of the bind
  protocol, carried over the primordial + configured channels.  It MAY
  resolve to a direct channel (transport drops out of the data path).

The loader/router/memory example used by the former design follows this model:
the loader does not find the memory — the **router orchestrates the local
bind**.  "Who
can cause A to be bound?" → A's local transport.

## Phase vs State

This note distinguishes **Phase** (state of a single homogeneous simulator)
from **State** (the heterogeneous federated whole).  Binding lives in both,
and the boundary is load-bearing:

- **Intra-simulator (Phase).** A SystemC kernel imposes a *global*
  `end_of_elaboration` barrier per simcontext; `sc_start` is all-or-
  nothing.  So all of *this* simulator's FSS lanes MUST bind before *this*
  simulator's elaboration closes.  This is the kernel-forced **mechanism**
  (the phase split: park before `sc_start`, close the window on demand) —
  but its *trigger* is still a carrier (a `sim::event{Started}` to the
  `fss/sim` port), not an ABI verb.  Crossing the barrier is expressed in
  the protocol; only the act of crossing it lives in the binding.
- **Inter-simulator (State).** Fully local and negotiated: each simulator
  only needs *its own* connections bound; the federation
  advances collaboratively (Demand / Request / Accept / Refuse).  No
  global "system is bound" barrier.  Modelled with `sim::` carriers, not
  ABI.

A is "bound" when all of *A's* declared connections are bound — a local
predicate, not a global one.

## Bind

Bind establishes a channel from a local endpoint to a named peer, resolving
the peer locator **once**.  Two irreducible transport primitives bracket it:
`get_endpoint` (resolve a local handle — the thing carriers reach) and
`bind` (establish the channel — `connect()`).  Neither can be a carrier: a
carrier needs a channel to travel over, and these *make* the channel.

1. The host/launcher calls **`bind(local_endpoint, peer_locator)`**.  The
   transport resolves `peer_locator` once (in-proc: via the artefact's
   `get_endpoint`; cross-transport: via the configured topology) and records
   the resolved peer on `local_endpoint`.  This is where the peer is named —
   the point of binding.
2. Thereafter **`send(local_endpoint, carrier)`** delivers to the bound peer
   with no further addressing.  The string was resolved at step 1, never per
   message.

*Endpoint vs lane.* The artefact **declares its endpoints (named ports) at
construction**, so `get_endpoint` resolves them in the bind window.  For a
multi-socket peer, the actual SystemC socket (`multi_passthrough_*`
sub-binding) is built **per bind** on the kernel thread (endpoints have
simcontext affinity, so the host thread cannot construct the socket — the
bind is marshalled onto the kernel thread).

**Discovery / direct-path negotiation (future, not built).** Once a channel
is bound, the two endpoints MAY exchange a capability carrier *over it* —
each announcing its carrier-id(s) — so that matched same-process peers agree
to a direct route (in-proc SystemC↔SystemC: pass the live
`tlm_generic_payload*` and cast on receipt — the zero-copy fast path, **Path A**
in `docs/spec/05-abi-conventions.md` terms; FSS marshalling drops out).
Mismatched ids / cross-process stay on the resolve-by-meta path (**Path B**).
This is a carrier *over* the channel — the layer where the earlier "bind as
protocol" intuition correctly lives — and it moves the Path-A-vs-B choice from
per-send to per-bind.  Deferred until a federation
needs it.

## Reference ABI surface

The guiding rule: **the ABI is only what cannot be a carrier.** Two
transport operations are irreducible because they *establish the channel a
carrier travels over* — they cannot themselves ride a carrier:

- **`get_endpoint`** — resolve a local handle (the thing a carrier is
  delivered *to*).  The `socket()`.
- **`bind`** — establish a channel from a local endpoint to a *named peer*,
  resolving the peer locator **exactly once**.  The `connect()`.

Everything else — lifecycle, config, time, capability discovery — is a
carrier sent **over an established channel** to a port.

**Historical transport ABI** — `open` / `close` / `get_endpoint` / `bind` /
`set_sink` / `send_to`:
- `bind(local_endpoint, peer_locator)` — resolve `peer_locator` once and
  **install the outbound channel on the endpoint** (`local->send` /
  `local->send_ctx`).  This is where the peer is named; the point of binding.
- `send_to(peer_locator, carrier)` — one-shot resolve + deliver, no standing
  channel (nothing bound, nothing to mutate).  For occasional point-to-point
  traffic (lifecycle pings).
- **No `transport->send`.** On a bound channel the model delivers with
  `endpoint->send(endpoint->send_ctx, carrier)` — a direct call to the
  installed callback, no transport on the hot path (a binding may wrap that
  one-liner; the ABI ships no helper).  (A locator-addressed `send` would be
  connectionless — it would mean we never bound; that was the wrong turn.)

**Historical artefact ABI** — `fss_create` / `fss_get_endpoint` / `fss_destroy`:
- `fss_create` constructs and **parks in the bind window** (no `sc_start`).
  No mid-elaboration pause, so **no psystemc dependency**.
- `fss_get_endpoint(node, name)` is the resolution floor.
- **No `fss_start`** — closing the bind window is a `sim::event{Started}`
  carrier delivered to the `fss/sim` port (§Standard ports).

`fss_endpoint_t { self, receive, send, send_ctx }`: inbound and outbound are
symmetric (same `fss_carrier_fn` signature, same return contract).  The
**artefact** installs `receive` (inbound); the **transport** installs
`send`/`send_ctx` at `bind` (outbound).  `send` returns what the peer's
`receive` returned.  In-proc 1:1 bind is the identity
`local->send = peer->receive; local->send_ctx = peer->self` — no shim, and
nothing for a model to bypass (the channel is private behind `send_ctx`).

**Uniform model: bind-then-send.** Data and control are the same shape —
bind a local endpoint (to a data socket *or* the `fss/sim` port), then
deliver via `endpoint->send(...)`; or use `send_to` for a one-shot.  One
delivery signature, one mental model.

## Standard ports

Two kinds of endpoint:
- **Data endpoints** — *model-provided*, *model-named* (`socket`, `can_rx`);
  carry domain carriers (TLM, CAN).  A peer binds to these and sends.
- **Standard control ports** — *binding-provided*, *FSS-named*; carry FSS
  protocol carriers.  The model does not implement them; the language
  binding does (its handler drives the kernel).  A peer binds to these the
  same way (uniform bind-then-send) and sends `sim::event` / `config::`
  carriers.

Naming: a **reserved `fss/` segment** under the node.  Model authors MUST
NOT use an `fss` segment.  Defined ports:

| Port | Carrier | Required | Role |
|------|---------|----------|------|
| `<node>/fss/sim`    | `sim::event` (`sim::EventCode`)   | **MUST** | lifecycle: `Bound → Elaborated → Started → Paused/Resumed → Stopped`.  Replaces `fss_start`: the `fss/sim` handler runs `sc_start`/`sc_stop`. |
| `<node>/fss/config` | `config::param_request`           | MAY      | parameter get/set.  Advertised *by presence*: `get_endpoint(".../fss/config")` returning null ⇒ unsupported. |

Time advance is a **SEPARATE** protocol on its own port (a dedicated
time-window carrier `(t_lower, t_upper)`), NOT folded into `fss/sim` — lifecycle
and time-sync are distinct concerns.

## Collaborative state protocol (federation level)

The `fss/sim` port is where this note's collaborative phasing lives — it can
Accept / Refuse / Demand a transition, not just obey:
- `sim::EventCode`: `Bound, Elaborated, Started, Paused, Resumed, Stopped`;
  `sim::Status { Ok, Failed }`.  (Time advance is a separate protocol.)

A simulator drives its own `Bound → Elaborated → Started` locally (the
phase-split mechanism, triggered by an `fss/sim` carrier).  Across
simulators, the same carriers negotiate State: a component
asserts/accepts/refuses an event; the federation advances only when every
*connected* peer agrees.  The multi-message choreography (ordering,
time-grant negotiation, async responses) is open issue #7 and stays
deferred — this note settles the bind-time + lifecycle-trigger model.

## Decisions

- **[HISTORICAL DECISION — revised 2026-06-02] Bind is a transport verb, not a carrier.**
  An earlier iteration made bind a carrier and `send` locator-addressed;
  that was a "bootstrap yourself by your own laces" error — a bind that has
  to be carried over the channel it is meant to create cannot be the thing
  that creates it, and a `send` that re-addresses every message means no
  bind ever happened.  Corrected: `bind(local, peer_locator)` resolves the
  peer **once** and establishes the channel; `send(endpoint, carrier)` is
  channel-based.  Bind joins `get_endpoint` as an irreducible primitive (it
  establishes the channel, so it cannot ride one).  The expandability that
  motivated "bind as carrier" is preserved one layer up: capability /
  discovery (e.g. announcing carrier-ids to negotiate the Path-1 direct
  route) is a carrier exchanged *over* the established channel — future
  work, not built yet.
- **[DECIDED] Two separable properties, split:**
  - *(a) Identify-or-reject (safety) = MUST.*  A receiver MUST identify a
    carrier — by `id`, or by resolving its meta by qname — before
    consuming it, and MUST reject what it can neither recognise nor
    resolve (no reinterpreting unknown bytes at guessed offsets).  **This
    resolves open issue #2.**
  - *(b) Direct-path short-circuit (performance) = MAY.*  The bind-time
    negotiation where matched same-process peers bypass FSS marshalling
    (in-proc SystemC↔SystemC → Path A) is a permitted optimisation.  The
    carrier (meta-resolve) path is the mandatory interoperability floor.
  Rationale: mandate the interop floor + the safety rule; leave
  performance permitted.  Mandating an optimisation raises the conformance
  bar without improving interop and would be unspecifiable portably
  ("direct" differs per language: GP cast / Rust call / …).
- **[DECIDED] Locator = transport-relative hierarchical name.**  A
  slash-separated path (`<node-path>/<endpoint>`), Zenoh-key-compatible,
  embeddable in a federation namespace.  **No transport scheme** — the
  locator never names *how* to reach the peer (consistent with "endpoints
  don't choose transports"; cross-transport routing is configured
  topology).  Direction set; exact grammar finalised when the first
  cross-transport federation forces the detail (today only in-proc, where
  the locator is effectively dylib + endpoint name).
- **[DECIDED] Lifecycle is a protocol, not an ABI verb.**  `start`/`stop`/
  `pause`/… are `sim::event` carriers sent over a channel bound to the
  `fss/sim` port, not C verbs.  `fss_start` is superseded.  The kernel
  mechanism (deferred `sc_start`, bind window) survives; only the *trigger*
  moves from a verb to a carrier.  (Bind itself stays a verb — see above; a
  carrier needs a channel, and bind makes one.)
- **[DECIDED] Standard ports** (§Standard ports): reserved `fss/` segment;
  `fss/sim` (lifecycle+time) = MUST, `fss/config` = MAY (advertised by
  presence — null `get_endpoint` ⇒ unsupported).  Two endpoint kinds:
  model-provided data endpoints vs binding-provided control ports.

## Implementation staging

**Increment 1 — SUPERSEDED by Increment 1.6 (below) — describes a design that was reversed; kept for history.** DONE (2026-06-01), ctest 9/9 green.  `fss::bind`
carrier id `0xa77eea52894cb077` (after core::Extensions rename + slot added); phase split + bind-as-carrier landed;
`sc_find_object` in the bind window confirmed valid pre-elaboration.
*(install the architecture; single-socket; tests stay green):*
- Phase split: `fss_create` stops running `sc_start`; add `fss_start`
  (artefact ABI + `transport.start` vtable + inproc plumbing + the
  `extern "C"` export wrapper each artefact emits).  Kernel thread parks
  in the bind window after constructing `top`; `fss_start` closes it.
- `get_endpoint` must resolve in the bind window (pre-`end_of_elaboration`)
  — `sc_find_object` over the constructed hierarchy.  **Verify** (the one
  real risk).
- Bind-as-carrier: define a minimal `fss::bind` control carrier (real
  composition + generated id `FSS_BIND_ID`, following the `tlm` pattern).
  `transport.bind` keeps its internal peer-routing wiring **and** delivers
  a bind-carrier to each side's `receive`; `endpoint_receive` recognises
  `FSS_BIND_ID` and returns (no dispatch).  Mechanism only — the carrier's
  discovery payload is unused until increment 2.
- Probe flow becomes `open → get_endpoint → bind → start → send`.

**Increment 1.5 — SUPERSEDED by Increment 1.6 (below) — describes a design that was reversed; kept for history.** DONE (2026-06-01), ctest 9/9 green.  ABI minimized:
- Removed `fss_start` (artefact ABI) + `transport.start`/`transport.bind`
  (vtable).  Transport ABI = `open`/`close`/`get_endpoint`/`send`;
  `send` is now `send(self, const char *dest, carrier)` (locator-addressed,
  resolved via inproc's `unordered_map` name→endpoint registry filled by
  `get_endpoint`).  `FSS_SEND_UNBOUND` → `FSS_SEND_NO_DEST`.
- `fss/sim` standard port: binding-provided; `get_endpoint("fss/sim")`
  returns a control endpoint whose `sim_receive` runs `start()` on
  `Started(2)` / `signal_stop()` on `Stopped(5)`.  `fss/config` is
  advertised by presence (no special code — unknown `fss/` name → null).
  FSS_SIM header generated (id `0x2b40a044b78453c3`).
- `load_probe` flow: `open → get_endpoint("socket"|"fss/sim") → bind-carrier
  to "socket" → Started to "fss/sim" → sends → Stopped`.
- ~~*Follow-ups (not breakage):* `docs/spec/05-abi-conventions.md` and the
  Rust/Python runtime mirrors still name the old verbs / `FSS_SEND_UNBOUND`
  — fold into the spec-promotion pass.~~ **DONE:** `docs/spec/05` already
  uses the corrected ABI (`send_to`, `FSS_SEND_UNBOUND`=1, no `fss_start`);
  no standalone Rust/Python language runtimes exist (only examples, which
  are also clean).

**Increment 1.6 — DONE (2026-06-03), ctest 7/7 green.**  Bind correction +
send-callback model:
- Endpoint: unified `fss_carrier_fn` (`int(void*, carrier*)`) for both
  `receive` and `send`; fields `{ self, receive, send, send_ctx }` (no
  `peer`).  `endpoint.h` is just the struct + typedef — no helper (delivery
  is `ep->send(ep->send_ctx, c)`; the probe keeps a local `fss_send` sugar).
- Transport ABI: `open / close / get_endpoint / bind / send_to`.  `bind`
  installs the outbound channel (in-proc: `local->send = peer->receive`,
  identity, no shim); `send_to` is the one-shot.  No `transport->send`.
- Deleted the bind *carrier* machinery (`protocols/bind/`, `atoms/bind/`,
  `FSS_BIND_ID`, the `endpoint_receive` bind branch).  Receive callbacks
  (`endpoint_receive`, `sim_receive`) now return `int` (status).
- `load_probe`: `bind(ep_data, "socket")` + `fss_send` for data; `send_to`
  for `Started`/`Stopped` lifecycle pings.
- Capability/discovery carrier (Path-1 negotiation over the channel) remains
  future (increment 2).

**Increment 2 (deferred):** per-bind SystemC **lane** construction on the
kernel thread (multi-socket); initiator-side artefact wrapping;
SystemC→SystemC federation; the capability/discovery carrier + Path-1
selection at bind.

## Non-goals (this note)

- Late joiners / mid-elaboration binding (needs the psystemc pause; this
  note is pre-`sc_start` only).
- Full sim-lifecycle choreography beyond bind (open issue #7).
- Time-grant negotiation (HLA TAR/TAG-style).
