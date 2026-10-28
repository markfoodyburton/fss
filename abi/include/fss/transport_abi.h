/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/transport_abi.h — vtable for the FSS transport object.
 *
 *   transport   one of the two main FSS objects (the other is the
 *               endpoint, fss/endpoint.h).  Communicates carriers
 *               (fss/carrier.h) between endpoints.  A program
 *               uses >= 1 transport.  Wrapped per implementation
 *               (shared-memory, zenoh, FMI binary, ...).
 *
 * Normative for the binary-loader (dlopen) transport family: artefacts
 * and transports built independently and loaded into one process MUST agree
 * on this ABI (struct layouts + symbol names) to interoperate.  The carrier
 * byte format (carrier.h) is normative universally — peers that share no C
 * ABI (e.g. across a wire transport) still agree on it.  A different
 * transport family may define its own in-process rendezvous, but
 * interoperates with this one only through the carrier.
 *
 * Transport ABI = channel / attach / send_to.
 *   - channel(name, round_trip, owner, hints, n) creates or finds a CHANNEL on
 *     this transport, by arbitrary `name` (an opaque token — the paper's `reg` /
 *     `canbus` — never a carrier name; a channel may carry many carriers, D3).
 *     A channel is the unit of connection; endpoints then `attach` to it in a
 *     role.  `round_trip` provisions the medium's write-back shape (a shm reply
 *     ring, a zenoh queryable vs subscriber).  The caller derives it from the
 *     channel's carrier definitions: use 1 when bytes are marked
 *     `mutable_by:'target'`, otherwise use 0.  It remains a channel() argument,
 *     not a field of fss_carrier_t.  A round-trip channel carries the carrier
 *     back to its originator with those bytes mutated in place; a one-way channel
 *     does not.  `owner` says whether THIS endpoint
 *     anchors a 1:1 medium (owner=1 creates it; owner=0 attaches, retrying until
 *     the owner is up).  Which side owns is the caller's rendezvous fact, not the
 *     orchestrator's dialect; what "own" MEANS (create a segment, ignore it for a
 *     symmetric keyed medium) is transport-private.  `carrier_hints` (may be
 *     NULL) is an INDICATIVE list (D3) for best-effort checking, never a
 *     contract.  Returns a channel handle, or NULL if the medium is not yet
 *     present (owner=0 before the owner is up — the caller retries).
 *   - attach(ch, local, role) joins an endpoint to a channel in a ROLE
 *     (fss_role, D2).  It is one verb over the two directions a channel has:
 *       - FSS_ROLE_INITIATOR: install the outbound channel on the endpoint
 *         (local->send / local->send_ctx).  Thereafter the model delivers with
 *         `local->send(local->send_ctx, carrier)` — a direct invocation of the
 *         installed callback, with no transport on the hot path.
 *       - FSS_ROLE_TARGET: install the already-constructed endpoint as this
 *         channel's receive sink, so carriers arriving on the channel (a
 *         cross-process reader thread, a zenoh queryable/subscriber) are
 *         delivered by `local->receive(local->self, carrier)`.  The sink is a
 *         LIVE endpoint the caller already holds (constructed via the placement
 *         facility).
 *     (INITIATOR and TARGET were the v0.1 `bind` and `set_sink`, now one verb;
 *     the channel is a handle from channel(), no longer a locator string.)
 *   - send_to(ch, carrier) is a ONE-SHOT: deliver a single carrier to a channel
 *     with no standing attach.  For occasional traffic — a lifecycle ping, a
 *     config value to a well-known channel — where a standing attach is
 *     unwarranted.
 *   - There is NO transport->send: delivery on an attached channel is the
 *     installed callback (`local->send(...)`), not a transport call.  The
 *     transport's irreducible jobs are name+provision a channel (channel) and
 *     join an endpoint to it (attach) — both make the channel a carrier rides.
 *   - start → a sim::event{Started} carrier delivered (via an attached channel
 *     or send_to) to the fss/sim port.
 *
 * Artefact construction is handled by the PLACEMENT FACILITY
 * (fss/runtime/placement.h), not by the transport.  Construction is ALWAYS
 * LOCAL (the placement rule): fss_place_construct builds in the CALLING
 * process, full stop.  A cross-process deployment has a launcher call
 * fss_place_construct in the far process, and the two processes are then
 * joined with an INITIATOR attach (connect-to-locator).  A transport never
 * reaches across a process boundary to construct; that reach is attach's job.
 *
 * Return-path contract (the round-trip semantics).  A bound `send()` is
 * SYNCHRONOUS and two-way: it returns the peer receive's status, AND on return
 * the carrier's data region reflects any in-place mutation the peer made while
 * handling it (e.g. a TLM b_transport writing a response status / read data, a
 * config request writing its Status field back).  The in-proc wiring meets
 * this for free — an INITIATOR attach installs `local->send = peer->receive`, so
 * the call is literally the peer's receive on the same buffer (§above).  A CROSS-PROCESS
 * round-trip transport (shm reply-ring, zenoh query/reply) MUST reproduce it:
 * ferry the peer's status back, and copy the mutated data region back over the
 * caller's carrier before returning.  A ONE-WAY transport (pub/sub: zenoh
 * z_put → subscriber, the shm one-way ring) cannot honour the write-back half,
 * so it is valid ONLY for carriers with no written-back fields — immutable or
 * fan-out messages (can::frame, a sim::event broadcast).  Choosing a one-way
 * transport for a carrier whose fields are written back is a deployment error,
 * not something the ABI can catch.
 *
 * A transport is itself a loadable PLUGIN.  The host does not link any
 * transport: it dlopens a transport `.so`/`.dylib` chosen at deploy time and
 * resolves the well-known factory symbols `fss_transport_create` /
 * `fss_transport_destroy` (declared below).  The host then uses only this
 * vtable, so it is fully transport-agnostic — swapping shm for zenoh is a
 * config change, not a recompile.  So both transports and artefacts are dlopen
 * plugins against this header-only ABI; the host links neither — only these
 * headers and libdl.
 */
