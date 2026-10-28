# FSS shared-memory transport — design

**Implementation status (2026-10-05): implemented reference transport.** The
current ABI is `channel`/`attach`/`send_to`; descriptions below that refer to
`open`, `bind`, or `set_sink` are historical design text. Current source
capacities are 256 slots per ring, 4096 data bytes per slot, and eight
2048-byte metadata entries (`reference/transports/shm/shm.c`). These are
reference implementation capacities, not FSS ABI limits.

The current plugin uses the binary-loader admission handshake: a host checks
the exported ABI version and required channel/attach/send-to capability bits
before factory use, then verifies the exported transport name.  This is a
reference ABI detail; the active paper and normative specification remain the
authority for protocol semantics.

*Supersedes the earlier Zenoh/iceoryx2 investigation (kept below as
§ Prior investigation).*

---

## 1. Why cross-process changes things

The in-proc transport (`reference/transports/inproc/inproc.cpp`) installs
`peer->receive` directly as `local->send`.  Delivery is a function call;
the carrier's `data_ptr`/`meta_ptr` are live process-virtual-address pointers.

A cross-process transport cannot do this.  Pointers are not portable across a
process boundary — each process has its own address space, so `data_ptr` in
process A is a dangling value in process B.

**This is NOT a wire/serialisation format.**  SHM is shared memory plus a
doorbell: the carrier's *flat data region* (its atom bytes) is **shallow-
copied** into a shared slot and the peer is signalled.  The C++ object that
may have produced those bytes — e.g. a `tlm_generic_payload` with its vtable,
`m_mm`, `m_extensions` — **never crosses**.  Only atom bytes move; the
receiver reconstructs whatever local object it needs from them (exactly the
Path-3/4 resolve-by-meta the in-proc receiver already does).  So a vtable on
the source object is irrelevant: it is never copied and never dispatched
across the boundary.  (And ownership is one-directional — the initiator owns
the payload; a target never frees it — so the destructor, the GP's only
virtual, never fires on a crossed payload either.)

**Carrying the meta (conformance).**  A carrier is `id + meta + data`, and the
receiver's resolve-by-meta path needs the meta — so SHM must carry it, not just
the data, to be conformant. Metadata is published **once** for each distinct
flattened metadata byte sequence in a small shared table. The id is checked but
is not the cache key. Each frame carries `{id, meta-table index, data}`; the
reader resolves the id to the published meta and delivers a conformant
`fss_carrier_t` (meta_ptr into the mapped table).  This is what lets a
meta-driven receiver — the SystemC binding mapping `can::frame` onto
`b_transport` — work across SHM, exactly as over in-proc and zenoh.

**Compile-time guard.**  A type a model hands to the SHM path as a carrier
backing MUST be memcpy-safe.  The SHM-facing API `static_assert`s
`std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>` — this fails
to compile for a `tlm_generic_payload` (its virtual dtor makes it non-trivially-
copyable), catching "someone stuffed a complex type down the pipe" at build
time.  The trait is the floor (no vtable, memcpy-safe); the stronger condition
for *true zero-copy* is pointer-free / fully-inline (no `fss:local-ptr` field
in the meta).

**Key insight — INLINE vs POINTER carriers:**

| carrier kind | data region | v0.1? |
|---|---|---|
| INLINE (e.g. `can::frame`, 16 bytes, no local-ptr) | self-contained atom bytes | YES — shallow-copy the bytes into the slot |
| POINTER (e.g. TLM GP: data_ptr / byte_enable separate heap allocs) | atom bytes + out-of-region pointers | YES — `shm_write_slot` flattens + `fss_carrier_serialize`s into the slot, `shm_reader` deserialises, and the reply ring applies the write-back |

