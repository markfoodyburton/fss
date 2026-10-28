# 05 — ABI conventions

This section is **informative**.  The carrier mechanism (see
[01-carrier.md](01-carrier.md)), field types (see
[02-encodings.md](02-encodings.md)), atoms (see [03-atoms.md](03-atoms.md)),
and protocol identity (see [04-protocols.md](04-protocols.md)) are
the normative parts of FSS.  How a particular language binding or
transport implementation exposes them is convention.

The conventions documented here are the FSS reference C ABI
(`abi/`), shared by the in-process transport, the
SystemC binding, and the load_probe demo.  A pure-Rust FSS stack
would be free to define its own equivalents.

## Endpoint

An **endpoint** is a carrier sink (inbound `receive`) and, once attached as an
initiator, a carrier source (outbound `send`).  Inbound and outbound are symmetric — the
same callback signature — and installed by opposite owners: the **artefact**
installs `receive`; the **transport** installs `send`/`send_ctx` at attach
(as an initiator).  Reference C shape:

```c
/* both inbound (receive) and outbound (send); returns a status (FSS_SEND_*) */
typedef int (*fss_carrier_fn)(void *self, fss_carrier_t *carrier);

typedef struct fss_endpoint {
    void           *self;       /* inbound context (artefact-defined)        */
    fss_carrier_fn  receive;    /* inbound delivery; installed by artefact   */
    fss_carrier_fn  send;       /* outbound delivery; installed by attach; NULL = unattached */
    void           *send_ctx;   /* outbound context; transport-private       */
} fss_endpoint_t;
```

A model NEVER reaches into another endpoint; it delivers on an attached endpoint
by invoking the transport-installed callback — `ep->send(ep->send_ctx,
carrier)` — which returns exactly what the peer's `receive` returned (the
transport ferries the status back).  This one-liner is the whole delivery
operation; a language binding may wrap it in a convenience, but the ABI is
just the struct (no helper in the normative headers).  In-process 1:1 wiring
is the identity `local->send = peer->receive; local->send_ctx = peer->self`
— no shim, no transport on the per-send path (an INITIATOR `attach` on the
in-process transport installs exactly this).

Endpoints carry no protocol-type information.  The binder is
responsible for knowing what protocols speak between two endpoints;
mismatched topology bindings surface as runtime failures (the receiver's
carrier-id filter rejects the unexpected carrier).

The single `send`/`send_ctx` slot makes **fan-in** free: many sources (each with
its own slot, over its own transport instance) deliver into one `receive`.
**Fan-out** is provided by a multipoint channel: the transport, not the endpoint,
routes one initiator to many targets for a one-way protocol (the in-process
transport wires 1:1 as the direct identity call and inserts a fan-out shim on
the second one-way target).
See [00-object-model.md](00-object-model.md) §Fan-in and fan-out for the
capability and the normative no-fan-out-of-write-back rule.

## Transport