#ifndef FSS_TRANSPORT_ABI_H
#define FSS_TRANSPORT_ABI_H

#include "fss/carrier.h"
#include "fss/endpoint.h"
#include "fss/abi_guard.h"
#include <stddef.h>   /* size_t (channel() hint count) */
#include <stdint.h>   /* uint64_t (carrier id hints)   */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define FSS_EXPORT __declspec(dllexport)
#else
#  define FSS_EXPORT __attribute__((visibility("default")))
#endif

/* Transport plugin negotiation.  These queries are deliberately separate from
 * fss_transport_t: extending the object would make an old host read a changed
 * vtable.  A host must resolve and validate both queries before it calls a
 * factory or dereferences a transport object.
 *
 * Compatibility is same major and a plugin minor no older than the host's
 * required minor.  A later minor can add query symbols or capability bits, but
 * cannot change fss_transport_t. */
#define FSS_TRANSPORT_ABI_VERSION_MAJOR 1u
#define FSS_TRANSPORT_ABI_VERSION_MINOR 0u
#define FSS_TRANSPORT_ABI_VERSION \
    (((uint32_t)FSS_TRANSPORT_ABI_VERSION_MAJOR << 16) | \
     (uint32_t)FSS_TRANSPORT_ABI_VERSION_MINOR)
#define FSS_TRANSPORT_ABI_VERSION_MAJOR_OF(version) ((uint32_t)(version) >> 16)
#define FSS_TRANSPORT_ABI_VERSION_MINOR_OF(version) ((uint32_t)(version) & UINT32_C(0xffff))

#define FSS_TRANSPORT_CAP_CHANNEL UINT64_C(1)
#define FSS_TRANSPORT_CAP_ATTACH  UINT64_C(2)
#define FSS_TRANSPORT_CAP_SEND_TO UINT64_C(4)
/* The plugin exports an optional, transport-specific remote-ready barrier.
 * It does not extend fss_transport_t. */
