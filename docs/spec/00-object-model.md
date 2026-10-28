# 00 — Object model

FSS is built from six object kinds.  This section names each kind,
states what it is, and describes how they compose.  The other spec
sections detail the data structures those objects exchange.

## Object kinds

### Carrier

A **carrier** is the unit of FSS communication: a 40-byte header
(`fss_carrier_t`) referencing a flat data region (`data_ptr` /
`data_size`) and a self-describing layout descriptor (`meta_ptr` /
`meta_size`).  Its `id` is the hash of its meta — both a type tag
and an equality key.  See [01-carrier.md](01-carrier.md).

A carrier has two forms:

- **Live form** — `data_ptr` references a live in-memory C++ object;
  the carrier meta may carry `core::TypeId` (a process-local,
  non-serialisable marker at offset 0 of the object).  Valid in-process
  only.
- **Flat form** — `data_ptr` references a plain byte buffer;
  `core::TypeId` is absent because a wire transport excludes it before
  transmission.  The product of any wire transport.  See
  [02-encodings.md](02-encodings.md) §`core::TypeId`.

### Endpoint

An **endpoint** is a carrier sink (inbound `receive` callback) and,
once attached as an initiator, a carrier source (outbound `send` callback).
The artefact that owns the endpoint installs `receive`; the transport installs
`send` / `send_ctx` at attach time.  An endpoint belongs to exactly one
artefact and is resolved by name via the transport.

Endpoints carry no protocol-type information.  The topology binder knows what
protocols speak between two endpoints; mismatched attachments surface as
runtime failures when the receiver's id filter rejects an unexpected
carrier.  See [05-abi-conventions.md](05-abi-conventions.md) §Endpoint.

### Transport

A **transport** hosts channels and joins endpoints to them.  Its two
irreducible primitives are:

- **`channel`** — create or find a named channel on the transport (the
  unit of connection; a channel may be point-to-point or multipoint).
- **`attach`** — join a local endpoint to a channel in a **role**.  As an
  `INITIATOR` it installs the outbound callback (`local->send` /
  `local->send_ctx`) on the local endpoint (the `connect()` analog);
  thereafter `local->send(local->send_ctx, carrier)` delivers into the
  channel with no further addressing.  As a `TARGET` it installs the
   endpoint as the channel's receive sink (the `accept()` analog).

There is no `transport->send` — delivery on an attached channel is the
installed callback, not a transport call.  `send_to(ch, carrier)` is the
one-shot alternative: deliver to a channel without installing a standing
attach, for occasional traffic such as lifecycle pings.

A transport is itself a loadable plugin (`dlopen` / `dlsym` against
`fss_transport_create` / `fss_transport_destroy`) so that in-process,
SHM, and network transports are interchangeable by configuration.  See
[05-abi-conventions.md](05-abi-conventions.md) §Transport.

**In-process transport.** The identity wiring (`local->send =
peer->receive; local->send_ctx = peer->self`) leaves the transport
entirely off the per-send hot path.  Live-form carriers (carrying
`core::TypeId`) are valid here; a matched peer may take the zero-copy
fast path (cast `data_ptr` to the live object) on id equality.  See
[02-encodings.md](02-encodings.md) §`core::TypeId`.

**Wire transports (SHM, zenoh).** A wire transport that encounters a
`core::TypeId` atom (or any `fss:local-ptr` field) MUST drop it,
inline the pointed-to bytes, and re-hash the reduced meta to obtain
the flat carrier id before transmitting.  Only flat bytes cross the
boundary; the receiver never casts a serialised carrier as a live
object.  See `docs/design/shm-transport.md` and
`docs/design/zenoh-transport.md`.

Topology-binding semantics and the relationship between transport primitives and
simulation phasing are detailed in `docs/design/binding-and-phases.md`.

### Artefact

An **artefact** is a loadable, deployable simulation component — a
`.so` / `.dylib` / `.dll` — that exposes one or more named endpoints.
 It constructs its model and owns its endpoints; it exports three
well-known C symbols: `fss_create`, `fss_get_endpoint`,
`fss_destroy`.  The artefact owns its endpoints; they are resolved by
name (via `fss_get_endpoint`) in the topology-binding window — after construction
but before the `sim::event{Started}` carrier that closes the window.

An artefact wraps a **model** (see below) and optionally provides
standard control ports (`fss/sim`, `fss/config`, `fss/time`) on the
 model's behalf.  The artefact does not choose its transport.  See [05-abi-conventions.md](05-abi-conventions.md)