`can::frame` is the motivating first payload: its entire data region is 16
self-contained bytes (fields at fixed offsets, no indirection).  Crossing it
is a flat `memcpy` of `{id, data[16]}` into a shared slot — no pointer chasing,
no object.  Pointer-carriers (TLM GP) now cross too: `shm_write_slot` flattens
the carrier (drops the non_serializable `core::TypeId`, re-hashes to the flat
id) and `fss_carrier_serialize`s the pointed-to bytes into the slot; `shm_reader`
deserialises into a local region+scratch before delivery, and the round-trip
reply re-serialises the mutated carrier so `fss_carrier_apply_reply` lands the
read-data back in the caller's buffers. The focused SystemC serializer test is
`tlm_live_gp_serialize`; it verifies the live-GP flatten/deserialize path. (A
zero-copy `MAP_FIXED` same-address arena was prototyped and **rejected** — the
copy is negligible against the doorbell; see the Decision note below.)

> **Decision (2026-06-04): copy, do not pursue zero-copy.**  A prototype
> zero-copy arena path (shared-arena loan + offset addressing + same-address
> `MAP_FIXED`, with a synchronous reply doorbell, even crossing a native
> `tlm_generic_payload` between two SystemC processes) was built and measured,
> then reverted.  The reason: the payload copy is negligible against the
> doorbell.  On Apple Silicon, one-way semaphore wakeup ≈ 2,450 ns; a 16-byte
> `memcpy` ≈ 4 ns, and even a 64 KB `memcpy` ≈ 677 ns (< a third of one
> doorbell; a synchronous round-trip pays two).  Swapping the doorbell for
> `os_sync_wait_on_address` (the macOS futex) measured identical to the
> semaphore — the cost is the cross-core wakeup + reschedule, not the kernel
> object, giving a ~2.4 µs portable floor.  Only busy-polling beats it (~40 ns
> one-way) at the price of a pinned core; that is a *separate* busy-poll
> transport, not a mode of this one.  So this transport copies the flat payload
> into the slot — simple, robust (no fragile `MAP_FIXED` between unrelated
> processes), and free in practice.  A carrier whose data exceeds the slot is
> rejected today; if a large-payload case appears, copy the pointed-to bytes
> into a per-slot region (still a copy, ~hundreds of ns) — NOT the arena/
> same-address machinery.

---

## 2. OS primitives — portable choice

| primitive | macOS | Linux | note |
|---|---|---|---|
| `shm_open` + `ftruncate` + `mmap` | yes | yes | POSIX; shared memory segment |
| POSIX named semaphore (`sem_open` / `sem_post` / `sem_wait`) | yes | yes | doorbell |
| `sem_init(pshared=1)` | **NO** (`ENOSYS`) | yes | — macOS does not implement process-shared unnamed semaphores |
| `futex` / `FUTEX_WAIT` | **NO** | yes | Linux-only; lower latency but not portable |
| `eventfd` | **NO** | yes | Linux-only |

**Chosen doorbell: POSIX named semaphore.**

`sem_init` with `pshared=1` is the natural choice for in-SHM synchronization
but macOS returns `ENOSYS`.  `futex` / `eventfd` are Linux-only.  Named
semaphores (`sem_open("/name", ...)`) work on both macOS and Linux, cost one
kernel name-lookup at open time, and are a one-line wrap.  For a future
Linux-only fast path, the doorbell shim can be swapped for a futex word stored
in the SHM header without changing the rest of the transport.

---

## 3. Segment layout

The current on-segment ABI is **v4**. A v4 attacher checks the magic,
version, ring size, mode, and complete layout size before it uses a segment.
The `mode` selects either one request ring or request and reply rings; it is
therefore part of layout compatibility. All v4 slots include `meta_index`.
This index identifies the exact published metadata entry for the frame; it is
not derived from the carrier id. `UINT32_MAX` means that the carrier supplied
no metadata.