#define FSS_TRANSPORT_CAP_REMOTE_READY UINT64_C(8)
#define FSS_TRANSPORT_CAP_CHANNEL_ABI \
    (FSS_TRANSPORT_CAP_CHANNEL | FSS_TRANSPORT_CAP_ATTACH | FSS_TRANSPORT_CAP_SEND_TO)

typedef uint32_t (*fss_transport_abi_version_fn_t)(void);
typedef uint64_t (*fss_transport_capabilities_fn_t)(void);

static inline int fss_transport_abi_compatible(uint32_t version, uint64_t capabilities)
{
    return FSS_TRANSPORT_ABI_VERSION_MAJOR_OF(version) ==
               FSS_TRANSPORT_ABI_VERSION_MAJOR &&
           FSS_TRANSPORT_ABI_VERSION_MINOR_OF(version) >=
               FSS_TRANSPORT_ABI_VERSION_MINOR &&
           (capabilities & FSS_TRANSPORT_CAP_CHANNEL_ABI) ==
               FSS_TRANSPORT_CAP_CHANNEL_ABI;
}

/* Return codes.  POSIX-shaped (0 = OK, non-zero = reason).  Carriers are
 * atomic messages — byte-count semantics don't apply, so this is the
 * mq_send(3) shape, not the send(2) one.  A delivered carrier returns
 * whatever the peer's receive returned.  FSS_SEND_UNBOUND is also returned
 * by attach()/send_to() when the channel is not resolvable. */
enum {
    FSS_SEND_OK          = 0,
    FSS_SEND_UNBOUND     = 1,   /* no attached channel, or channel unresolved */
    FSS_SEND_NO_RECEIVER = 2,   /* resolved peer has null receive */
    /* future: FSS_SEND_QUEUE_FULL, FSS_SEND_DISCONNECTED, ... */
};

/* The role an endpoint takes when it attaches to a channel (D2).  A channel is
 * one-directional (initiator -> target, with an optional round-trip write-back),
 * so every attach states which side it is.  BIDI (a target that may also
 * originate) is deferred; INITIATOR/TARGET covers point-to-point, round-trip,
 * and fan-out. */
typedef enum {
    FSS_ROLE_INITIATOR = 0,   /* sends into the channel (installs local->send) */
    FSS_ROLE_TARGET    = 1,   /* receives from the channel (install as sink)   */
} fss_role;

/* A channel handle — an opaque, transport-private object returned by channel()
 * and passed to attach()/send_to().  Its identity is the channel; the transport
 * owns its lifetime (freed at fss_transport_destroy).  A carrier id list may be
 * attached to it as an indicative hint (D3). */
typedef struct fss_channel fss_channel_t;