§Artefact and §Standard ports.

### Model

A **model** is the actual simulation component — a SystemC module, an
FMI FMU, a Python or Rust model — that an artefact wraps.  The model
sees only its own language API; FSS is the artefact's concern.  A
model is not an FSS object and has no FSS ABI.

### Binding

A **binding** (under `reference/languages/<lang>/`) is the per-language glue
that turns a model into an artefact.  It is responsible for:

- Constructing the model and registering its endpoints.
- Translating between the language's native carrier representation
  (e.g. a `tlm_generic_payload` in SystemC-TLM) and the FSS flat
  carrier.
- Providing the standard control ports (`fss/sim`, `fss/config`,
  `fss/time`) so the model does not need to implement lifecycle,
  configuration, or time-sync explicitly.

A binding is not a spec-normative object — its interface to the model
is language-defined.  It is normative only at the carrier boundary it
presents to the transport.

## Roles: node and orchestrator

The six kinds above are *things*; a running federation also has two
**roles** — how those things are deployed and driven.

- A **node** is a live instance of an artefact in the topology,
  addressable by the `<node-path>` portion of a locator (see Locators
  below).  An artefact is the deployable unit on disk; a node is its
  instantiation.  The reference ABI's opaque per-instance handle is
  `fss_node_t`.
- An **orchestrator** coordinates construction, transport selection,
  endpoint attachment, and lifecycle/configuration policy.  It may load
  artefacts as nodes, but it does not transfer ownership of their models or
  endpoints to a transport.  "Orchestrator" is a role, not an object kind.

## Fan-in and fan-out

A topology **binding** wires endpoint attachments to a named channel.  Two
multiplicities build on that primitive:

- **Fan-in** (many → one): several source endpoints, each over its own
  transport instance, deliver into one target endpoint's `receive`.  The
  single outbound `send` slot lives on each *source*, so it never
  constrains the shared target — fan-in needs no special support and
  works today across heterogeneous transports.  The config pull (every
  node queries the one orchestrator `fss/config` responder) and the
  readiness latch are fan-in.
- **Fan-out** (one → many): a transport multipoint channel MAY deliver one
  source to several targets when the protocol is one-way. This is a permitted
  topology, not merely an implementation optimisation.

**RULE (normative): fan-out MUST carry only a one-way protocol, never a
write-back protocol** — one
whose carrier has a `mutable_by:'target'` atom (e.g. `bus::Status`,
`config::Status`, `sim::Status`).  Each receiver would stamp its result
into a distinct copy and there would be no single carrier to return.
Write-back protocols are therefore round-trip (and MAY fan *in*), never
broadcast; only one-way protocols (a CAN frame, a pure sim broadcast) may
fan out. A binder MUST NOT create, and a transport that can determine the
protocol directionality MUST reject, a fan-out attachment for a write-back
protocol.

## Composition

```
     ┌──────────── artefact (.so) ─────────────┐
     │  binding glue                            │
     │    ┌─────── model ───────────┐           │
     │    │  (SystemC / FMI / …)   │           │
     │    └────────────────────────┘           │
     │  endpoint A       endpoint B             │
     └─────────┬─────────────────┬─────────────┘
               │                 │
          attach │           attach │
               │                 │
         transport                transport
               │                 │
               carrier ─────────▶
```

The artefact constructs its model and resolves its endpoints.  The
transport owns channels and attach/send state.  The orchestrator
coordinates these actions.  Carriers flow between attached endpoints along
installed callbacks; the transport is off the hot path for in-process
attachments.

## Locators

An endpoint is named by a **locator** — a slash-separated hierarchical
path (`<node-path>/<endpoint-name>`), compatible with zenoh key
expressions.  The locator never names the transport: endpoints do not
choose transports; cross-transport routing is configured topology.  The
`fss/` is a reference convention; its namespace policy is not a Tier-1
requirement.

## Cross-references

- Carrier data model — [01-carrier.md](01-carrier.md).
- Field types and `core::TypeId` — [02-encodings.md](02-encodings.md).
- Atom definitions and reference vocabulary — [03-atoms.md](03-atoms.md).
- Protocol compositions and `core::Extensions` — [04-protocols.md](04-protocols.md).
- Transport, endpoint, artefact ABIs (informative) — [05-abi-conventions.md](05-abi-conventions.md).
- Topology-binding semantics and simulation phasing — `docs/design/binding-and-phases.md`.
- SHM transport design — `docs/design/shm-transport.md`.
- Zenoh transport design — `docs/design/zenoh-transport.md`.