A **transport** hosts **channels** and joins endpoints to them.  Its two
irreducible jobs — `channel` (name/provision a channel) and `attach` (join an
endpoint to a channel in a role) — both *make* the channel a carrier rides, so
neither can itself be a carrier.  There is no `transport->send`: delivery on an
attached channel is the installed callback (`ep->send(...)`), not a transport
call.  (Node construction is **not** a transport job; it is the placement
facility's — see §Artefact.)  Reference C shape (vtable):

```c
typedef enum { FSS_ROLE_INITIATOR = 0, FSS_ROLE_TARGET = 1 } fss_role;
typedef struct fss_channel fss_channel_t;   /* opaque, transport-private */

typedef struct fss_transport {
    void *self;
    fss_channel_t  *(*channel) (void *self, const char *name, int round_trip,
                                int owner, const uint64_t *carrier_hints, size_t n);
    int             (*attach)  (void *self, fss_channel_t *ch,
                                fss_endpoint_t *local, fss_role role);
    int             (*send_to) (void *self, fss_channel_t *ch, fss_carrier_t *carrier);
} fss_transport_t;
```

`channel(name, round_trip, owner, ...)` creates or finds the channel `name` (an
arbitrary opaque token — never a carrier name; a channel may carry many
carriers).  The compiler or binder derives `round_trip` from the planned
carrier compositions (`mutable_by:'target'` means 1) and passes the result as a
compatibility hint.  It may provision a write-back medium shape (a shm reply
ring, a zenoh queryable vs subscriber), but it does not define protocol
directionality.  A transport has only this integer and optional, indicative
carrier ids; it does not receive atom semantics and cannot infer them from a
carrier id or traffic.  `owner` = 1 anchors/creates the 1:1
medium and `owner` = 0 attaches to an existing one (retry until the owner is up).
Which side owns is the caller's rendezvous fact; what "own" *means* is
transport-private (create a segment; a no-op for a symmetric keyed medium).
`carrier_hints` (may be NULL) is an INDICATIVE id list for best-effort checking,
never a contract.

`attach(ch, local, role)` joins `local` to a channel in a **role**:
`FSS_ROLE_INITIATOR` installs the outbound channel on `local`
(`local->send` / `local->send_ctx`); thereafter the model delivers with
`local->send(local->send_ctx, carrier)`.  `FSS_ROLE_TARGET` installs `local` as
the channel's inbound receive sink (a live endpoint pointer, which no locator
could name).  A point-to-point link is the degenerate 1-initiator/1-target case;
a bus is the general multipoint case (the in-process transport collapses a 1:1
one-way channel to the direct identity call and steps off the path, then reinserts
itself as a fan-out shim when a second one-way target attaches). Fan-out is
permitted for one-way carriers. A round-trip/write-back carrier MUST NOT fan
out: its write-back returns to one originator.

`send_to(ch, carrier)` is a **one-shot**: deliver a single carrier to a channel
with no standing attach — for occasional traffic such as a lifecycle ping.
Return codes:

```c
enum {
    FSS_SEND_OK          = 0,
    FSS_SEND_UNBOUND     = 1,   /* no attached channel, or channel unresolved */
    FSS_SEND_NO_RECEIVER = 2,   /* resolved peer has null receive             */
    /* future: FSS_SEND_QUEUE_FULL, FSS_SEND_DISCONNECTED, ... */
};
```

POSIX-shaped: `0` is success, non-zero is a reason.  Carriers are
atomic messages — byte-count semantics from `send(2)` do not apply
(the relevant POSIX analog is `mq_send(3)`).

A transport is itself a loadable **plugin**.  The host links no
transport: it `dlopen`s a transport `.so`/`.dylib` chosen at deploy
time, admits it through its version/capability queries, then resolves the
factory symbols and uses only the vtable above — so swapping in-process for a
wire transport is a config change, not a recompile.

```c
fss_transport_t *fss_transport_create (const char *config); /* config: transport-defined, may be NULL */
void             fss_transport_destroy(fss_transport_t *transport);
const char      *fss_transport_name   (void);               /* short, stable, lowercase; no path/extension */
```

The plugin also exports `fss_transport_name()`,
which returns the transport's short, stable, lowercase name (e.g. `"shm"`,
`"zenoh"`), with no path or file extension.  This is the name a host uses to
select the plugin: a name→plugin loader maps `<name>` to the plugin file
`libfss_<name>.<ext>`, `dlopen`s it, and confirms it loaded the intended
transport by matching the plugin's exported `fss_transport_name()` against
the requested name.

### Plugin ABI admission

The binary-loader transport family requires five exports:
`fss_transport_abi_version`, `fss_transport_capabilities`,
`fss_transport_create`, `fss_transport_destroy`, and `fss_transport_name`.
The first returns the packed major/minor ABI version; the second returns a bit
set.  For the current channel ABI, that set MUST include
`FSS_TRANSPORT_CAP_CHANNEL`, `FSS_TRANSPORT_CAP_ATTACH`, and
`FSS_TRANSPORT_CAP_SEND_TO` (together `FSS_TRANSPORT_CAP_CHANNEL_ABI`).
`FSS_TRANSPORT_CAP_REMOTE_READY` is an optional capability. It says that the
plugin also exports a transport-specific remote-readiness facility; it does not
change the fixed vtable or the required channel-ABI capability set.

