/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * zenoh.c — network / cross-host FSS transport plugin (fss/transport_abi.h),
 * built on eclipse-zenoh/zenoh-c.  See fss_zenoh.h for the public contract and
 * docs/design/zenoh-transport.md for the design.
 *
 * Two carrier-flow shapes (selected at create from `config`):
 *   - request/reply (default): bind() installs a z_get-based send that blocks
 *     for the peer's reply and copies the mutated bytes + status back over the
 *     caller's carrier — the cross-process return-path contract.  The receive
 *     side is a z_queryable whose handler delivers to the sink then replies.
 *   - pub/sub (one-way): bind() installs a z_put-based send (fire-and-forget);
 *     the receive side is a z_subscriber.  For carriers with no write-back.
 *
 * Only atom BYTES cross the boundary.  Wire framing (see zenoh.h):
 *   request payload = [data magic/version][carrier_id : u64 LE][data ...]
 *   reply   payload = [status     : i32 LE][data ...]
 */
#include "fss_zenoh.h"

#include "fss/carrier.h"
#include "fss/endpoint.h"
#include "fss/c/wire.h"      /* generic pointer-inlining serializer */
#include "fss/c/meta.h"      /* fss_meta_flatten: drop non_serializable atoms + re-hash */

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <zenoh.h>

/* ── one zenoh session per PROCESS ────────────────────────────────────────────
 * zenoh wedges past a handful of concurrent z_open()s, so all the per-channel
 * transports in a process SHARE ONE session.  zenoh already multiplexes by
 * key-expression — each transport declares its own queryable/subscriber with its
 * own key + callback context — so N keyed views over one session route
 * independently with no extra code.  The first fss_transport_create() in the
 * process opens the session (with its config's listen/connect); the rest reuse it
 * (every channel in a process carries the SAME rendezvous connectivity, so the
 * open is order-insensitive).  Refcounted: the session closes on the last destroy.
 * Artefacts never see any of this — they only touch endpoints. */
static z_owned_session_t g_session;
static int               g_refcount     = 0;
static int               g_session_open = 0;
static pthread_mutex_t   g_session_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── wire framing ─────────────────────────────────────────────────────────── */
/*
 * A request frame carries enough for the peer to reconstruct the carrier — id,
 * meta (so the receiver can deserialize generically), and the payload:
 *
 *   [data magic/version][u64 id][u32 meta_len][meta bytes][u32 payload_len][payload bytes]
 *
 * For a self-contained carrier the payload is the flat data region verbatim.
 * For a pointer-carrier (fss:local-ptr fields, e.g. TLM bus::Data) the payload
 * is the serialized wire form (fss_carrier_serialize), inlining the pointed-to
 * bytes.  The receiver picks the path from its own fss_carrier_has_pointers on
 * the meta — no flag needed.  The reply frame is the same shape carrying the
 * (mutated) carrier back: [u32 status][u32 payload_len][payload].
 */
#define MAX_DATA  4096u
#define MAX_META  4096u
#define MAX_FRAME (8u + 8u + 4u + MAX_META + 4u + 2u * MAX_DATA + 64u)
/* Keep the wire-validation result aligned with the other transport. */
#define FSS_ZENOH_SEND_INVALID 3

/* Every transport payload has a type header.  This makes readiness probes
 * impossible to decode as carriers (and vice versa), even if an application
 * happens to use a carrier id that looks like a probe magic value. */
static const uint8_t data_magic[8]  = { 'F','S','S','D', 1, 0, 0, 0 };
static const uint8_t probe_magic[8] = { 'F','S','S','P', 1, 0, 0, 0 };
#define PROBE_SIZE 16u                 /* magic/version + u64 challenge */