```
 ┌─────────────────────────────────────────────────────────┐
 │  shm_header_t  (64 bytes, cache-line aligned)            │
 │    magic      u32   0x46535348  ("FSSH")                 │
│    version    u32   4                                     │
 │    ring_size  u32   power-of-two slot count              │
│    meta_count u32   published metadata entries             │
 │    head/tail            u64 ×2  (request ring cursors)    │
 │    reply_head/reply_tail u64 ×2 (reply ring, round-trip)  │
│    mode/layout_size + padding (fixed 64-byte header)       │
 ├─────────────────────────────────────────────────────────┤
 │  slot_t [ring_size]            (request ring)            │
 │    carrier_id  u64                                        │
 │    data_size   u32                                        │
│    meta_index  u32   exact published metadata entry, or   │
│                      UINT32_MAX when metadata is absent   │
│    status      i32   (reply ring: receiver's return code) │
│    data        u8[4096]                                   │
 ├─────────────────────────────────────────────────────────┤
 │  slot_t [ring_size]            (reply ring; round-trip)  │
 ├─────────────────────────────────────────────────────────┤
 │  meta_entry_t [FSS_SHM_META_TYPES]   (published meta)    │
 │    id     u64   carrier id (0 = empty)                    │
 │    size   u32   meta byte count                           │
 │    bytes  u8[FSS_SHM_META_BYTES]   canonical CBOR meta    │
 └─────────────────────────────────────────────────────────┘
```

### Layout version history

- **v2** added the optional reply ring for synchronous round-trips.
- **v3** added the published-metadata table, so carrier metadata crosses with
  the data (see §1). A receiver resolved metadata by carrier id.
- **v4 (current)** adds `slot_t.meta_index`. The sender publishes each distinct
  flattened metadata byte sequence once and each frame identifies its exact
  table entry. The receiver verifies that entry's carrier id before it delivers
  the carrier. This permits distinct metadata entries with the same carrier id;
  carrier id is checked, not used as the metadata cache key.

`SLOT_DATA_CAP` is 4096 bytes in the current reference implementation. It must
also fit the 3072-byte `control::PlanJson` field plus status. The ring size is
256 slots, and the fixed metadata table has eight 2048-byte entries. Oversize
serialized payloads or additional metadata types are rejected; dynamic sizing
and chunking remain deferred.

One named semaphore per direction signals "at least one slot is available to
read" (counting semaphore).  The single-producer/single-consumer (SPSC)
constraint lets `head`/`tail` be plain relaxed-atomic u64 values; no lock is
needed for the ring itself — only the semaphore for the blocking wakeup.

For multi-peer (future): one segment + semaphore pair per directed channel, or
an MPMC ring with a CAS on `head`.

---

## 4. Historical v0.1 vtable mapping

The following `open`/`bind` discussion documents the superseded v0.1 ABI. The
current mapping is: `channel(name, round_trip, owner, hints, n)` provisions or
finds the segment; `attach(..., INITIATOR)` installs the sender; and
`attach(..., TARGET)` installs the sink and starts the reader. `send_to` uses
the same frame path for one-shot delivery.  In the current ABI,
`fss_transport_create(config)` accepts the transport-private session config
(the reference SHM plugin currently ignores it); channel name, owner, and
round-trip compatibility hint belong only to `channel()`.  The
`create:`, `attach:`, `create-rr:`, and `attach-rr:` strings below are a
superseded v0.1 configuration dialect, not accepted current configuration.

### Names

```
SHM segment:    /fss_shm_<channel>
Semaphores:     /fss_sem_<channel>_A2B   (A → B direction)
                /fss_sem_<channel>_B2A   (B → A direction)
```

`<channel>` is a short identifier agreed between the two processes (e.g.
`can_bus0`).  The FSS locator for a peer endpoint is
`<channel>/<node>/<endpoint>` — the transport strips the leading
`<channel>/` and the remainder is the endpoint name within that peer.

### Historical `fss_transport_create(config)`

`config` is the channel name string (e.g. `"can_bus0"`).  The transport
allocates its private state and stores the channel name.  No SHM is touched
yet — deferred to `open`.

> **Historical note:** this dialect selected role, mode, and channel in
> `config` (`"create[-rr]:<channel>"` / `"attach[-rr]:<channel>"`).  It was
> removed with the v0.1 ABI.  It does not describe the current plugin.

### `open(name)`

`open` is the universal "instantiate + connect a node here" verb (transport_abi.h):
`name` is `"<node>=<path>[@<inbound-endpoint>]"`.

- The transport `dlopen`s `<path>`, resolves the artefact ABI, and
  `fss_create`s it as `<node>` **in this (the receiver's) process** — the
  cross-process analogue of in-proc's `open` (SHM still moves only flat bytes
  across the boundary; the model runs locally).