A host MUST use this order: `dlopen`; resolve and call the version and
capability queries; require its ABI major, a plugin minor no older than the
host requirement, and the complete required capability set; resolve the
factory/name symbols; verify `fss_transport_name()` against the selected name;
then call `fss_transport_create` and dereference the returned fixed vtable.
It MUST reject a plugin on any failed lookup or check and MUST NOT call a
factory or inspect a vtable before admission succeeds.  Later ABI minors add
queries or capability bits without changing the fixed vtable.

A host MAY use an optional facility only after it tests its advertised
capability bit and resolves that facility's documented symbols. It MUST handle
a missing optional bit without treating the channel-ABI plugin as inadmissible.
For example, the Zenoh reference plugin advertises
`FSS_TRANSPORT_CAP_REMOTE_READY` and, when that bit is set, provides
`fss_zenoh_set_edge()` and `fss_zenoh_wait_ready()`. These are Zenoh-specific
symbols, not ABI-vtable entries.

**Reference scope.** The factory symbol names, admission sequence, vtable, channel token, and
`round_trip` mirror are reference ABI conventions, not Tier-1 semantic
requirements.  The **config-string grammar** is not standardised: the `config`
passed to `fss_transport_create` (the
session-level rendezvous, e.g. `"listen=tcp/…"` / `"connect=tcp/…"` for zenoh,
empty for shm) is a transport-defined opaque string.  A caller must therefore
know a given transport's private dialect.
For Zenoh launch edges, start the listener/target first and attach its receive
endpoint before starting the connecting initiator. If the optional remote-ready
capability is used, configure the edge before target attach, then wait with a
finite timeout on the initiator. Zenoh liveliness discovery alone is not proof
of delivery: the reference facility follows it with a flow-specific data-path
challenge (`z_get`/reply for round-trip, or `z_put` plus target acknowledgement
for pub/sub). It retries only within that deadline; it does not use a settle
sleep.
Typed configuration, standard transport selectors, and a configuration grammar
are out of scope.  A transport-defined opaque configuration remains sufficient
for this reference ABI.

Artefacts are **not** constructed by the transport.  Construction is the job of
the **placement facility** (repository implementation:
`reference/runtime/placement/`; public include name:
`<fss/runtime/placement.h>`),
which is always LOCAL: `fss_place_construct(node, locator)` `dlopen`s +
`fss_create`s the artefact in the CALLING process, full stop.  A cross-process
deployment has a launcher call it in the far process; the two processes are then
joined by attaching endpoints to a shared channel (`attach`).  A transport never
reaches across a process boundary to construct.  Both transports and artefacts
are `dlopen` plugins against this header-only ABI; the host links neither.

## Artefact

For the reference binary-loader convention, an artefact loaded via `dlopen` +
`dlsym` exports three C symbols:

```c
fss_node_t     *fss_create      (fss_transport_t *transport, const char *name);
void            fss_destroy     (fss_node_t *handle);
fss_endpoint_t *fss_get_endpoint(fss_node_t *node, const char *endpoint_name);
```

`fss_create` instantiates the artefact and returns an opaque handle;
`fss_get_endpoint` resolves a named endpoint on the handle (names
are local to the artefact's namespace; the transport prepends any
hierarchical prefixes); `fss_destroy` tears it down.

This convention applies only to reference binary loaders.  An out-of-process
deployment constructs the artefact locally and joins endpoints through a
transport channel; no entry-point ABI is involved.

## Multi-artefact libraries (forward compatibility)