/* Little-endian put/get into a byte buffer at *off, bounded by cap. */
static int fput(uint8_t *b, size_t *off, size_t cap, const void *src, size_t n) {
    if (*off + n > cap) return 0;
    memcpy(b + *off, src, n); *off += n; return 1;
}
static int fput32(uint8_t *b, size_t *off, size_t cap, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    return fput(b, off, cap, t, 4);
}
static int fput64(uint8_t *b, size_t *off, size_t cap, uint64_t v) {
    uint8_t t[8];
    for (unsigned i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
    return fput(b, off, cap, t, sizeof(t));
}
static int fget(const uint8_t *b, size_t *off, size_t len, void *dst, size_t n) {
    if (*off + n > len) return 0;
    memcpy(dst, b + *off, n); *off += n; return 1;
}
static int fget32(const uint8_t *b, size_t *off, size_t len, uint32_t *v) {
    uint8_t t[4];
    if (!fget(b, off, len, t, 4)) return 0;
    *v = (uint32_t)t[0] | ((uint32_t)t[1]<<8) | ((uint32_t)t[2]<<16) | ((uint32_t)t[3]<<24);
    return 1;
}
static int fget64(const uint8_t *b, size_t *off, size_t len, uint64_t *v) {
    uint8_t t[8];
    if (!fget(b, off, len, t, sizeof(t))) return 0;
    *v = 0;
    for (unsigned i = 0; i < 8; i++) *v |= (uint64_t)t[i] << (8 * i);
    return 1;
}

static int build_probe(uint8_t *buf, size_t cap, uint64_t challenge)
{
    size_t off = 0;
    return fput(buf, &off, cap, probe_magic, sizeof(probe_magic)) &&
           fput64(buf, &off, cap, challenge) && off == PROBE_SIZE;
}

static int parse_probe(const uint8_t *buf, size_t len, uint64_t *challenge)
{
    size_t off = sizeof(probe_magic);
    return len == PROBE_SIZE && memcmp(buf, probe_magic, sizeof(probe_magic)) == 0 &&
           fget64(buf, &off, len, challenge);
}

/* Serialize a carrier's payload (region verbatim, or wire form if it has
 * pointer fields) into `out`.  Returns payload length or -1. */
static long frame_payload(const fss_carrier_t *c, uint8_t *out, size_t cap) {
    if (c->meta_ptr && fss_carrier_has_pointers(c->meta_ptr, c->meta_size)) {
        return fss_carrier_serialize(c->meta_ptr, c->meta_size, c, out, cap);
    }
    if (c->data_size > cap) return -1;
    if (c->data_size && c->data_ptr) memcpy(out, c->data_ptr, c->data_size);
    return (long)c->data_size;
}

/* Build a request/publish frame: [data magic/version][u64 id][u32 meta_len][meta][u32 plen][payload].
 *
 * Crossing a boundary FLATTENS the carrier: every non_serializable atom
 * (core::TypeId — the live-object marker) is dropped and the meta re-hashed, so
 * the wire carries the FLAT id + FLAT meta, never the live id.  This is what
 * stops a receiver from seeing the live id, taking the zero-copy fast path, and
 * casting deserialised flat bytes as a live object.  The payload is serialised
 * by walking the carrier's OWN (live) meta — its real offsets + local-ptr fields
 * describe data_ptr — while the transmitted meta is the flat one (same offsets,
 * marker dropped), which the receiver walks.  Returns frame length or -1. */
static long build_frame(const fss_carrier_t *c, uint8_t *buf, size_t cap) {
    size_t off = 0;
    uint64_t flat_id = c->id;
    uint8_t  flat_meta[MAX_META];
    size_t   flat_meta_len = 0;
    if (c->meta_ptr && c->meta_size) {
        long fl = fss_meta_flatten(c->meta_ptr, c->meta_size,
                                   flat_meta, sizeof(flat_meta), &flat_id);
        if (fl < 0) return -1;
        flat_meta_len = (size_t)fl;
    }
    if (!fput(buf, &off, cap, data_magic, sizeof(data_magic))) return -1;
    if (!fput64(buf, &off, cap, flat_id)) return -1;
    if (!fput32(buf, &off, cap, (uint32_t)flat_meta_len)) return -1;
    if (flat_meta_len && !fput(buf, &off, cap, flat_meta, flat_meta_len)) return -1;
    long pl = frame_payload(c, buf + off + 4, cap - off - 4);
    if (pl < 0) return -1;
    if (!fput32(buf, &off, cap, (uint32_t)pl)) return -1;
    off += (size_t)pl;
    return (long)off;
}

/* ── per-transport state ──────────────────────────────────────────────────── */

typedef struct query_channel query_channel;

typedef struct {
    fss_transport_t   transport;

    /* The zenoh session is process-global (g_session); a view never owns one. */
    int               pubsub;            /* 1 = one-way pub/sub; 0 = request/reply */

    char              key_prefix[256];   /* this node's key expression */

    /* Optional launch-scoped remote-ready edge.  The endpoint is represented
     * by a fixed FNV-1a hash so arbitrary endpoint text never needs escaping
     * for a Zenoh key expression. */
    char              local_endpoint[256];
    char              peer_endpoint[256];
    char              ready_key[320];
    int               edge_role;
    int               edge_set;
    z_owned_liveliness_token_t ready_token;
    int               ready_declared;
    z_owned_queryable_t ready_queryable; /* pub/sub probe acknowledgement */
    int               ready_queryable_declared;
    pthread_mutex_t   probe_lock;
    uint64_t          observed_challenge;
    int               probe_observed;

    fss_endpoint_t   *sink;              /* receive delivery target (set_sink) */
    z_owned_queryable_t  queryable;      /* request/reply receive side */
    z_owned_subscriber_t subscriber;     /* pub/sub receive side */
    int               recv_declared;

    /* Initiator send contexts outlive attach().  Keep every one here so a
     * transport destroy can release it after Zenoh has stopped using the
     * state.  attach_lock also makes a repeat attach deterministic. */
    pthread_mutex_t   attach_lock;
    query_channel    *initiators;
} zenoh_state;

/* Opt-in diagnostics for cross-process readiness failures.  Keep each record
 * under one lock so Zenoh callback threads cannot interleave stderr lines. */
static pthread_once_t  g_trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_trace_enabled;

static void zenoh_trace_init(void)
{
    const char *value = getenv("FSS_ZENOH_TRACE");
    g_trace_enabled = value && value[0] && strcmp(value, "0") != 0;
}

static void zenoh_trace_fields(const char *event, const char *channel,
                               const char *local, const char *peer,
                               const char *detail)
{
    pthread_once(&g_trace_once, zenoh_trace_init);
    if (!g_trace_enabled) return;
    pthread_mutex_lock(&g_trace_lock);
    fprintf(stderr, "fss_zenoh trace pid=%ld event=%s channel=%s local=%s peer=%s%s%s\n",
            (long)getpid(), event, channel && channel[0] ? channel : "-",
            local && local[0] ? local : "-", peer && peer[0] ? peer : "-",
            detail && detail[0] ? " " : "", detail && detail[0] ? detail : "");
    fflush(stderr);
    pthread_mutex_unlock(&g_trace_lock);
}

static void zenoh_trace(const zenoh_state *s, const char *event, const char *detail)
{
    zenoh_trace_fields(event, s->key_prefix, s->local_endpoint,
                       s->peer_endpoint, detail);
}

static uint64_t ready_endpoint_hash(const char *endpoint)
{
    uint64_t h = UINT64_C(14695981039346656037); /* FNV-1a, stable by definition */
    const unsigned char *p = (const unsigned char *)endpoint;
    while (*p) {
        h ^= *p++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* The target's full endpoint name selects its token.  Both peers derive this
 * from the same channel and endpoint, independent of any key escaping rules. */
static int zenoh_ready_key(zenoh_state *s)
{
    const char *target;
    if (!s->edge_set || !s->key_prefix[0]) return 0;
    target = s->edge_role == FSS_ROLE_TARGET ? s->local_endpoint : s->peer_endpoint;
    if (!target[0]) return 0;
    int n = snprintf(s->ready_key, sizeof(s->ready_key), "%s/ready/%016llx",
                     s->key_prefix,
                     (unsigned long long)ready_endpoint_hash(target));
    return n > 0 && (size_t)n < sizeof(s->ready_key);
}

/* The launch edge gives both peers the same target-specific scope.  Include the
 * full ready key, not only its endpoint hash, so distinct channel keys never
 * share a challenge. */
static uint64_t zenoh_probe_challenge(const zenoh_state *s)
{
    uint64_t h = UINT64_C(14695981039346656037);
    const unsigned char *p;
    for (p = (const unsigned char *)s->key_prefix; *p; p++) { h ^= *p; h *= UINT64_C(1099511628211); }
    h ^= 0; h *= UINT64_C(1099511628211);
    for (p = (const unsigned char *)s->ready_key; *p; p++) { h ^= *p; h *= UINT64_C(1099511628211); }
    return h;
}

static int zenoh_declare_ready(zenoh_state *s)
{
    z_view_keyexpr_t ke;
    z_liveliness_token_options_t opts;
    if (s->ready_declared || !s->edge_set || s->edge_role != FSS_ROLE_TARGET)
        return FSS_SEND_OK;
    if (!zenoh_ready_key(s) || z_view_keyexpr_from_str(&ke, s->ready_key) < 0) {
        zenoh_trace(s, "target-token", "result=bad-key");
        return FSS_SEND_UNBOUND;
    }
    z_liveliness_token_options_default(&opts);
    if (z_liveliness_declare_token(z_loan(g_session), &s->ready_token,
                                    z_loan(ke), &opts) < 0) {
        zenoh_trace(s, "target-token", "result=failed");
        return FSS_SEND_UNBOUND;
    }
    s->ready_declared = 1;
    zenoh_trace(s, "target-token", "result=declared");
    return FSS_SEND_OK;
}

/* ── outbound: request/reply (z_get) ──────────────────────────────────────── */

/* Installed as local->send by bind() for a request/reply channel.  Serialise
 * the carrier, z_get the peer's queryable (peer key captured at bind in
 * send_ctx's key), block on a fifo reply channel, copy the reply's mutated
 * bytes + status back over the caller's carrier, return the status. */
struct query_channel {
    zenoh_state *s;
    char         peer_key[256];
    fss_endpoint_t *endpoint;
    query_channel  *next;
};

static int zenoh_query_send(void *send_ctx, fss_carrier_t *carrier)
{
    query_channel *ch = (query_channel *)send_ctx;
    zenoh_state   *s  = ch->s;
    if (!carrier) return FSS_SEND_NO_RECEIVER;
    if (carrier->data_size > MAX_DATA || carrier->meta_size > MAX_META) return 3;

    /* Frame the request, flattening the carrier to its wire (flat) form. */
    uint8_t buf[MAX_FRAME];
    long frame_len = build_frame(carrier, buf, sizeof(buf));
    if (frame_len < 0) return 3;
    size_t off = (size_t)frame_len;

    z_view_keyexpr_t ke;
    if (z_view_keyexpr_from_str(&ke, ch->peer_key) < 0)
        return FSS_SEND_UNBOUND;

    z_owned_bytes_t payload;
    z_bytes_copy_from_buf(&payload, buf, off);

    /* A capacity-1 fifo channel collects the (single) reply synchronously. */
    z_owned_closure_reply_t closure;
    z_owned_fifo_handler_reply_t handler;
    z_fifo_channel_reply_new(&closure, &handler, 1);

    z_get_options_t opts;
    z_get_options_default(&opts);
    opts.payload = z_move(payload);
    /* NB: do NOT shorten opts.timeout_ms here.  z_get already self-bounds (zenoh's
     * default query timeout, ~10 s) so a dead-peer query can't hang forever — and
     * the startup readiness barrier (ts_confirm) RETRIES the first window until OK,
     * so a too-short timeout makes a merely-slow (loaded) reply look like a dead
     * peer and the clock never confirms → federation stalls.  The default is long
     * enough for a loaded live reply and short enough to bound teardown. */

    if (z_get(z_loan(g_session), z_loan(ke), "", z_move(closure), &opts) < 0) {
        z_drop(z_move(handler));
        return FSS_SEND_UNBOUND;
    }

    int rc = FSS_SEND_NO_RECEIVER;
    z_owned_reply_t reply;
    if (z_recv(z_loan(handler), &reply) == Z_OK) {
        if (z_reply_is_ok(z_loan(reply))) {
            const z_loaned_sample_t *sample = z_reply_ok(z_loan(reply));
            z_owned_slice_t slice;
            if (z_bytes_to_slice(z_sample_payload(sample), &slice) == 0) {
                const uint8_t *rb = z_slice_data(z_loan(slice));
                size_t         rn = z_slice_len(z_loan(slice));
                /* Reply frame: [status][payload_len][payload]. */
                size_t roff = 0;
                uint32_t status = 0, plen = 0;
                if (fget32(rb, &roff, rn, &status) && fget32(rb, &roff, rn, &plen) &&
                    plen == rn - roff) {
                    const uint8_t *pp = rb + roff;
                    if (carrier->meta_ptr &&
                        fss_carrier_has_pointers(carrier->meta_ptr, carrier->meta_size)) {
                        /* Pointer-carrier: overlay scalars + write read-data back
                         * into the caller's payload buffer (in place). */
                        if (fss_carrier_apply_reply(carrier->meta_ptr,
                                                    carrier->meta_size,
                                                    carrier, pp, plen) == 0)
                            rc = (int)status;
                        else
                            rc = FSS_ZENOH_SEND_INVALID;
                    } else if (plen == carrier->data_size &&
                               (!plen || carrier->data_ptr)) {
                        /* A self-contained reply must replace the complete
                         * caller region.  Validate its exact size before the
                         * copy so a malformed reply cannot partly mutate it. */
                        if (plen) memcpy(carrier->data_ptr, pp, plen);
                        rc = (int)status;
                    }
                }
                z_drop(z_move(slice));
            }
        }
        z_drop(z_move(reply));
    }
    z_drop(z_move(handler));
    return rc;
}

/* ── outbound: pub/sub (z_put) ────────────────────────────────────────────── */

static int zenoh_put_send(void *send_ctx, fss_carrier_t *carrier)
{
    query_channel *ch = (query_channel *)send_ctx;
    zenoh_state   *s  = ch->s;
    if (!carrier) return FSS_SEND_NO_RECEIVER;
    if (carrier->data_size > MAX_DATA || carrier->meta_size > MAX_META) return 3;

    uint8_t buf[MAX_FRAME];
    long frame_len = build_frame(carrier, buf, sizeof(buf));
    if (frame_len < 0) return 3;
    size_t off = (size_t)frame_len;

    z_view_keyexpr_t ke;
    if (z_view_keyexpr_from_str(&ke, ch->peer_key) < 0)
        return FSS_SEND_UNBOUND;

    z_owned_bytes_t payload;
    z_bytes_copy_from_buf(&payload, buf, off);

    z_put_options_t opts;
    z_put_options_default(&opts);
    /* zenoh's default congestion control is DROP (best-effort, tuned for lossy
     * sample streams): under sustained one-way traffic the session TX buffer
     * fills and frames are SILENTLY dropped, so a device sees e.g. 175/200.  A
     * carrier is a discrete transaction, not a sample we may skip — select BLOCK
     * so a full buffer applies backpressure to the sender instead of losing the
     * frame, and is_express to bypass batching so each frame is emitted promptly.
     * This is what makes a cross-process run deliver all N and be measurable. */
    opts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    opts.is_express         = true;
    if (z_put(z_loan(g_session), z_loan(ke), z_move(payload), &opts) < 0)
        return FSS_SEND_UNBOUND;
    return FSS_SEND_OK;   /* reliable (BLOCK): backpressure, no silent drop */
}

/* ── inbound: decode a request frame into a deliverable carrier ───────────── *
 *
 * Parses [data magic/version][id][meta_len][meta][payload_len][payload] from `pb`.  Fills `c` with
 * an in-process carrier whose data_ptr is `region` (the payload deserialized if
 * the meta has pointer fields — inlined bytes laid into `scratch` — else the
 * payload copied verbatim).  meta_buf holds the carried meta.  Returns 1 on
 * success, 0 on malformed input. */
static int decode_request(const uint8_t *pb, size_t pn,
                          fss_carrier_t *c,
                          uint8_t *region, size_t region_cap,
                          uint8_t *scratch, size_t scratch_cap,
                          uint8_t *meta_buf, size_t meta_cap) {
    size_t off = 0;
    uint64_t id = 0;
    uint32_t meta_len = 0, payload_len = 0;
    if (pn < sizeof(data_magic) || memcmp(pb, data_magic, sizeof(data_magic)) != 0)
        return 0;
    off = sizeof(data_magic);
    if (!fget64(pb, &off, pn, &id)) return 0;
    if (!fget32(pb, &off, pn, &meta_len)) return 0;
    if (meta_len > meta_cap || off + meta_len > pn) return 0;
    memcpy(meta_buf, pb + off, meta_len); off += meta_len;
    if (!fget32(pb, &off, pn, &payload_len)) return 0;
    /* A request frame is self-delimiting.  Do not accept another envelope (or
     * arbitrary bytes) after its payload. */
    if (payload_len != pn - off) return 0;
    const uint8_t *pp = pb + off;

    c->id = id;
    c->meta_ptr = meta_len ? meta_buf : NULL;
    c->meta_size = meta_len;
    c->data_ptr = region;

    if (meta_len && fss_carrier_has_pointers(meta_buf, meta_len)) {
        long rn = fss_carrier_deserialize(meta_buf, meta_len, pp, payload_len,
                                          region, region_cap, scratch, scratch_cap);
        if (rn < 0) return 0;
        c->data_size = (uint64_t)rn;
    } else {
        if (payload_len > region_cap) return 0;
        memcpy(region, pp, payload_len);
        c->data_size = payload_len;
    }
    return 1;
}

/* ── inbound: queryable handler (request/reply receive side) ──────────────── */

static void reply_probe(z_loaned_query_t *query, uint64_t challenge)
{
    uint8_t payload_buf[PROBE_SIZE];
    z_owned_bytes_t payload;
    z_query_reply_options_t opts;
    if (!build_probe(payload_buf, sizeof(payload_buf), challenge)) return;
    z_bytes_copy_from_buf(&payload, payload_buf, sizeof(payload_buf));
    z_query_reply_options_default(&opts);
    z_query_reply(query, z_query_keyexpr(query), z_move(payload), &opts);
}

/* The pub/sub acknowledgement queryable is deliberately separate from the
 * data subscriber.  It confirms that this exact target received the matching
 * data-key probe; it never reports discovery alone as delivery. */
static void ready_query_handler(z_loaned_query_t *query, void *context)
{
    zenoh_state *s = (zenoh_state *)context;
    const z_loaned_bytes_t *payload = z_query_payload(query);
    z_owned_slice_t slice;
    uint64_t challenge, expected;
    int accepted = 0;
    if (!payload || z_bytes_to_slice(payload, &slice) != 0) return;
    if (parse_probe(z_slice_data(z_loan(slice)), z_slice_len(z_loan(slice)), &challenge) &&
        s->edge_set && s->edge_role == FSS_ROLE_TARGET && zenoh_ready_key(s)) {
        expected = zenoh_probe_challenge(s);
        pthread_mutex_lock(&s->probe_lock);
        accepted = challenge == expected && s->probe_observed &&
                   s->observed_challenge == challenge;
        pthread_mutex_unlock(&s->probe_lock);
        if (accepted) reply_probe(query, challenge);
    }
    z_drop(z_move(slice));
    zenoh_trace(s, "probe-ack", accepted ? "result=ack" : "result=not-observed");
}

static int zenoh_declare_ready_queryable(zenoh_state *s)
{
    z_view_keyexpr_t ke;
    z_owned_closure_query_t closure;
    if (!s->pubsub || s->ready_queryable_declared || !s->edge_set ||
        s->edge_role != FSS_ROLE_TARGET)
        return FSS_SEND_OK;
    if (!zenoh_ready_key(s) || z_view_keyexpr_from_str(&ke, s->ready_key) < 0)
        return FSS_SEND_UNBOUND;
    z_closure(&closure, ready_query_handler, NULL, s);
    if (z_declare_queryable(z_loan(g_session), &s->ready_queryable,
                            z_loan(ke), z_move(closure), NULL) < 0) {
        zenoh_trace(s, "probe-ack", "result=declare-failed");
        return FSS_SEND_UNBOUND;
    }
    s->ready_queryable_declared = 1;
    zenoh_trace(s, "probe-ack", "result=declared");
    return FSS_SEND_OK;
}

/* Decode the request frame, deliver the carrier to the sink (which may mutate
 * it in place — e.g. a TLM b_transport writing Status + read data), then reply
 * with [status][payload_len][re-serialized carrier].  Runs on a zenoh thread. */
static void query_handler(z_loaned_query_t *query, void *context)
{
    zenoh_state *s = (zenoh_state *)context;

    const z_loaned_bytes_t *payload = z_query_payload(query);
    if (!payload) return;
    z_owned_slice_t slice;
    if (z_bytes_to_slice(payload, &slice) != 0) return;
    const uint8_t *pb = z_slice_data(z_loan(slice));
    size_t         pn = z_slice_len(z_loan(slice));

    uint64_t challenge;
    if (parse_probe(pb, pn, &challenge)) {
        int accepted = s->edge_set && s->edge_role == FSS_ROLE_TARGET &&
                       zenoh_ready_key(s) && challenge == zenoh_probe_challenge(s);
        z_drop(z_move(slice));
        if (accepted) reply_probe(query, challenge);
        zenoh_trace(s, "probe-query", accepted ? "result=reply" : "result=ignored");
        return;
    }

    uint8_t region[MAX_DATA], scratch[MAX_DATA], meta_buf[MAX_META];
    fss_carrier_t c;
    memset(&c, 0, sizeof(c));
    int ok = decode_request(pb, pn, &c, region, sizeof(region),
                            scratch, sizeof(scratch), meta_buf, sizeof(meta_buf));
    z_drop(z_move(slice));
    if (!ok) return;

    int32_t status = FSS_SEND_NO_RECEIVER;
    if (s->sink && s->sink->receive)
        status = (int32_t)s->sink->receive(s->sink->self, &c);

    /* Frame the reply: [status][payload_len][re-serialized (mutated) carrier]. */
    uint8_t rbuf[MAX_FRAME];
    size_t roff = 0;
    fput32(rbuf, &roff, sizeof(rbuf), (uint32_t)status);
    long pl = frame_payload(&c, rbuf + roff + 4, sizeof(rbuf) - roff - 4);
    if (pl < 0) return;
    fput32(rbuf, &roff, sizeof(rbuf), (uint32_t)pl);
    roff += (size_t)pl;

    z_owned_bytes_t reply_payload;
    z_bytes_copy_from_buf(&reply_payload, rbuf, roff);

    z_query_reply_options_t opts;
    z_query_reply_options_default(&opts);
    z_query_reply(query, z_query_keyexpr(query), z_move(reply_payload), &opts);
}

/* ── inbound: subscriber handler (pub/sub receive side) ───────────────────── */

static void sample_handler(z_loaned_sample_t *sample, void *context)
{
    zenoh_state *s = (zenoh_state *)context;

    const z_loaned_bytes_t *payload = z_sample_payload(sample);
    if (!payload) return;
    z_owned_slice_t slice;
    if (z_bytes_to_slice(payload, &slice) != 0) return;
    const uint8_t *pb = z_slice_data(z_loan(slice));
    size_t         pn = z_slice_len(z_loan(slice));

    uint64_t challenge;
    if (parse_probe(pb, pn, &challenge)) {
        int accepted = s->edge_set && s->edge_role == FSS_ROLE_TARGET &&
                       zenoh_ready_key(s) && challenge == zenoh_probe_challenge(s);
        if (accepted) {
            pthread_mutex_lock(&s->probe_lock);
            s->observed_challenge = challenge;
            s->probe_observed = 1;
            pthread_mutex_unlock(&s->probe_lock);
        }
        z_drop(z_move(slice));
        zenoh_trace(s, "probe-put", accepted ? "result=observed" : "result=ignored");
        return;
    }

    uint8_t region[MAX_DATA], scratch[MAX_DATA], meta_buf[MAX_META];
    fss_carrier_t c;
    memset(&c, 0, sizeof(c));
    int ok = decode_request(pb, pn, &c, region, sizeof(region),
                            scratch, sizeof(scratch), meta_buf, sizeof(meta_buf));
    if (ok && s->sink && s->sink->receive)
        s->sink->receive(s->sink->self, &c);   /* one-way: status discarded */
    z_drop(z_move(slice));
}

/* ── vtable ───────────────────────────────────────────────────────────────── */

/* Register the receive delivery target and declare this node's queryable
 * (request/reply) or subscriber (pub/sub) on its key prefix.  Shared by
 * fss_zenoh_set_sink and open()'s auto-wire.  Idempotent on recv_declared. */
static int zenoh_install_sink(zenoh_state *s, fss_endpoint_t *sink)
{
    int rc = FSS_SEND_UNBOUND;
    pthread_mutex_lock(&s->attach_lock);
    if (!sink || !g_session_open) goto done;
    /* A state owns one receive declaration and thus one sink.  Re-attaching the
     * same endpoint is idempotent; a different endpoint is rejected instead of
     * silently replacing the live callback. */
    if (s->recv_declared) {
        if (sink == s->sink) rc = zenoh_declare_ready(s);
        goto done;
    }
    s->sink = sink;

    z_view_keyexpr_t ke;
    if (z_view_keyexpr_from_str(&ke, s->key_prefix) < 0) {
        fprintf(stderr, "fss_zenoh: bad key prefix '%s'\n", s->key_prefix);
        goto done;
    }

    if (s->pubsub) {
        z_owned_closure_sample_t closure;
        z_closure(&closure, sample_handler, NULL, s);
        if (z_declare_subscriber(z_loan(g_session), &s->subscriber,
                                 z_loan(ke), z_move(closure), NULL) < 0) {
            fprintf(stderr, "fss_zenoh: declare_subscriber failed\n");
            goto done;
        }
    } else {
        z_owned_closure_query_t closure;
        z_closure(&closure, query_handler, NULL, s);
        if (z_declare_queryable(z_loan(g_session), &s->queryable,
                                z_loan(ke), z_move(closure), NULL) < 0) {
            fprintf(stderr, "fss_zenoh: declare_queryable failed\n");
            goto done;
        }
    }
    s->recv_declared = 1;
    /* This order is the readiness contract: peers may proceed only after the
     * data receiver and (for pub/sub) probe acknowledgement endpoint exist. */
    if (zenoh_declare_ready_queryable(s) != FSS_SEND_OK) goto done;
    rc = zenoh_declare_ready(s);
done:
    pthread_mutex_unlock(&s->attach_lock);
    return rc;
}

/* channel(name, round_trip, owner, hints, n): select this view's keyed channel
 * (transport_abi.h).  The zenoh session is process-global and already open (from
 * create); a channel is a key-expression + a flow shape.  `name` becomes the key
 * prefix; `round_trip` picks request/reply (queryable + z_get) vs one-way pub/sub
 * (subscriber + z_put).  `owner` is ignored — a keyed medium is symmetric, both
 * sides just declare their keyed entities.  In v0.1 one channel per transport, so
 * the handle IS the transport state (cast).  `carrier_hints` accepted, ignored
 * (indicative check: R4). */
static fss_channel_t *zenoh_channel_vt(void *self, const char *name, int round_trip,
                                       int owner, const uint64_t *carrier_hints, size_t n)
{
    zenoh_state *s = (zenoh_state *)self;
    (void)owner; (void)carrier_hints; (void)n;   /* symmetric medium; hints: R4 */
    if (!name || !*name) {
        fprintf(stderr, "fss_zenoh: empty channel name\n");
        return NULL;
    }
    if (strlen(name) >= sizeof(s->key_prefix)) {
        fprintf(stderr, "fss_zenoh: channel name is too long\n");
        return NULL;
    }
    if (!g_session_open) return NULL;
    pthread_mutex_lock(&s->attach_lock);
    /* A declared receiver already owns this state's one channel definition. */
    if (s->recv_declared) {
        pthread_mutex_unlock(&s->attach_lock);
        return NULL;
    }
    memcpy(s->key_prefix, name, strlen(name) + 1);
    s->pubsub = round_trip ? 0 : 1;
    if (s->edge_set && !zenoh_ready_key(s)) {
        pthread_mutex_unlock(&s->attach_lock);
        return NULL;
    }
    pthread_mutex_unlock(&s->attach_lock);
    return (fss_channel_t *)s;
}

/* attach(ch, local, role): the single vtable connect verb (transport_abi.h).
 *   FSS_ROLE_INITIATOR — install the send fn (request/reply or publish) with a
 *     channel capturing the peer's key (the channel's own key_prefix).
 *   FSS_ROLE_TARGET — install `local` as the receive sink and declare this
 *     node's queryable/subscriber on the channel's key. */
static int zenoh_attach_vt(void *self, fss_channel_t *ch,
                            fss_endpoint_t *local, fss_role role)
{
    zenoh_state *s = (zenoh_state *)self;
    (void)ch;   /* v0.1: one channel per transport, ch == s */
    if (!local) return FSS_SEND_UNBOUND;
    if (role != FSS_ROLE_INITIATOR && role != FSS_ROLE_TARGET)
        return FSS_SEND_UNBOUND;

    if (role == FSS_ROLE_TARGET) {
        pthread_mutex_lock(&s->attach_lock);
        int edge_mismatch = s->edge_set && role != s->edge_role;
        pthread_mutex_unlock(&s->attach_lock);
        if (edge_mismatch) return FSS_SEND_UNBOUND;
        return zenoh_install_sink(s, local);
    }

    pthread_mutex_lock(&s->attach_lock);
    if (!g_session_open || (s->edge_set && role != s->edge_role)) {
        pthread_mutex_unlock(&s->attach_lock);
        return FSS_SEND_UNBOUND;
    }
    for (query_channel *existing = s->initiators; existing; existing = existing->next) {
        if (existing->endpoint == local) {
            /* Do not replace an endpoint's live context. */
            pthread_mutex_unlock(&s->attach_lock);
            return FSS_SEND_OK;
        }
    }
    query_channel *qc = (query_channel *)calloc(1, sizeof(query_channel));
    if (!qc) {
        pthread_mutex_unlock(&s->attach_lock);
        return FSS_SEND_UNBOUND;
    }
    qc->s = s;
    qc->endpoint = local;
    snprintf(qc->peer_key, sizeof(qc->peer_key), "%s", s->key_prefix);

    local->send     = s->pubsub ? zenoh_put_send : zenoh_query_send;
    local->send_ctx = qc;
    qc->next = s->initiators;
    s->initiators = qc;
    pthread_mutex_unlock(&s->attach_lock);
    return FSS_SEND_OK;
}

static int zenoh_send_to_vt(void *self, fss_channel_t *ch, fss_carrier_t *carrier)
{
    zenoh_state *s = (zenoh_state *)self;
    (void)ch;   /* v0.1: one channel per transport, ch == s */
    if (!s->key_prefix[0]) return FSS_SEND_UNBOUND;
    query_channel qc;
    qc.s = s;
    snprintf(qc.peer_key, sizeof(qc.peer_key), "%s", s->key_prefix);
    return s->pubsub ? zenoh_put_send(&qc, carrier) : zenoh_query_send(&qc, carrier);
}

/* ── public extra symbol ──────────────────────────────────────────────────── */

FSS_EXPORT void fss_zenoh_set_sink(fss_transport_t *transport, fss_endpoint_t *sink)
{
    if (!transport) return;
    (void)zenoh_install_sink((zenoh_state *)transport->self, sink);
}

typedef struct ready_wait {
    pthread_mutex_t lock;
    pthread_cond_t  changed;
    unsigned        references;
    int             live;
    char            expected[320];
    char            channel[256];
    char            local[256];
    char            peer[256];
} ready_wait;

#if defined(__APPLE__)
static int monotonic_remaining(const struct timespec *deadline,
                               struct timespec *remaining)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > deadline->tv_sec ||
        (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
        return 0;
    remaining->tv_sec = deadline->tv_sec - now.tv_sec;
    if (deadline->tv_nsec < now.tv_nsec) {
        remaining->tv_sec--;
        remaining->tv_nsec = deadline->tv_nsec + 1000000000L - now.tv_nsec;
    } else {
        remaining->tv_nsec = deadline->tv_nsec - now.tv_nsec;
    }
    return 1;
}
#endif

static void ready_wait_release(void *context)
{
    ready_wait *wait = (ready_wait *)context;
    if (!wait) return;
    pthread_mutex_lock(&wait->lock);
    if (--wait->references == 0) {
        pthread_mutex_unlock(&wait->lock);
        pthread_cond_destroy(&wait->changed);
        pthread_mutex_destroy(&wait->lock);
        free(wait);
        return;
    }
    pthread_mutex_unlock(&wait->lock);
}

/* A liveliness subscriber reports both the current token set (history) and
 * later changes.  The key expression is exact, but check it again: readiness
 * must never accept a wildcard or malformed event. */
static void ready_sample_handler(z_loaned_sample_t *sample, void *context)
{
    ready_wait *wait = (ready_wait *)context;
    if (!sample) return;
    z_view_string_t key;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);
    char detail[384];
    z_sample_kind_t kind = z_sample_kind(sample);
    snprintf(detail, sizeof(detail), "kind=%s key=%.*s",
             kind == Z_SAMPLE_KIND_PUT ? "put" :
             kind == Z_SAMPLE_KIND_DELETE ? "delete" : "unknown",
             (int)z_string_len(z_loan(key)), z_string_data(z_loan(key)));
    zenoh_trace_fields("ready-event", wait->channel, wait->local, wait->peer, detail);
    pthread_mutex_lock(&wait->lock);
    if (kind == Z_SAMPLE_KIND_PUT &&
        z_string_len(z_loan(key)) == strlen(wait->expected) &&
        memcmp(z_string_data(z_loan(key)), wait->expected,
               strlen(wait->expected)) == 0) {
        wait->live = 1;
        pthread_cond_signal(&wait->changed);
    }
    pthread_mutex_unlock(&wait->lock);
}

static uint32_t deadline_remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t ns;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ns = ((int64_t)deadline->tv_sec - now.tv_sec) * INT64_C(1000000000) +
         deadline->tv_nsec - now.tv_nsec;
    if (ns <= 0) return 0;
    return (uint32_t)((ns + 999999) / 1000000);
}

/* One bounded get and exact probe reply check.  This is used for both the
 * request/reply data-key challenge and the pub/sub acknowledgement key. */
static int zenoh_probe_get(const char *key, uint64_t challenge, uint32_t timeout_ms)
{
    z_view_keyexpr_t ke;
    z_owned_bytes_t payload;
    z_owned_closure_reply_t closure;
    z_owned_fifo_handler_reply_t handler;
    z_get_options_t opts;
    z_owned_reply_t reply;
    uint8_t buf[PROBE_SIZE];
    int ok = 0;
    if (!timeout_ms || z_view_keyexpr_from_str(&ke, key) < 0 ||
        !build_probe(buf, sizeof(buf), challenge)) return 0;
    z_bytes_copy_from_buf(&payload, buf, sizeof(buf));
    z_fifo_channel_reply_new(&closure, &handler, 1);
    z_get_options_default(&opts);
    opts.payload = z_move(payload);
    opts.timeout_ms = timeout_ms;
    if (z_get(z_loan(g_session), z_loan(ke), "", z_move(closure), &opts) < 0) {
        z_drop(z_move(handler));
        return 0;
    }
    if (z_recv(z_loan(handler), &reply) == Z_OK) {
        if (z_reply_is_ok(z_loan(reply))) {
            const z_loaned_sample_t *sample = z_reply_ok(z_loan(reply));
            z_owned_slice_t slice;
            uint64_t got;
            if (z_bytes_to_slice(z_sample_payload(sample), &slice) == 0) {
                ok = parse_probe(z_slice_data(z_loan(slice)), z_slice_len(z_loan(slice)), &got) &&
                     got == challenge;
                z_drop(z_move(slice));
            }
        }
        z_drop(z_move(reply));
    }
    z_drop(z_move(handler));
    return ok;
}

static int zenoh_probe_put(const char *key, uint64_t challenge)
{
    z_view_keyexpr_t ke;
    z_owned_bytes_t payload;
    z_put_options_t opts;
    uint8_t buf[PROBE_SIZE];
    if (z_view_keyexpr_from_str(&ke, key) < 0 || !build_probe(buf, sizeof(buf), challenge))
        return 0;
    z_bytes_copy_from_buf(&payload, buf, sizeof(buf));
    z_put_options_default(&opts);
    /* A readiness retry must return to its deadline even under local pressure. */
    opts.is_express = true;
    return z_put(z_loan(g_session), z_loan(ke), z_move(payload), &opts) >= 0;
}

static int zenoh_challenge_ready(zenoh_state *s, const struct timespec *deadline)
{
    uint64_t challenge = zenoh_probe_challenge(s);
    while (deadline_remaining_ms(deadline)) {
        uint32_t remaining = deadline_remaining_ms(deadline);
        uint32_t window = remaining > 250 ? 250 : remaining;
        int ok;
        if (s->pubsub) {
            (void)zenoh_probe_put(s->key_prefix, challenge);
            ok = zenoh_probe_get(s->ready_key, challenge, window);
        } else {
            ok = zenoh_probe_get(s->key_prefix, challenge, window);
        }
        if (ok) {
            zenoh_trace(s, "challenge", "result=success");
            return FSS_SEND_OK;
        }
    }
    zenoh_trace(s, "challenge", "result=timeout");
    return FSS_SEND_UNBOUND;
}

FSS_EXPORT int fss_zenoh_set_edge(fss_transport_t *transport,
                                  const char *local_endpoint,
                                  const char *peer_endpoint, int role)
{
    if (!transport || !local_endpoint || !*local_endpoint ||
        !peer_endpoint || !*peer_endpoint ||
        (role != FSS_ROLE_INITIATOR && role != FSS_ROLE_TARGET))
        return FSS_SEND_UNBOUND;
    zenoh_state *s = (zenoh_state *)transport->self;
    pthread_mutex_lock(&s->attach_lock);
    if (s->ready_declared || s->recv_declared) {
        pthread_mutex_unlock(&s->attach_lock);
        return FSS_SEND_UNBOUND;
    }
    if (strlen(local_endpoint) >= sizeof(s->local_endpoint) ||
        strlen(peer_endpoint) >= sizeof(s->peer_endpoint)) {
        pthread_mutex_unlock(&s->attach_lock);
        return FSS_SEND_UNBOUND;
    }
    snprintf(s->local_endpoint, sizeof(s->local_endpoint), "%s", local_endpoint);
    snprintf(s->peer_endpoint, sizeof(s->peer_endpoint), "%s", peer_endpoint);
    s->edge_role = role;
    s->edge_set = 1;
    if (s->key_prefix[0] && !zenoh_ready_key(s)) {
        s->edge_set = 0;
        pthread_mutex_unlock(&s->attach_lock);
        zenoh_trace(s, "set-edge", "result=bad-ready-key");
        return FSS_SEND_UNBOUND;
    }
    pthread_mutex_unlock(&s->attach_lock);
    zenoh_trace(s, "set-edge", role == FSS_ROLE_TARGET ? "role=target result=ok" :
                                                    "role=initiator result=ok");
    return FSS_SEND_OK;
}

FSS_EXPORT int fss_zenoh_wait_ready(fss_transport_t *transport, uint32_t timeout_ms)
{
    zenoh_state *s;
    z_view_keyexpr_t ke;
    z_liveliness_subscriber_options_t opts;
    z_owned_closure_sample_t closure;
    z_owned_subscriber_t subscriber;
    ready_wait *wait;
    struct timespec deadline;
#if !defined(__APPLE__)
    pthread_condattr_t condattr;
#endif
    int rc = FSS_SEND_UNBOUND;

    if (!transport || timeout_ms == 0) return FSS_SEND_UNBOUND;
    s = (zenoh_state *)transport->self;
    if (!g_session_open || !s->edge_set || s->edge_role != FSS_ROLE_INITIATOR ||
        !zenoh_ready_key(s) || z_view_keyexpr_from_str(&ke, s->ready_key) < 0)
        return FSS_SEND_UNBOUND;
    wait = (ready_wait *)calloc(1, sizeof(*wait));
    if (!wait) return FSS_SEND_UNBOUND;
    pthread_mutex_init(&wait->lock, NULL);
    /* The deadline and condition variable use the same monotonic clock. */
#if !defined(__APPLE__)
    if (pthread_condattr_init(&condattr) != 0) {
        pthread_mutex_destroy(&wait->lock);
        free(wait);
        return FSS_SEND_UNBOUND;
    }
    if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&wait->changed, &condattr) != 0) {
        pthread_condattr_destroy(&condattr);
        pthread_mutex_destroy(&wait->lock);
        free(wait);
        return FSS_SEND_UNBOUND;
    }
    pthread_condattr_destroy(&condattr);