- If `@<inbound-endpoint>` is given, that endpoint is wired as the receive sink
  — which marks this instance the receive side and **spawns the reader thread**.
- Returns the real `fss_node_t`, so `get_endpoint` can resolve the node's other
  endpoints (e.g. a bridge's outbound socket to wire downstream).

A `name` with no `=` is the legacy form: register a delivery target, return a
non-null sentinel, and let the host wire the receive side via the transport
vtable's `set_sink` slot (the inbound dual of `bind`; back-compat for a bare C
sink or a Python receiver that can't be `dlopen`'d).

### `get_endpoint(node, endpoint_name)`

Resolves a hosted artefact's named endpoint (delegates to the artefact's
`fss_get_endpoint`); returns `NULL` when no artefact is hosted.  For v0.1 the
channel is point-to-point, so the ring itself needs no routing — the endpoint
name selects *which* of a hosted node's endpoints to bind.

### `bind(local, "<channel>/<node>/<endpoint>")`

The transport verifies the channel matches, stores the destination endpoint
string in `local->send_ctx`, and sets `local->send` to `shm_send_fn` — a
static function that writes a slot and posts the semaphore.  After `bind`,
model code calls `local->send(local->send_ctx, carrier)` and the transport is
no longer on the hot path for the pointer itself (the function pointer is
direct).

### `send_to(peer_locator, carrier)` and `shm_send_fn`

Both serialize the carrier into the next ring slot:

```
1.  sem_wait(sem_empty)              // block if ring is full
2.  idx = head % ring_size
3.  slot[idx].carrier_id = carrier->id
4.  slot[idx].data_size  = (u32)carrier->data_size
5.  assert(carrier->data_size <= SLOT_DATA_CAP)
6.  memcpy(slot[idx].data, carrier->data_ptr, carrier->data_size)
7.  head++ (atomic store, release)
8.  sem_post(sem_full)               // wake receiver
```

For an INLINE carrier like `can::frame` the `data_ptr` points into the
carrier struct on the caller's stack; the `memcpy` is 16 bytes.

### Reader thread (per node)

Each node runs a dedicated reader thread:

```
loop:
    sem_wait(sem_full)               // block until slot available
    idx = tail % ring_size
    reconstruct local carrier:
        c.id        = slot[idx].carrier_id
        c.data_ptr  = slot[idx].data       // points into SHM — valid for duration of receive call
        c.data_size = slot[idx].data_size
        c.meta_ptr  = resolve_meta(c.id)   // published-meta table (§1); NULL if sender carried none
        c.meta_size = (size from the table)
    tail++ (atomic store, release)
    sem_post(sem_empty)              // free the slot
    bound_endpoint->receive(bound_endpoint->self, &c)
```

The `data_ptr` in the reconstructed carrier points directly into the SHM slot.
The receive callback must consume the data before returning (or copy it out);
it must not retain the pointer.  This matches the FSS carrier lifetime
contract (a carrier is valid only for the duration of the receive call).

### `close` / `fss_transport_destroy`

Signal the reader thread to exit (set a stop flag, post the semaphore once),
join it.  Call `munmap`.  If this process was the creator: `shm_unlink`,
`sem_unlink` both semaphores.

---

## 5. Lifecycle and cleanup

**Stale segment handling:** on the owner `channel()` path, `shm_unlink` + re-create
if `O_CREAT|O_EXCL` fails with `EEXIST` and a magic-check on the existing
segment shows a bad header (version mismatch or wrong size).  A clean shutdown
always unlinks.  A crash leaves the names in the kernel namespace; the next
creator detects and replaces them.

**Refcounting:** for v0.1 (two-process SPSC) the creator owns the names and
unlinks on destroy.  For multi-peer (future): a refcount field in
`shm_header_t`; the last closer unlinks.

---

## 6. Historical first-cut scope

### In scope (v0.1; historical)

- INLINE carriers only (`can::frame` and any other carrier with
  `data_size <= SLOT_DATA_CAP` and `data_ptr` pointing into its own struct).
- SPSC: one writer process, one reader process, one channel.
- Named POSIX semaphore as doorbell (portable macOS + Linux).
- Fixed-size slots (4096-byte data capacity), 256 slots per ring, and eight
  2048-byte metadata entries.