typedef struct fss_transport fss_transport_t;
struct fss_transport {
    void *self;
    /* channel(name, round_trip, owner, carrier_hints, n): create or find the
     * channel `name` (an arbitrary opaque token, never a carrier name — a channel
     * may carry many carriers, D3) on this transport.  Derive `round_trip` from
     * the channel's carrier definitions (1 if `mutable_by:'target'` requires
     * write-back; 0 otherwise).  It provisions the write-back medium shape and
     * remains an argument, not a fss_carrier_t field (1 = a round-trip channel
     * that carries the carrier back to its originator with mutated bytes in
     * place; 0 = one-way).
     * `owner` = 1 anchors/creates the 1:1 medium, `owner` = 0 attaches to an
     * existing one (retry until the owner is up).  What "own" means is
     * transport-private (create a shm segment; a no-op for a symmetric keyed
     * medium).  `carrier_hints` (may be NULL, n = 0) is an INDICATIVE id list for
     * best-effort checking, never a contract.  Returns a channel handle, or NULL
     * if the medium is not present yet (owner = 0 before the owner — retry). */
    fss_channel_t  *(*channel)     (void *self, const char *name, int round_trip,
                                    int owner, const uint64_t *carrier_hints,
                                    size_t n);
    /* attach(ch, local, role): join `local` to channel `ch` in a role (D2).
     *   FSS_ROLE_INITIATOR — install the outbound channel on `local`
     *     (local->send / local->send_ctx).  Delivery thereafter is
     *     local->send(local->send_ctx, carrier) — no transport call.  Returns
     *     FSS_SEND_OK, or FSS_SEND_UNBOUND on failure.
     *   FSS_ROLE_TARGET — install `local` as `ch`'s inbound receive sink;
     *     carriers arriving on the channel are delivered via
     *     local->receive(local->self, carrier).  Returns FSS_SEND_OK.
     * A cross-process transport (shm/zenoh) MUST implement both roles.  An
     * in-proc direct-wire may leave `channel`/`attach` NULL: its 1:1 edges are
     * wired directly (local->send = peer->receive) by the agent/host, with no
     * transport on the path and no reader thread. */
    int             (*attach)      (void *self, fss_channel_t *ch,
                                    fss_endpoint_t *local, fss_role role);
    /* send_to(ch, carrier): one-shot deliver to a channel, no standing attach
     * (nothing attached, nothing to mutate).  Returns the peer receive's status,
     * or FSS_SEND_UNBOUND if the channel is not resolvable. */
    int             (*send_to)     (void *self, fss_channel_t *ch,
                                    fss_carrier_t *carrier);
};

/* Keep this historic vtable fixed.  Negotiation is symbol based so these
 * size/offset guards remain true for all ABI minor revisions. */
FSS_ABI_STATIC_ASSERT(sizeof(fss_transport_t) == 32,
                       "FSS ABI unsupported layout: fss_transport_t must be 32 bytes");
FSS_ABI_STATIC_ASSERT(offsetof(fss_transport_t, self) == 0,
                       "FSS ABI unsupported layout: fss_transport_t.self offset must be 0");
FSS_ABI_STATIC_ASSERT(offsetof(fss_transport_t, channel) == 8,
                       "FSS ABI unsupported layout: fss_transport_t.channel offset must be 8");
FSS_ABI_STATIC_ASSERT(offsetof(fss_transport_t, attach) == 16,
                       "FSS ABI unsupported layout: fss_transport_t.attach offset must be 16");
FSS_ABI_STATIC_ASSERT(offsetof(fss_transport_t, send_to) == 24,
                       "FSS ABI unsupported layout: fss_transport_t.send_to offset must be 24");

/* Transport-plugin factory symbols.  A transport `.so`/`.dylib` MUST export
 * these; the host dlopens the plugin and resolves them by name.  `config` is
 * a transport-defined opaque string (may be NULL).  `destroy` tears the
 * transport down (closing any nodes it opened). */
FSS_EXPORT fss_transport_t *fss_transport_create (const char *config);
FSS_EXPORT void             fss_transport_destroy(fss_transport_t *transport);

/* Required for plugins using this channel/attach/send_to ABI.  They are
 * resolved dynamically, so their declarations also serve hosts that only know
 * the function typedefs above. */
FSS_EXPORT uint32_t         fss_transport_abi_version (void);
FSS_EXPORT uint64_t         fss_transport_capabilities(void);

/* fss_transport_name(): the transport's own short, stable name — the token by
 * which a deployment selects it (a `control::Transport` field in the topology
 * carrier, an entry in a name->plugin registry).  Lowercase, no path and no
 * file extension (e.g. "inproc", "shm", "zenoh"); a loader that resolves a name
 * to a plugin file (say `libfss_<name>.so`) MUST be able to confirm the loaded
 * plugin by matching this against the requested name.  A plugin MUST export it,
 * and it MUST be constant for the life of the process (independent of any
 * create() config). */
FSS_EXPORT const char      *fss_transport_name   (void);


#ifdef __cplusplus
}
#endif
#endif