#else
    if (pthread_cond_init(&wait->changed, NULL) != 0) {
        pthread_mutex_destroy(&wait->lock);
        free(wait);
        return FSS_SEND_UNBOUND;
    }
#endif
    wait->references = 2; /* caller and Zenoh closure */
    snprintf(wait->expected, sizeof(wait->expected), "%s", s->ready_key);
    snprintf(wait->channel, sizeof(wait->channel), "%s", s->key_prefix);
    snprintf(wait->local, sizeof(wait->local), "%s", s->local_endpoint);
    snprintf(wait->peer, sizeof(wait->peer), "%s", s->peer_endpoint);
    z_closure(&closure, ready_sample_handler, ready_wait_release, wait);
    z_liveliness_subscriber_options_default(&opts);
    opts.history = true;
    if (z_liveliness_declare_subscriber(z_loan(g_session), &subscriber,
                                        z_loan(ke), z_move(closure), &opts) < 0) {
        /* z_move transfers the closure even on an error; Zenoh invokes its
         * drop callback after it has released the subscriber request. */
        ready_wait_release(wait);
        return FSS_SEND_UNBOUND;
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&wait->lock);
    while (!wait->live) {
#if defined(__APPLE__)
        /* Darwin has no monotonic condition-variable clock.  Its relative
         * wait preserves this monotonic deadline without polling or sleeping. */
        struct timespec remaining;
        if (!monotonic_remaining(&deadline, &remaining) ||
            pthread_cond_timedwait_relative_np(&wait->changed, &wait->lock,
                                                &remaining) != 0)
            break;
#else
        if (pthread_cond_timedwait(&wait->changed, &wait->lock, &deadline) != 0)
            break;
#endif
    }
    if (wait->live)
        rc = zenoh_challenge_ready(s, &deadline);
    pthread_mutex_unlock(&wait->lock);
    /* Dropping the subscriber stops its callback before the caller's reference
     * is released.  The closure's drop callback releases its reference. */
    z_drop(z_move(subscriber));
    zenoh_trace(s, "wait-ready",
                rc == FSS_SEND_OK ? "result=success" : "result=timeout");
    ready_wait_release(wait);
    return rc;
}