- macOS + Linux.
- **Synchronous request/reply** (then selected by the
  `create-rr:`/`attach-rr:` configuration dialect).
  A second (reply) ring + reply doorbell turns a bound `send()` into the
  cross-process realisation of the return-path contract (`transport_abi.h`):
  the sender writes the request, blocks, and on wake copies the receiver's
  (possibly mutated) bytes + status back over the carrier.  This closes the
  one-way gap for **self-contained** carriers — a request whose `Status` /
  read-data field is written back now federates over SHM.  The one-way mode
  (`create:`/`attach:`) remains for immutable / fan-out carriers (`can::frame`).

- **TLM `b_transport` over SHM** — implemented after the original scope.
  `shm_write_slot` links `fss-lang-c-meta`, flattens the carrier, and
  `fss_carrier_serialize`s the pointed-to bytes into the slot when
  `fss_carrier_has_pointers(meta)`; `shm_reader` deserialises; the reply ring
  re-serialises so `fss_carrier_apply_reply` writes read-data back to the
  caller. The current focused serializer test is `tlm_live_gp_serialize`.

### Out of scope (as recorded at v0.1)

- Large pointer-carriers whose serialized form exceeds the current 4096-byte
  slot capacity: grow the per-slot region or chunk it. Still a
  copy — NOT the zero-copy arena / `MAP_FIXED` approach, which was prototyped and
  rejected (see the Decision note in §1: the copy is negligible against the
  doorbell).
- MPMC / multi-peer: multiple writers or readers on one segment.  The
  request/reply mode is one outstanding request at a time (SPSC synchronous).
- Windows: `CreateFileMapping` / `CreateSemaphore` shim.
- Slot routing / endpoint demux: in v0.1 the channel is point-to-point; a
  future slot header field carries a destination endpoint hash.
- Per-carrier auto mode-select: v0.1 selected round-trip versus one-way in its
  config dialect.  The current ABI instead receives the binder's channel
  compatibility hint; it cannot recover `mutable_by` semantics from metadata,
  ids, or traffic.
- A busy-poll transport variant for latency-critical channels (~40 ns one-way
  vs ~2.4 µs for the blocking doorbell, at the cost of a pinned core).  This is
  a *separate* transport, not a mode of this one — see the Decision note (§1).

---

## 7. Platform spike

`docs/design/exploration/shm-spike/shm_spike.c` is a self-contained C
file that proves all three macOS primitives work together:

```
cc -o /tmp/shm_spike docs/design/exploration/shm-spike/shm_spike.c \
    && /tmp/shm_spike
```

Output on macOS 15 (Darwin 25):

```
[OK] shm-spike: 5 messages crossed the process boundary
```

macOS-specific findings confirmed by the spike:

1. **`sem_init(pshared=1)` is NOT implemented** — returns `ENOSYS`.  Use
   `sem_open` (named semaphores).  This is the primary portability trap.
2. **No `-lrt` or `-lpthread` needed** on macOS — `shm_open`, `mmap`,
   `sem_open`, `fork` are all in the default `libSystem` (linked
   automatically by `cc`).
3. **`shm_unlink` before `shm_open(O_CREAT|O_EXCL)`** is the right pattern
   to handle stale names from a previous crash; the unlink of a non-existent
   name returns `ENOENT` harmlessly.
4. **SHM segment lives in `/var/folders/.../`** on macOS (not `/dev/shm` as on
   Linux); this is transparent to the API — `shm_open` names are just strings.

---

## Prior investigation (kept for reference)

*(The original Zenoh vs. iceoryx2 investigation is preserved below.)*

---

### Original: Efficient on-machine transport — does Zenoh cover it, or do we build?

Question: for a high-efficiency *same-machine* FSS transport (shared-memory
payloads + an inter-process wakeup), does Zenoh give us a cross-platform
answer, or must we build one?

Investigated against the actual sources: `eclipse-zenoh/zenoh-c` and the
`zenoh-shm` crate in `eclipse-zenoh/zenoh` (2026-05).

#### What Zenoh actually provides

