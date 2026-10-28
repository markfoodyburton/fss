# Zenoh transport

**Status:** implemented reference transport. `reference/transports/zenoh/` is
built only with `FSS_FETCH_ZENOH=ON`. CPM then fetches
`eclipse-zenoh/zenoh-c` 1.9.0 and provides `zenohc::lib`.

Zenoh-c builds through Cargo. A Zenoh build therefore needs CMake, a C11
compiler, Cargo/Rust, and the usual platform linker tools. The fetched
`zenohc::lib` is commonly a static archive linked into the loadable
`fss_zenoh` module. Do not mix host architectures or incompatible C/C++/Rust
toolchains. In particular, a static archive used in a module must be built
with position-independent code where the platform requires it; it is not a
drop-in replacement for a shared system Zenoh library.

Where the SHM transport is the same-machine fast path (`shm-transport.md`),
zenoh is the **network / cross-host** path.  Both move only atom *bytes* across
the boundary; neither dlopens artefacts (that is an in-proc concern).

## Two carrier-flow shapes, two zenoh primitives

FSS carriers split cleanly by whether the receiver writes anything back:

| flow | zenoh primitive | use for | selector |
|---|---|---|---|
| **request/reply** | `z_get` → `z_queryable` → `z_query_reply` | carriers with target write-back — a `txn` whose `Status` / read-data is written back | `channel(..., round_trip=1, ...)` |
| **pub/sub** (one-way) | `z_put` → `z_subscriber` | carriers with no write-back; suitable for fan-out — `can::frame`, a `sim::event` broadcast | `channel(..., round_trip=0, ...)` |

This is the user's framing: *pub/sub for carriers with no mutable fields and/or
multi-connected endpoints; request/reply for the rest.* The binder selects the
flow per channel with `round_trip`; configuration does not select it.
Request/reply honours the **return-path contract** (`transport_abi.h`): a bound
`send()` is synchronous and two-way — it returns the peer receive's status, and
on return the carrier's data region reflects the peer's in-place mutation.
Pub/sub `send()` is fire-and-forget and returns `FSS_SEND_OK` with no write-back
— valid only for carriers with no written-back field.

## Mapping onto the current FSS vtable

- **`fss_transport_create(config)`** — `z_open` a session.  `config` is a
  `;`-separated option string:
  - `key=<prefix>` — this node's key-expression prefix (`<node>/<endpoint>`,
    zenoh-key-compatible per `binding-and-phases.md`).
  - `mode=<peer|client>` — zenoh *session* mode (default peer), not the
    carrier-flow selector.
  - `pubsub` — historical compatibility flag. If present, it is ignored when
    `channel()` selects the flow mode with `round_trip`; it is not a flow
    selector.
  - `listen=<endpoint>` / `connect=<endpoint>` — explicit zenoh endpoints
    (e.g. `tcp/127.0.0.1:7789`).  Giving one disables multicast scouting, so
    two same-host peers rendezvous deterministically over that endpoint rather
    than relying on multicast discovery (which is often blocked/slow on a dev
    box — see "Discovery" below).
- **`channel(name, round_trip, owner, hints, n)`** — finds the named,
  key-defined channel and is the Zenoh carrier-flow mode selector: nonzero
  `round_trip` selects request/reply and zero selects one-way pub/sub. Zenoh's
  keyed medium is symmetric, so ownership is transport-private.
- **`attach(ch, local, INITIATOR)`** — installs `z_get` for round-trip traffic
  or `z_put` for one-way traffic as `local->send`.
- **`attach(ch, local, TARGET)`** — declares a queryable or subscriber and
  installs `local` as its receive sink.
- **`send_to(ch, c)`** — one-shot delivery with the same framing.

Artefact construction and endpoint lookup are local placement work; they are not
Zenoh transport-vtable operations. `fss_zenoh_set_sink` is retained only as a
direct-host compatibility helper.

## Wire framing

The carrier byte format is the only cross-ABI contract (`carrier.h`); both peers
agree on this framing:

```
request/publish = [magic/version:"FSSD\\x01\\x00\\x00\\x00" (8 bytes)][id:u64 LE][meta_len:u32 LE][flat meta]
                   [payload_len:u32 LE][serialized payload]
reply           = [status:i32 LE][payload_len:u32 LE][serialized payload]
```

The request carries exact flattened metadata because an id alone does not
identify metadata. The payload is either the flat data region or the generic
serialized representation with `fss:local-ptr` data inlined. The queryable
handler reconstructs a local carrier, lets the sink mutate it, then replies with
status and serialized mutated payload. The `z_get` side copies write-back into
the caller's carrier and returns the status.

## Discovery