/* ── config parsing ───────────────────────────────────────────────────────── */

/* Parse the ';'-separated config into key prefix + session mode + pubsub flag
 * + zenoh endpoints.  Recognised tokens:
 *   "key=<prefix>"          this node's key-expression prefix.
 *   "mode=<peer|client>"    zenoh session mode.
 *   "pubsub"                select the one-way pub/sub shape.
 *   "listen=<endpoint>"     a zenoh listen endpoint (e.g. tcp/127.0.0.1:7789).
 *   "connect=<endpoint>"    a zenoh connect endpoint.
 * Giving an explicit listen/connect endpoint also disables multicast scouting,
 * so two same-host peers rendezvous deterministically over that endpoint rather
 * than relying on (often-blocked) multicast discovery. */
static void parse_config(zenoh_state *s, const char *config, z_owned_config_t *zc)
{
    z_config_default(zc);
    s->pubsub = 0;
    s->key_prefix[0] = '\0';
    if (!config) return;

    char  tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", config);
    char *save = NULL;
    int   explicit_endpoint = 0;
    for (char *tok = strtok_r(tmp, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        if (strncmp(tok, "key=", 4) == 0) {
            snprintf(s->key_prefix, sizeof(s->key_prefix), "%s", tok + 4);
        } else if (strncmp(tok, "mode=", 5) == 0) {
            char modeval[64];
            snprintf(modeval, sizeof(modeval), "\"%s\"", tok + 5);
            zc_config_insert_json5(z_loan_mut(*zc), "mode", modeval);
        } else if (strcmp(tok, "pubsub") == 0) {
            s->pubsub = 1;
        } else if (strncmp(tok, "listen=", 7) == 0) {
            char v[256];
            snprintf(v, sizeof(v), "[\"%s\"]", tok + 7);
            zc_config_insert_json5(z_loan_mut(*zc), "listen/endpoints", v);
            explicit_endpoint = 1;
        } else if (strncmp(tok, "connect=", 8) == 0) {
            char v[256];
            snprintf(v, sizeof(v), "[\"%s\"]", tok + 8);
            zc_config_insert_json5(z_loan_mut(*zc), "connect/endpoints", v);
            explicit_endpoint = 1;
        }
    }
    if (explicit_endpoint)
        zc_config_insert_json5(z_loan_mut(*zc), "scouting/multicast/enabled", "false");
}

/* ── factory ──────────────────────────────────────────────────────────────── */

FSS_EXPORT const char *fss_transport_name(void) { return "zenoh"; }
FSS_EXPORT uint32_t fss_transport_abi_version(void) { return FSS_TRANSPORT_ABI_VERSION; }
FSS_EXPORT uint64_t fss_transport_capabilities(void)
{
    return FSS_TRANSPORT_CAP_CHANNEL_ABI | FSS_TRANSPORT_CAP_REMOTE_READY;
}

FSS_EXPORT fss_transport_t *fss_transport_create(const char *config)
{
    zenoh_state *s = (zenoh_state *)calloc(1, sizeof(zenoh_state));
    if (!s) return NULL;
    if (pthread_mutex_init(&s->probe_lock, NULL) != 0) {
        free(s);
        return NULL;
    }
    if (pthread_mutex_init(&s->attach_lock, NULL) != 0) {
        pthread_mutex_destroy(&s->probe_lock);
        free(s);
        return NULL;
    }

    z_owned_config_t zc;
    parse_config(s, config, &zc);   /* fills s->key_prefix / s->pubsub (per view); zc is session-level */

    /* Open the process session ONCE (first create wins); every later create in the
     * process reuses it and discards its own config — all channels in a process
     * carry the SAME rendezvous connectivity, so this is order-insensitive. */
    pthread_mutex_lock(&g_session_lock);
    if (g_refcount == 0) {
        z_result_t zr = z_open(&g_session, z_move(zc), NULL);
        if (zr < 0) {
            fprintf(stderr, "fss_zenoh: z_open failed (z_result=%d)\n", (int)zr);
            pthread_mutex_unlock(&g_session_lock);
            pthread_mutex_destroy(&s->attach_lock);
            pthread_mutex_destroy(&s->probe_lock);
            free(s);
            return NULL;
        }
        g_session_open = 1;
    } else {
        z_drop(z_move(zc));   /* session already open — this view just adds keyed entities */
    }
    g_refcount++;
    pthread_mutex_unlock(&g_session_lock);

    s->transport.self         = s;
    s->transport.channel      = zenoh_channel_vt;
    s->transport.attach       = zenoh_attach_vt;
    s->transport.send_to      = zenoh_send_to_vt;
    return &s->transport;
}

FSS_EXPORT void fss_transport_destroy(fss_transport_t *transport)
{
    if (!transport) return;
    zenoh_state *s = (zenoh_state *)transport->self;

    if (s->recv_declared) {
        if (s->pubsub) z_drop(z_move(s->subscriber));
        else           z_drop(z_move(s->queryable));
    }
    if (s->ready_queryable_declared)
        z_drop(z_move(s->ready_queryable));
    if (s->ready_declared)
        z_drop(z_move(s->ready_token));
    /* This view's recv side is dropped (joining its handlers); close the shared
     * process session only when the LAST view goes away. */
    pthread_mutex_lock(&g_session_lock);
    if (g_refcount > 0 && --g_refcount == 0 && g_session_open) {
        z_drop(z_move(g_session));
        g_session_open = 0;
    }
    pthread_mutex_unlock(&g_session_lock);

    /* An initiator endpoint owns no Zenoh entity, but its send context refers
     * to this state.  Entity callbacks and the shared session are gone before
     * these contexts are released.  Endpoints must not be used after their
     * transport is destroyed, as required by the transport ABI. */
    pthread_mutex_lock(&s->attach_lock);
    query_channel *qc = s->initiators;
    s->initiators = NULL;
    pthread_mutex_unlock(&s->attach_lock);
    while (qc) {
        query_channel *next = qc->next;
        free(qc);
        qc = next;
    }
    pthread_mutex_destroy(&s->attach_lock);
    pthread_mutex_destroy(&s->probe_lock);
    free(s);
}