**SHM payloads — cross-platform, but opt-in and unstable.**
- The SHM *segment* layer is genuinely cross-platform: `zenoh-shm/src/shm/unix.rs`
  (`shm_open`/`mmap`) **and** `shm/windows.rs` (`CreateFileMapping` /
  `MapViewOfFile` / `OpenFileMapping`). So zero-copy SHM buffers map on
  Linux, macOS, and Windows.
- It is gated behind **two non-default build flags** —
  `ZENOHC_BUILD_WITH_UNSTABLE_API` + `ZENOHC_BUILD_WITH_SHARED_MEMORY` — i.e.
  explicitly **unstable**. The C API is the `z_*shm*` / `z_posix_shm_provider_new`
  family.
- It carries a **watchdog** subsystem: background periodic threads validating
  buffer liveliness so a crashed peer can't corrupt/leak shared buffers.
  Robust, but real background-thread overhead; the source itself warns of
  timeouts "in case of a heavy congested system".

**The notification/wakeup path — NOT cross-platform-efficient.**
This is the gap. SHM only carries the *payload*; the *signal* ("data is
ready") still rides a Zenoh transport **link**. The local link options are:

| link | platforms | note |
|---|---|---|
| `unixpipe` | POSIX only | fast local |
| `unixsock_stream` | POSIX only | fast local |
| `vsock` | Linux only | VM sockets |
| `tcp` | all | loopback — syscall + TCP stack per message |
| quic / tls / ws / udp / serial | — | not for fast local |

There is **no Windows named-pipe or event-object link**. On Windows the
signalling path falls back to **TCP loopback**. So Zenoh's *fast* local
notification is POSIX-only; its cross-platform notification is the slow path.

#### Verdict

Zenoh solves the **payload** half cross-platform (SHM buffers map everywhere)
but **not** the **notification** half cross-platform (fast local links are
POSIX-only; Windows = TCP loopback). It is also a heavy, unstable-flagged
dependency for what FSS wants as a lean reference transport.

So: **Zenoh does not give us a turnkey cross-platform "SHM + IPC-event"
local transport.** Two viable paths:

**Option A — build a lean FSS SHM transport (recommended for the reference)**

A purpose-built same-machine transport, dependency-light, genuinely
cross-platform:
- **Payload + queue:** a fixed SHM segment (POSIX `shm_open`+`mmap`;
  Windows `CreateFileMapping`+`MapViewOfFile` — ~the same ~200 LOC the
  zenoh segment layer is) holding an SPSC/MPSC ring of carrier slots.
- **Doorbell (the inter-process event):** the cross-platform-portable
  primitives are a **named semaphore** (`sem_open` / `CreateSemaphore`) or a
  futex-style wait (`futex` on Linux, `WaitOnAddress` on Windows, a
  process-shared `pthread_cond` on macOS). Wrap one behind a 3-call
  `doorbell_{create,signal,wait}` shim.
- Maps cleanly onto the existing transport vtable (`open`/`close`/`get_endpoint`/
  `bind`/`send_to`) — the carrier's `data_ptr`/`meta_ptr` become offsets into
  the SHM segment (which the wire form already anticipates).
- Estimated ~600–1000 LOC + per-platform doorbell. Well-understood; no
  external runtime.

**Option B — adopt iceoryx2 (purpose-built, cross-platform)**

`eclipse-iceoryx/iceoryx2` is a Rust zero-copy IPC middleware **designed**
for exactly this: cross-platform (Linux/macOS/Windows/FreeBSD) SHM + an
event/notifier mechanism, with a C/C++ API. If we want to *not* maintain the
SHM+doorbell ourselves, it is the strongest off-the-shelf fit — closer to the
need than Zenoh's SHM. Cost: another sizeable dependency and a Rust runtime.

#### Recommendation

Keep **Zenoh as the network/cross-host transport** (where it excels), and for
the same-machine fast path build **Option A** — a small FSS SHM transport
with a cross-platform doorbell. It is dependency-light, matches FSS's lean
reference ethos, and is the only path that gives uniform efficiency on
Windows as well as POSIX. Evaluate **iceoryx2** before writing the doorbell
if we'd rather adopt than build.