Two separate processes each `z_open` their own session and must find each other.
Default zenoh peer discovery is UDP multicast scouting, which on a typical dev
machine is slow or blocked — measured: two default-config loopback peers did not
connect within ~2 s.  So the transport accepts explicit `listen=`/`connect=`
endpoints and disables multicast when one is given; the receiver listens, the
sender connects, and they rendezvous immediately.  The federation tests use a
loopback TCP endpoint for exactly this. Start the listener/target first: it
opens the session, creates the channel, and attaches its receive endpoint
before the initiator starts. The initiator may then connect and attach. This
ordering makes the receiver and its Zenoh entity available before the first
application carrier.

### Optional remote-ready barrier

The Zenoh plugin advertises `FSS_TRANSPORT_CAP_REMOTE_READY` in addition to
the channel ABI capabilities. A host that requires this optional facility must
first test that bit, then resolve the transport-specific symbols
`fss_zenoh_set_edge()` and `fss_zenoh_wait_ready()`. The facility does not
extend `fss_transport_t`; transports without the bit remain valid channel-ABI
plugins.

For one launch edge, call `fss_zenoh_set_edge(transport, local_endpoint,
peer_endpoint, role)` before the target attaches, on both peers. After the
target has attached its receiver, it declares a target-specific Zenoh
liveliness token. The initiator calls `fss_zenoh_wait_ready()` with a finite
timeout. It subscribes with history to discover that exact token, then proves
the data path rather than treating discovery as delivery:

- round-trip sends a challenge with `z_get` to the target data key and requires
  the matching reply;
- pub/sub publishes a challenge to the target data key, and requires a matching
  acknowledgement query only after the target subscriber observed that probe.

The challenge loop uses bounded attempts within the supplied monotonic
deadline. It returns `FSS_SEND_UNBOUND` on timeout. There is no fixed
"discovery settle" sleep in this startup contract. The probes are transport
control traffic and are not delivered to the application sink.

## TLM over the wire (built)

A SystemC-TLM `b_transport` uses the same cross-boundary serialization path.
The focused current test label is `tlm_live_gp_serialize`; references to the
former `fss_tlm_over_wire` federation label below are historical. This needs
more than the round-trip: a TLM carrier is a **pointer-carrier** — its
`data_ptr` points to a separate payload of `bus::Length` bytes (and
`byte_enable_ptr` to another) — so the pointer is meaningless to the peer.

The fix is the **generic carrier serializer**
(`reference/languages/core-c/include/fss/c/wire.h`; include as
`fss/c/wire.h`), driven entirely by the carrier meta:

1. **Serialize** (transport send): walk the meta; for each `fss:local-ptr`
   field, read its length from the `attrs.length_at` companion field and inline
   the pointed-to bytes.  See `fss_carrier_serialize`.
2. **Deserialize** (transport receive): rebuild the flat region and repoint each
   pointer field at the inlined bytes laid into a scratch arena
   (`fss_carrier_deserialize`).  The SystemC receiver's existing foreign/flat
   path then reads `bus::Data` as a (now valid, local) pointer and runs
   `b_transport`.
3. **Write-back** (transport reply): the receiver re-serializes the mutated
   carrier; the sender overlays scalars (Status) and copies read data back into
   its own payload buffer in place (`fss_carrier_apply_reply`).

This is protocol-INDEPENDENT — one piece of code, every pointer-carrier — and it
lives in `core-c`
(`reference/languages/core-c/include/fss/c/{wire.h,meta.h}`; included as
`fss/c/wire.h` and `fss/c/meta.h`), so the SHM transport gets the same
capability for free.  Before transmitting, the transport calls
`fss_meta_flatten`: it **drops the non-serialisable `core::TypeId` atom** (and
inlines the `fss:local-ptr` payloads) and **re-hashes the reduced meta** to derive
the **flat carrier id** (`0xa16da8f9ada715fe`), distinct from `FSS_TLM_ID`
(`0xb22e5166992195b9`, the live id).  The wire therefore carries the flat id and
the flat meta — never the live id — so the receiver's `id == FSS_TLM_ID` fast-path
gate is false and it never casts deserialised wire bytes as a live object.  This
id split is the whole mechanism; see `docs/spec/02-encodings.md` §core::TypeId and
§"In-process pointer inlining".

SC-initiator-over-wire works at the mechanism level today: a live
`tlm_generic_payload` serialises across the boundary (proven by the
`tlm_live_gp_serialize` test).  The remaining integration piece is a full
two-process federation test driving a real SC initiator artefact (task: the
SC-initiator-over-wire federation); the host-driven target direction (model as
target) was previously exercised by the historical `fss_tlm_over_wire` label.

## Out of scope (v0.1)

- SC-model-as-initiator over the wire (the `initiator_stub` flat-form emit
  above); the target direction is built.
- Multi-peer / MPSC fan-in on a queryable; `z_querier`-cached sessions.
- The zenoh SHM feature (`Z_FEATURE_SHARED_MEMORY`, unstable) for same-host
  zero-copy — the lean SHM transport already covers the same-host fast path.
- Per-carrier auto mode-select from the meta `mutable_by`.