The reference convention is one artefact type per `.dylib` (singleton
symbols).  A future spec revision MAY define an optional
`fss_artefact_manifest()` entry point that exposes multiple types
from one library; loaders SHOULD prefer it when present and fall
back to `fss_create` otherwise.

## Standard ports and lifecycle

The FSS path segment `fss/` is reserved: model authors MUST NOT use an
`fss` segment in their endpoint names.  The binding provides standard
control ports under this prefix on behalf of every artefact node.

Two endpoint kinds:

- **Data endpoints** — model-provided, model-named (e.g. `"socket"`,
  `"can_rx"`); carry domain carriers.
- **Standard control ports** — binding-provided, FSS-named.  Advertised
  by presence: `get_endpoint` returns `NULL` for an unsupported port.

**Each standard port is associated with exactly one base protocol** — the port name
*is* the protocol selector, and the port accepts only that protocol's carrier.
This is the normative rule for the reserved namespace: `fss/<name>` ⇒ the
`<name>` base protocol.  Control of an artefact (lifecycle, configuration, time)
is therefore ordinary carrier delivery to a named endpoint — never a side
channel, a constructor argument, or an out-of-band hook.

Defined standard ports:

| Port path       | Base protocol / carrier | MUST / MAY |
|-----------------|-------------------------|------------|
| `<node>/fss/sim`    | sim — `sim::event`           | MUST       |
| `<node>/fss/config` | config — `config::param_request` | MAY    |
| `<node>/fss/time`   | time/sync — `time::window`   | MAY        |

`fss/time` carries the time-synchronisation protocol: a node sends its peer a
`time::window` `(Lower, Upper)` at a `Unit` resolution ("run at least to
`Lower`, do not pass `Upper`") and the peer answers with its own window.  The
exchange is symmetric and one-way (fire-and-forget — no request/grant pair); a
node that does no time-sync omits the port.  See
`protocols/time-sync/`.  A federation-wide coordinator barrier (HLA-style
TAR/TAG) remains design-pending.

Lifecycle is a **protocol, not an ABI verb**.  A peer delivers `sim::event`
carriers (`sim::EventCode`: Started, Stopped, …) to a node's `fss/sim` port,
either by channel-then-attach (attach a local endpoint to the channel, then
`ep->send(...)`) or — for an occasional one-shot, the common case for
lifecycle — by `send_to(ch, carrier)` after obtaining the channel, with no
standing attachment.  There is
no `fss_start` ABI entry; the binding's `fss/sim` handler drives the kernel
(`sc_start` / `sc_stop`).

Configuration is likewise a protocol: a `config::param_request` carrier
Set/Gets a model's named parameters.  A node's own `fss/config` port answers
requests pushed *to* it.  In the reference orchestration the flow is inverted to
a **pull**: pushing config to N nodes from one orchestrator endpoint would need
fan-out of a write-back protocol (forbidden — `config::Status` is
`mutable_by:'target'`; see [00-object-model.md](00-object-model.md) §Fan-in and
fan-out), so instead each node, in its **topology-binding window** (after construction,
before the `sim::event{Started}` that elaborates the model), issues a
`config::param_request{Get}` to the single orchestrator `fss/config` responder
and applies the reply pre-elaboration.  Many nodes querying one responder is
fan-in.  This is the standard, in-band replacement for any binding-specific
"configure" hook, a constructor argument, or an environment side-channel.  (The
reference SystemC binding maps `config::ParamName`/`ParamValue` onto the model's
CCI parameters by name.)

`channel` and `attach` are the two irreducible transport primitives: they
establish the channel a carrier travels over and therefore cannot
themselves be carriers.  Everything else is a carrier over an
established channel.

## Bootstrap injection — reference breadcrumb

An agent that is brought into being by a launcher (fork / `exec`) or loaded
into a process needs to know which transport to use to reach the orchestrator,
the name of its (pull) topology channel, and the name of its (push) lifecycle
channel.  The reference uses a **breadcrumb**: a flat JSON object carrying those
facts.  Its grammar is not a Tier-1 requirement:

```json
{"transport":"<name>","control":"<topology channel>","lifecycle":"<sim-control channel>"}
```

`transport` is a transport **name** (`fss_transport_name()`, e.g. `"shm"`), not a
plugin path: the agent resolves it to a plugin file (`libfss_<name>.<ext>` on its
transport-plugin search directory) and confirms the loaded plugin by matching its
`fss_transport_name()` — the *same* name-to-plugin resolution a data edge uses for
`control::Transport`, so the control bootstrap and data selection share one code
path and no path ever crosses the breadcrumb.  `control` names the **topology**
channel: the orchestrator owns it (creates it + installs the plan responder), and
the agent attaches as the INITIATOR and PULLS its plan.  `lifecycle` names the
separate **sim-control** channel the agent attaches as a target, on which the
orchestrator pushes stop.  The agent is otherwise medium-agnostic.

**Two standard delivery paths:**

- **CLI** (`--fss-parent '<json>'`): the launcher passes the breadcrumb as a
  command-line argument to a forked/exec'd agent.  The agent reads it from
  `argv` on startup.
- **In-process** (`fss_topology_transport_init`): when a model is loaded via
  `dlopen` into an embedding process, it calls this standard entry point after
  reading its native configuration:

```c
/* reference/runtime/agent-core/include/fss/runtime/agent_core.h */
int fss_topology_transport_init(fss_agent_t *A, const char *breadcrumb_json);
```

Both paths result in the agent holding an initialised control transport,
ready to phone home.  Choosing a different control transport (e.g. zenoh
instead of shm for a networked launch) is a change to those two JSON strings;
no recompile is required.

Per-data-edge transport selection is a carried **name**: a Server/Client step in
the pulled `control::plan` names the edge's transport via its `control::Transport`
field (see §Topology control below), which the agent resolves to a plugin and
loads lazily.  What REMAINS future work is finer-grained per-*interface*
selection, a transport's right to refuse an edge, and the config-string grammar
(the opaque per-transport `config` dialect).

## Topology control — reference `control::plan` carrier

Topology (which endpoints join which channels over which transport) is delivered
to an agent by **pull**, on its own channel — kept SEPARATE from the
simulation-control (lifecycle) plane, which is a separate push channel (below).

The orchestrator **provisions the fabric up front** — it creates every channel
before any node attaches — then owns a per-agent **control channel** and installs
a stateful responder on it.  A launched agent attaches that channel as the
INITIATOR and **calls back**: it sends an (empty) `control::plan` request
(`protocols/control/plan.jsonnet`, `FSS_CONTROL_PLAN_ID`) and the responder
fills the `PlanJson` slot — a JSON array of the agent's WHOLE build+wire plan
(Construct + Server/Client/Bind attach steps) — in the round-trip reply.  Because
the channels already exist, the plan carries no ordering constraint: the agent
runs every step (construct its models, attach its endpoints to the named
channels) in one go, in any order.  The call-back doubles as the **readiness
signal** — the orchestrator cannot know when an agent is up, so the agent tells it
by calling — and a single per-language artefact can be told to build one, two or
three models in one pull.

`control::plan` is round-trip (`control::Status` `mutable_by:'target'`), so an
independent agent consumes it by Path A (known id) or Path B (meta-resolve).  Its
build/attach steps use the standard `control::OpCode` encoding (Construct, Server,
Client, Bind); a Server/Client step names its data-edge transport in the
`control::Transport` field (resolved to a plugin by name, loaded lazily).  What
stays the orchestrator's own non-normative affair is the plan's content.

**Simulation control is a separate plane.** Lifecycle — "configured", "ready",
"finished"/stop — is NOT carried on the topology channel.  It flows on separate
sim-control endpoints (`fss/sim`, `sim::event`; and, for the orchestrator↔agent
stop, a dedicated one-way lifecycle channel the orchestrator pushes on at
completion), whose wiring is set BY the pulled topology and may be n-to-n (the
orchestrator and a peer model may both listen for a node's "finished").  A node
is never push-driven over the topology channel; the topology channel only answers
pulls.

The operation set (Construct, Server, Client, Bind, Arm, Stop, Done) is the
standard `control::OpCode` encoding, so any conforming agent speaks it.  A
`control::instr` also carries a `control::Transport` field: an OP_SERVER /
OP_CLIENT instruction names the data-edge transport by that carried name (the
value returned by the transport plugin's `fss_transport_name()`, e.g. `"shm"`,
`"zenoh"`), rather than by any coarse shm-vs-zenoh flag.  The agent resolves
that name to a plugin file `libfss_<name>.<ext>` on its transport-plugin search
directory and `dlopen`s it lazily on first use, confirming the loaded plugin by
matching its exported `fss_transport_name()` against the requested name.
Adding a transport is therefore "drop a plugin on the search path and name it",
with no agent recompile.  What stays the orchestrator runtime's own affair, and
is **non-normative**, is only the *plan*: which instructions it issues, and in
what order, to realise a particular topology.

When the control channel rides the SHM transport (the reference bootstrap medium
for a local fork), `control::instr` crosses SHM slots like any other carrier, so
it must fit the transport's slot cap (`SLOT_DATA_CAP`).  That cap is a property of
the SHM transport, not of the control carriers: the reference sizes it to 4096
bytes so the widest control carrier — the `control::plan` build plan (a ~3 KB
`PlanJson` slot) — fits, with the 288-byte `control::instr` well within it.
Growing a control carrier therefore touches the SHM slot only because control
*uses* SHM; it does not couple to any data protocol.  A wider control payload is handled by choosing a control
transport whose slot fits it (widen the shm segment config, or name zenoh in the
breadcrumb), without a recompile.

## Consuming a carrier's fields

A receiver MUST identify a carrier before consuming it.  It does so by
one of two strategies:

- **Known id (Path A).**  The receiver recognises `carrier->id` as a
  protocol it was built against, so it already knows every field's
  offset.  It treats `data_ptr` as that layout and accesses fields
  directly — a single integer compare, then fixed-offset reads/writes,
  no meta parsing.  Example: the SystemC binding casts `data_ptr`
  directly to a live `tlm_generic_payload` when `carrier->id ==
  FSS_TLM_ID`.

- **Resolve by meta (Path B).**  A receiver that does not recognise
  `carrier->id` MUST NOT dispatch the carrier at guessed offsets.  It
  MUST instead resolve the needed fields by qname from the carrier meta
  (`qname → byte_offset`), and SHOULD cache the result keyed by
  `(id, qname)` so subsequent access is a lookup, not a re-parse.  This
  resolver is protocol-independent: one piece of library code per
  language, not per protocol.

The governing rule is **identify-or-reject**: a receiver that can neither
recognise the `id` (Path A) nor resolve the required fields from a meta
(Path B) MUST reject the carrier — never reinterpret unknown bytes at
guessed offsets.  Path A is a permitted optimisation (MAY); Path B is the
mandatory interoperability floor — every conforming consumer MUST support it.

The meta MAY be absent (`meta_ptr == NULL`, `meta_size == 0`) when the
transport guarantees both ends already share the layout for that `id` —
for example a transport that sends the meta once, caches it on both sides
keyed by `id`, then ships only `id` + data thereafter (the in-process and
SHM transports rely on this; a wire transport such as zenoh can negotiate
it).  Such a carrier is consumed by Path A.  This caching is a transport
concern; the standard neither mandates nor describes the negotiation — it
only requires that a carrier reaching a receiver be either id-recognisable
or meta-resolvable, else rejected.

## C-side mirrors

The reference headers are:

- `abi/include/fss/carrier.h` — carrier struct.
- `abi/include/fss/endpoint.h` — endpoint type.
- `abi/include/fss/transport_abi.h` — transport vtable.
- `abi/include/fss/artefact_abi.h` — artefact entry points.
