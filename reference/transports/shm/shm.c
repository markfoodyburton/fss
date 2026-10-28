/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * shm.c — cross-process shared-memory FSS transport, built as a loadable
 * PLUGIN (fss/transport_abi.h).  See shm.h for the public contract and
 * docs/design/shm-transport.md for the design.
 *
 * v0.1: INLINE carriers only, SPSC, one directed channel per instance, named
 * POSIX semaphores as the doorbell.  Only atom BYTES cross the boundary — the
 * carrier's flat data region is memcpy'd into a shared ring slot and the peer
 * is signalled; no C/C++ object, no pointer, ever crosses (design §1).  The
 * carrier META travels too, but amortised: the sender publishes each distinct
 * flat meta once into a shared table and frames carry its table discriminator.
 * This does not assume that a carrier id uniquely identifies metadata (for
 * example, extension-neutral ids can have distinct metadata).  The
 * data copy is therefore a flat memcpy of a trivially-copyable region; a model
 * that hands the SHM path a non-memcpy-safe type (e.g. tlm_generic_payload,
 * whose virtual dtor makes it non-trivially-copyable) is rejected at the
 * C++ SHM-facing API's static_assert — this C core only sees the flat bytes.
 *
 * macOS primitives (proven by docs/design/exploration/shm-spike/shm_spike.c):
 *   - shm_open / ftruncate / mmap and sem_open / sem_post / sem_wait all live
 *     in libSystem — NO -lrt / -lpthread needed on macOS.
 *   - sem_init(pshared=1) is ENOSYS on macOS, so we use NAMED semaphores.
 *   - shm_unlink before shm_open(O_CREAT|O_EXCL) clears a stale name left by a
 *     crashed run (ENOENT on a fresh name is harmless).
 */
#include "shm.h"

#include "fss/carrier.h"
#include "fss/endpoint.h"
#include "fss/c/wire.h"   /* pointer-payload (fss:local-ptr) inlining across the boundary */
#include "fss/c/meta.h"   /* fss_meta_flatten: drop non_serializable atoms + re-hash to the flat id */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── segment layout (design §3) ───────────────────────────────────────────── */

#define FSS_SHM_MAGIC      0x46535348u   /* "FSSH" */
#define FSS_SHM_VERSION    4u             /* v4: slots discriminate exact published metadata */
#define FSS_SHM_RING_SIZE  256u          /* default slot count (power of two)   */
#define SLOT_DATA_CAP      4096u         /* per-slot payload capacity (inline OR
                                          * a serialised pointer-carrier, e.g. a
                                          * TLM txn with its bus::Data inlined).
                                          * Must fit the widest control carrier:
                                          * the control::plan carrier (R5) carries
                                          * an agent's whole build+wire plan as a
                                          * JSON PlanJson slot (3072 B) + Status,
                                          * so ~3.1 KB — well above the old
                                          * fss_ctrl_instr (288 B).               */

/* Published-meta table (design: a carrier IS id + meta + data, so SHM must
 * carry the meta to be conformant — see docs/spec/01-carrier.md).  Exact flat
 * metadata is published once and a slot identifies its entry.  Sized for a
 * handful of carrier types on
 * a v0.1 SPSC channel; FSS_SHM_META_BYTES comfortably holds e.g. the can::frame
 * meta (~412 B). */
#define FSS_SHM_META_TYPES 8u
#define FSS_SHM_META_BYTES 2048u   /* holds e.g. the flat TLM meta (~1.1 KB) */

/* This is an ABI layout selector, not a statement about carrier semantics or
 * directionality.  The transport has no carrier metadata when channel() runs.
 * Both peers must therefore supply the same round_trip value selected by the
 * deployment from the channel definition. */
#define FSS_SHM_MODE_ONE_WAY    0u
#define FSS_SHM_MODE_ROUND_TRIP 1u

/* The transport ABI reserves 0--2.  The existing SHM implementation returns
 * 3 for a carrier it cannot represent; retain that local, explicit failure. */
#define FSS_SHM_SEND_INVALID 3

typedef struct {
    uint32_t magic;       /* 0x46535348 */
    uint32_t version;     /* FSS_SHM_VERSION */
    uint32_t ring_size;   /* slot count (each ring) */
    uint32_t meta_count;  /* published meta entries (writer: the request sender) */
    uint64_t head;        /* request ring: writer increments; reader reads */
    uint64_t tail;        /* request ring: reader increments; writer reads */
    uint64_t reply_head;  /* reply ring: reader (receiver) increments       */
    uint64_t reply_tail;  /* reply ring: writer (sender) increments         */
    uint32_t mode;        /* FSS_SHM_MODE_*: fixed layout compatibility field */
    uint32_t layout_size; /* complete segment byte size for this layout */
    uint8_t  _pad2[8];    /* round the fixed header to 64 bytes */
} shm_header_t;

_Static_assert(sizeof(shm_header_t) == 64, "SHM header must remain fixed-size");

/* One published carrier meta.  `id` is checked when resolving the slot's
 * discriminator, but is not used as the cache key. */
typedef struct {
    uint64_t id;                          /* carrier id (0 = empty)    */
    uint32_t size;                        /* meta byte count           */
    uint8_t  bytes[FSS_SHM_META_BYTES];   /* canonical CBOR meta       */
} meta_entry_t;

/* A request slot carries the carrier id + flat data bytes.  On the REPLY ring
 * the same struct also carries the receiver's status: a round-trip send writes
 * the request, the receiver mutates the flat bytes in place (e.g. a write-back
 * Status field) and echoes them back here along with the int it returned. */
typedef struct {
    uint64_t carrier_id;
    uint32_t data_size;
    uint32_t meta_index; /* published-meta entry, or UINT32_MAX for no meta */
    int32_t  status;     /* reply ring: the receiver's return code (request ring: unused) */
    uint8_t  data[SLOT_DATA_CAP];
} slot_t;

/* ── artefact ABI (fss/artefact_abi.h) ────────────────────────────────────── */
/* open("<node>=<path>[@<inbound-endpoint>]") can now bring an artefact to life
 * locally (dlopen + fss_create) and wire its inbound endpoint as the receive
 * sink — the universal "a transport instantiates the artefact" path
 * (transport_abi.h / artefact_abi.h), the cross-process analogue of inproc's
 * open().  SHM still carries only atom BYTES across the boundary; the hosted
 * artefact runs in THIS (the receiver's) process. */

/* ── per-transport state ──────────────────────────────────────────────────── */

typedef struct {
    fss_transport_t  transport;

    char             channel[256];
    int              is_creator;
    int              round_trip;   /* 1 = synchronous request/reply ring enabled */

    void            *map;          /* base of the mmap'd segment */
    size_t           map_size;
    shm_header_t    *hdr;
    slot_t          *slots;        /* request ring */
    slot_t          *reply_slots;  /* reply ring (round_trip only) */
    meta_entry_t    *metas;        /* published-meta table (after the rings) */

    sem_t           *sem_empty;    /* free request slots the writer may fill */
    sem_t           *sem_full;     /* ready request slots the reader may consume */
    sem_t           *sem_reply_empty;  /* free reply slots (round_trip)     */
    sem_t           *sem_reply_full;   /* ready replies the sender may take (round_trip) */

    char             shm_name[256];
    char             sem_empty_name[256];
    char             sem_full_name[256];
    char             sem_reply_empty_name[256];
    char             sem_reply_full_name[256];

    fss_endpoint_t  *sink;         /* receive delivery target (fss_shm_set_sink) */

    pthread_mutex_t  sink_lock;    /* serializes sink ownership and reader start */
    pthread_mutex_t  close_lock;   /* protects senders while destroy is in progress */
    /* The shared rings have one producer.  This lock extends that property to
     * concurrent local callers, including the complete RR request/reply pair. */
    pthread_mutex_t  sender_lock;
    pthread_cond_t   close_cond;
    unsigned         active_senders;
    /* Each count covers the interval from the stop check through sem_wait's
     * return.  destroy snapshots them while holding close_lock, so it cannot
     * miss one of several threads that is about to sleep on a semaphore. */
    unsigned         wait_empty;
    unsigned         wait_full;
    unsigned         wait_reply_empty;
    unsigned         wait_reply_full;

    pthread_t        reader;
    int              reader_started;
    atomic_int       stop;         /* set on destroy to unblock the reader */
} shm_state;

/* ── name construction ────────────────────────────────────────────────────── */

static void make_names(shm_state *s, const char *channel)
{
    snprintf(s->channel,        sizeof(s->channel),        "%s", channel);
    snprintf(s->shm_name,       sizeof(s->shm_name),       "/fss_shm_%s",    channel);
    snprintf(s->sem_empty_name, sizeof(s->sem_empty_name), "/fss_sem_%s_e",  channel);
    snprintf(s->sem_full_name,  sizeof(s->sem_full_name),  "/fss_sem_%s_f",  channel);
    snprintf(s->sem_reply_empty_name, sizeof(s->sem_reply_empty_name), "/fss_sem_%s_re", channel);
    snprintf(s->sem_reply_full_name,  sizeof(s->sem_reply_full_name),  "/fss_sem_%s_rf", channel);
}

static size_t shm_layout_size(int round_trip)
{
    size_t n_rings = round_trip ? 2u : 1u;
    return sizeof(shm_header_t)
         + n_rings * (size_t)FSS_SHM_RING_SIZE * sizeof(slot_t)
         + (size_t)FSS_SHM_META_TYPES * sizeof(meta_entry_t);
}

/* The header contains both immutable ABI fields and live ring counters.  Check
 * the ABI fields one by one: comparing the whole object would include padding
 * and the counters change while an attacher is looking at it. */
static int shm_header_is_compatible(const shm_header_t *hdr, int round_trip,
                                    size_t expected_layout)
{
    uint32_t wanted_mode = round_trip ? FSS_SHM_MODE_ROUND_TRIP
                                      : FSS_SHM_MODE_ONE_WAY;
    uint32_t magic = atomic_load_explicit((_Atomic uint32_t *)&hdr->magic,
                                          memory_order_acquire);
    uint32_t version = atomic_load_explicit((_Atomic uint32_t *)&hdr->version,
                                            memory_order_relaxed);
    uint32_t ring_size = atomic_load_explicit((_Atomic uint32_t *)&hdr->ring_size,
                                              memory_order_relaxed);
    uint32_t mode = atomic_load_explicit((_Atomic uint32_t *)&hdr->mode,
                                         memory_order_relaxed);
    uint32_t layout_size = atomic_load_explicit((_Atomic uint32_t *)&hdr->layout_size,
                                                memory_order_relaxed);

    return magic == FSS_SHM_MAGIC &&
           version == FSS_SHM_VERSION &&
           ring_size == FSS_SHM_RING_SIZE &&
           mode == wanted_mode &&
           layout_size == expected_layout;
}

/* ── published-meta table ─────────────────────────────────────────────────── */

/* Sender side: publish a distinct (flat) meta once.  Scans the (small) table
 * for an exact id, size, and byte match; if absent and there is room, appends
 * the entry and bumps
 * meta_count with a release store so the reader, which loads meta_count with
 * acquire, sees fully-written bytes.  Single writer (the request sender), so no
 * lock is needed.  A carrier which supplied meta must not be sent unless that
 * meta is available to the peer: silently dropping it changes its meaning. */
static int shm_publish_meta(shm_state *s, uint64_t id, const uint8_t *meta,
                            uint32_t size, uint32_t *index_out)
{
    if (!meta || !size || size > FSS_SHM_META_BYTES || !index_out) return -1;

    uint32_t n = atomic_load_explicit((_Atomic uint32_t *)&s->hdr->meta_count,
                                       memory_order_acquire);
    if (n > FSS_SHM_META_TYPES) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (s->metas[i].id == id && s->metas[i].size == size &&
            memcmp(s->metas[i].bytes, meta, size) == 0) {
            *index_out = i;                  /* exact meta already published */
            return 0;
        }
    }
    if (n >= FSS_SHM_META_TYPES) return -1;   /* table full */

    s->metas[n].size = size;
    memcpy(s->metas[n].bytes, meta, size);
    s->metas[n].id = id;
    atomic_store_explicit((_Atomic uint32_t *)&s->hdr->meta_count, n + 1,
                          memory_order_release);
    *index_out = n;
    return 0;
}

/* A close wakes every semaphore waiter.  Register under close_lock after the
 * stop check and before sem_wait.  destroy uses this count to post once for
 * each waiter, rather than assuming that one post wakes all waiters. */
static int shm_wait(shm_state *s, sem_t *sem, unsigned *waiters)
{
    int rc;

    pthread_mutex_lock(&s->close_lock);
    if (atomic_load_explicit(&s->stop, memory_order_acquire)) {
        pthread_mutex_unlock(&s->close_lock);
        return -1;
    }
    ++*waiters;
    pthread_mutex_unlock(&s->close_lock);

    do { rc = sem_wait(sem); } while (rc < 0 && errno == EINTR);

    pthread_mutex_lock(&s->close_lock);
    --*waiters;
    int stopped = atomic_load_explicit(&s->stop, memory_order_acquire);
    pthread_mutex_unlock(&s->close_lock);
    return rc < 0 || stopped ? -1 : 0;
}

/* Keep a sender alive while it waits for the producer lock or a semaphore.
 * destroy wakes semaphore waiters, then waits for this count before it closes
 * or unmaps storage. */
static int shm_sender_enter(shm_state *s)
{
    int open;
    pthread_mutex_lock(&s->close_lock);
    open = !atomic_load_explicit(&s->stop, memory_order_acquire);
    if (open) s->active_senders++;
    pthread_mutex_unlock(&s->close_lock);
    return open ? FSS_SEND_OK : FSS_SEND_UNBOUND;
}

static void shm_sender_leave(shm_state *s)
{
    pthread_mutex_lock(&s->close_lock);
    if (--s->active_senders == 0)
        pthread_cond_broadcast(&s->close_cond);
    pthread_mutex_unlock(&s->close_lock);
}

/* A sender first joins active_senders, then waits for the local producer lock.
 * destroy waits for active_senders, so it cannot free this mutex while a caller
 * is blocked on it.  Recheck stop after acquisition: a caller released by a
 * preceding sender during teardown must not publish a new request. */
static int shm_sender_lock(shm_state *s)
{
    pthread_mutex_lock(&s->sender_lock);
    if (atomic_load_explicit(&s->stop, memory_order_acquire)) {
        pthread_mutex_unlock(&s->sender_lock);
        return FSS_SEND_UNBOUND;
    }
    return FSS_SEND_OK;
}

/* Reader side: resolve a slot's metadata discriminator (and verify its id).
 * Bytes live in the mapped segment and are valid for the receive call's
 * duration.  Returns NULL if unpublished or malformed. */
static const uint8_t *shm_resolve_meta(shm_state *s, uint32_t index, uint64_t id,
                                       uint32_t *size_out)
{
    uint32_t n = atomic_load_explicit((_Atomic uint32_t *)&s->hdr->meta_count,
                                        memory_order_acquire);
    if (n > FSS_SHM_META_TYPES) n = FSS_SHM_META_TYPES;
    if (index < n && s->metas[index].id == id) {
        if (s->metas[index].size <= FSS_SHM_META_BYTES) {
            *size_out = s->metas[index].size;
            return s->metas[index].bytes;
        }
    }
    *size_out = 0;
    return NULL;
}

/* ── ring write (design §4: shm_send_fn / send_to share this) ─────────────── */

static int shm_write_slot(shm_state *s, fss_carrier_t *carrier,
                          uint64_t *transmitted_id)
{
    if (!carrier) return FSS_SEND_NO_RECEIVER;
    if ((!carrier->meta_ptr) != (!carrier->meta_size)) {
        fprintf(stderr, "fss_shm: carrier metadata pointer/size mismatch\n");
        return FSS_SHM_SEND_INVALID;
    }

    /* FLATTEN, then publish the FLAT meta under the FLAT id.  Crossing a boundary
     * drops every non_serializable atom (core::TypeId — the live-object marker)
     * and re-hashes, so the slot carries the flat id; a receiver therefore never
     * sees the live id and never casts flat bytes as a live object (mirrors
     * zenoh's build_frame).  A self-contained carrier (e.g. can::frame) flattens
     * to itself — same id, same meta. */
    uint64_t flat_id = carrier->id;
    uint32_t meta_index = UINT32_MAX;
    if (carrier->meta_ptr && carrier->meta_size) {
        uint8_t flat_meta[FSS_SHM_META_BYTES];
        long fl = fss_meta_flatten(carrier->meta_ptr, carrier->meta_size,
                                   flat_meta, sizeof(flat_meta), &flat_id);
        if (fl < 0) {
            fprintf(stderr, "fss_shm: meta flatten failed (meta > %u?)\n", FSS_SHM_META_BYTES);
            return FSS_SHM_SEND_INVALID;
        }
        if (fl == 0 || shm_publish_meta(s, flat_id, flat_meta, (uint32_t)fl,
                                        &meta_index) != 0) {
            fprintf(stderr, "fss_shm: metadata table is full or invalid\n");
            return FSS_SHM_SEND_INVALID;
        }
    }

    /* A pointer-carrier (fss:local-ptr fields, e.g. a TLM txn's bus::Data) is
     * SERIALISED — its pointed-to bytes inlined — into the slot; a self-contained
     * carrier is a verbatim region copy.  Either way the slot holds a flat
     * payload bounded by SLOT_DATA_CAP.  (We serialise with the carrier's OWN
     * meta — real offsets + local-ptr fields — exactly as zenoh's frame_payload.) */
    int has_ptr = carrier->meta_ptr &&
                  fss_carrier_has_pointers(carrier->meta_ptr, carrier->meta_size);
    if (!has_ptr && carrier->data_size > SLOT_DATA_CAP) {
        fprintf(stderr, "fss_shm: carrier data_size %llu exceeds SLOT_DATA_CAP %u\n",
                (unsigned long long)carrier->data_size, SLOT_DATA_CAP);
        return FSS_SHM_SEND_INVALID;
    }
    if (carrier->data_size && !carrier->data_ptr) {
        fprintf(stderr, "fss_shm: carrier has data size but no data pointer\n");
        return FSS_SHM_SEND_INVALID;
    }

    if (shm_wait(s, s->sem_empty, &s->wait_empty) < 0) /* block if ring full */
        return FSS_SEND_UNBOUND;

    uint64_t head = atomic_load_explicit((_Atomic uint64_t *)&s->hdr->head,
                                         memory_order_relaxed);
    slot_t *slot = &s->slots[head % s->hdr->ring_size];

    slot->carrier_id = flat_id;
    slot->meta_index = meta_index;
    slot->status     = 0;
    if (has_ptr) {
        long n = fss_carrier_serialize(carrier->meta_ptr, carrier->meta_size,
                                       carrier, slot->data, SLOT_DATA_CAP);
        if (n < 0) {
            fprintf(stderr, "fss_shm: serialize failed (payload > SLOT_DATA_CAP %u?)\n",
                    SLOT_DATA_CAP);
            sem_post(s->sem_empty);   /* release the slot we reserved */
            return FSS_SHM_SEND_INVALID;
        }
        slot->data_size = (uint32_t)n;
    } else {
        slot->data_size = (uint32_t)carrier->data_size;
        if (carrier->data_size && carrier->data_ptr)
            memcpy(slot->data, carrier->data_ptr, carrier->data_size);
    }

    /* release: the slot writes above must be visible before the reader, woken
     * by sem_post, observes the advanced head.  (The sem itself is also a
     * barrier; the atomic store documents the SPSC ordering intent.) */
    atomic_store_explicit((_Atomic uint64_t *)&s->hdr->head, head + 1,
                          memory_order_release);

    if (sem_post(s->sem_full) < 0)
        return FSS_SEND_UNBOUND;
    if (transmitted_id)
        *transmitted_id = flat_id;
    return FSS_SEND_OK;
}

/* Installed as local->send by bind() for a ONE-WAY channel; send_ctx is the
 * shm_state.  Fire-and-forget: no status from the peer, no write-back. */
static int shm_send_fn(void *send_ctx, fss_carrier_t *carrier)
{
    shm_state *s = (shm_state *)send_ctx;
    if (shm_sender_enter(s) != FSS_SEND_OK) return FSS_SEND_UNBOUND;
    if (shm_sender_lock(s) != FSS_SEND_OK) {
        shm_sender_leave(s);
        return FSS_SEND_UNBOUND;
    }
    int rc = shm_write_slot(s, carrier, NULL);
    pthread_mutex_unlock(&s->sender_lock);
    shm_sender_leave(s);
    return rc;
}

/* Installed as local->send by bind() for a ROUND-TRIP channel.  Writes the
 * request, then blocks on the reply ring until the receiver has handled it,
 * copies the receiver's (possibly mutated) bytes back over the caller's carrier
 * and returns the receiver's status.  This is the cross-process analogue of the
 * in-proc identity send = peer->receive: synchronous, two-way, with in-place
 * write-back (transport_abi.h "Return-path contract").  SPSC: one outstanding
 * request at a time. */
static int shm_request_reply(void *send_ctx, fss_carrier_t *carrier)
{
    shm_state *s = (shm_state *)send_ctx;
    uint64_t transmitted_id;
    if (shm_sender_enter(s) != FSS_SEND_OK) return FSS_SEND_UNBOUND;
    if (shm_sender_lock(s) != FSS_SEND_OK) {
        shm_sender_leave(s);
        return FSS_SEND_UNBOUND;
    }
    int rc = shm_write_slot(s, carrier, &transmitted_id);
    if (rc != FSS_SEND_OK) {
        pthread_mutex_unlock(&s->sender_lock);
        shm_sender_leave(s);
        return rc;
    }

    if (shm_wait(s, s->sem_reply_full, &s->wait_reply_full) < 0) {
        pthread_mutex_unlock(&s->sender_lock);
        shm_sender_leave(s);
        return FSS_SEND_UNBOUND;
    }

    uint64_t rt = atomic_load_explicit((_Atomic uint64_t *)&s->hdr->reply_tail,
                                       memory_order_relaxed);
    slot_t *rslot = &s->reply_slots[rt % s->hdr->ring_size];

    /* Write the receiver's (possibly mutated) bytes back over the caller's
     * ORIGINAL carrier, then read the status.  A pointer-carrier applies the
     * reply through the meta — read-data lands in the caller's own pointed-to
     * buffers, pointers preserved (the write-back half of the return-path
     * contract); a self-contained carrier copies the region verbatim. */
    int apply_rc = 0;
    /* A reply is valid only for this request's flattened wire identity.  The
     * caller's id can differ when metadata flattening drops local-only atoms. */
    if (rslot->carrier_id != transmitted_id) {
        apply_rc = -1;
    } else if (rslot->data_size > SLOT_DATA_CAP) {
        apply_rc = -1;
    } else if (carrier->meta_ptr &&
        fss_carrier_has_pointers(carrier->meta_ptr, carrier->meta_size)) {
        apply_rc = fss_carrier_apply_reply(carrier->meta_ptr, carrier->meta_size,
                                           carrier, rslot->data, rslot->data_size);
    } else if (rslot->data_size && !carrier->data_ptr) {
        /* A data reply has nowhere to go.  Empty request/empty reply is valid. */
        apply_rc = -1;
    } else if (rslot->data_size != carrier->data_size) {
        apply_rc = -1;
    } else if (rslot->data_size) {
        memcpy(carrier->data_ptr, rslot->data, rslot->data_size);
    }
    int status = rslot->status;

    atomic_store_explicit((_Atomic uint64_t *)&s->hdr->reply_tail, rt + 1,
                          memory_order_release);
    sem_post(s->sem_reply_empty);              /* free the reply slot */
    rc = apply_rc == 0 ? status : FSS_SHM_SEND_INVALID;
    pthread_mutex_unlock(&s->sender_lock);
    shm_sender_leave(s);
    return rc;
}

/* ── reader thread (design §4) ────────────────────────────────────────────── */

static void *shm_reader(void *arg)
{
    shm_state *s = (shm_state *)arg;
    for (;;) {
        if (shm_wait(s, s->sem_full, &s->wait_full) < 0) break;

        uint64_t tail = atomic_load_explicit((_Atomic uint64_t *)&s->hdr->tail,
                                             memory_order_relaxed);
        slot_t *slot = &s->slots[tail % s->hdr->ring_size];

        int status = FSS_SEND_NO_RECEIVER;
        int malformed = slot->data_size > SLOT_DATA_CAP;

        /* Reconstruct a local carrier.  Attach the published (flat) meta for this
         * id — so a meta-driven receiver (e.g. the SystemC binding) can map the
         * carrier, the same as in-proc / zenoh.  A pointer-carrier's slot bytes
         * are the SERIALISED form: DESERIALISE into local region+scratch (the
         * carrier's data home for this call) and repoint its local-ptr fields; a
         * self-contained carrier points straight into the slot.  The receive may
         * MUTATE these bytes in place (e.g. write back a Status field / read data);
         * on a round-trip channel we re-serialise and echo them back. */
        fss_carrier_t c;
        c.id        = slot->carrier_id;
        uint32_t meta_sz = 0;
        c.meta_ptr = NULL;
        c.meta_size = 0;
        /* UINT32_MAX is the only valid discriminator for a carrier without
         * metadata.  Any other discriminator must name a published entry with
         * the same id.  Do this before dispatch: treating a bad discriminator
         * as no metadata could make a meta-driven sink accept a different
         * carrier shape. */
        if (slot->meta_index != UINT32_MAX) {
            c.meta_ptr = (void *)shm_resolve_meta(s, slot->meta_index,
                                                   slot->carrier_id, &meta_sz);
            c.meta_size = meta_sz;
            if (!c.meta_ptr || !c.meta_size)
                malformed = 1;
        }

        uint8_t region[SLOT_DATA_CAP], scratch[SLOT_DATA_CAP];
        int has_ptr = c.meta_ptr && fss_carrier_has_pointers(c.meta_ptr, c.meta_size);
        if (malformed) {
            goto reply;
        } else if (has_ptr) {
            long rn = fss_carrier_deserialize(c.meta_ptr, c.meta_size,
                                              slot->data, slot->data_size,
                                              region, sizeof(region),
                                              scratch, sizeof(scratch));
            if (rn < 0) {
                fprintf(stderr, "fss_shm: deserialize failed\n");
                malformed = 1;
                goto reply;     /* reply with an error; never strand an RR sender */
            }
            c.data_ptr  = region;
            c.data_size = (uint64_t)rn;
        } else {
            c.data_ptr  = slot->data;
            c.data_size = slot->data_size;
        }

        if (s->sink && s->sink->receive)
            status = s->sink->receive(s->sink->self, &c);

    reply:
        if (s->round_trip) {
            /* Echo the (possibly mutated) carrier + status onto the reply ring,
             * unblocking the sender waiting in shm_request_reply.  Re-serialise a
             * pointer-carrier (the receiver may have written read-data into its
             * pointed-to bytes); copy the region verbatim otherwise. */
            if (shm_wait(s, s->sem_reply_empty, &s->wait_reply_empty) == 0) {
                uint64_t rh = atomic_load_explicit((_Atomic uint64_t *)&s->hdr->reply_head,
                                                   memory_order_relaxed);
                slot_t *rslot = &s->reply_slots[rh % s->hdr->ring_size];
                rslot->carrier_id = slot->carrier_id;
                rslot->status     = malformed ? FSS_SHM_SEND_INVALID : status;
                if (has_ptr && !malformed) {
                    long rn = fss_carrier_serialize(c.meta_ptr, c.meta_size,
                                                    &c, rslot->data, SLOT_DATA_CAP);
                    if (rn < 0) {
                        rslot->data_size = 0;
                        rslot->status = FSS_SHM_SEND_INVALID;
                    } else rslot->data_size = (uint32_t)rn;
                } else {
                    if (!malformed && (c.data_size > SLOT_DATA_CAP ||
                                       (c.data_size && !c.data_ptr))) {
                        rslot->data_size = 0;
                        rslot->status = FSS_SHM_SEND_INVALID;
                    } else {
                        rslot->data_size = malformed ? 0 : (uint32_t)c.data_size;
                    }
                    if (!malformed && rslot->status != FSS_SHM_SEND_INVALID && c.data_size)
                        memcpy(rslot->data, c.data_ptr, c.data_size);
                }
                atomic_store_explicit((_Atomic uint64_t *)&s->hdr->reply_head, rh + 1,
                                      memory_order_release);
                sem_post(s->sem_reply_full);
            }
        }

    advance:
        atomic_store_explicit((_Atomic uint64_t *)&s->hdr->tail, tail + 1,
                              memory_order_release);
        sem_post(s->sem_empty);   /* free the slot */
    }
    return NULL;
}

/* ── segment + semaphore setup ────────────────────────────────────────────── */

static int shm_setup(shm_state *s, const char *channel, int is_creator, int round_trip)
{
    round_trip = !!round_trip;
    make_names(s, channel);
    s->is_creator = is_creator;
    s->round_trip = round_trip;
    /* One ring for one-way; a second (reply) ring of equal size for round-trip;
     * plus the published-meta table at the tail of the segment. */
    size_t n_rings = round_trip ? 2u : 1u;
    s->map_size = shm_layout_size(round_trip);

    int fd;
    if (is_creator) {
        /* Clear stale names from a crashed run, then create exclusively. */
        shm_unlink(s->shm_name);
        sem_unlink(s->sem_empty_name);
        sem_unlink(s->sem_full_name);
        sem_unlink(s->sem_reply_empty_name);
        sem_unlink(s->sem_reply_full_name);

        fd = shm_open(s->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) { perror("fss_shm: shm_open(create)"); return -1; }
        if (ftruncate(fd, (off_t)s->map_size) < 0) {
            perror("fss_shm: ftruncate");
            close(fd); shm_unlink(s->shm_name); return -1;
        }
    } else {
        /* Map only the fixed header first.  Its mode and layout_size must agree
         * with this caller before any mode-specific ring or meta-table address
         * is calculated.  In particular, mapping an RR segment as one-way would
         * otherwise locate the meta table in the reply ring. */
        fd = shm_open(s->shm_name, O_RDWR, 0600);
        if (fd < 0) return -1;          /* creator not ready — caller retries */
        struct stat st;
        if (fstat(fd, &st) < 0 || st.st_size < 0 ||
            (uintmax_t)st.st_size < sizeof(shm_header_t)) {
            close(fd); return -1;       /* not sized yet — caller retries */
        }
        s->map = mmap(NULL, sizeof(shm_header_t), PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
        if (s->map == MAP_FAILED) {
            perror("fss_shm: mmap(header)");
            s->map = NULL;
            close(fd);
            return -1;
        }
        s->hdr = (shm_header_t *)s->map;

        uint32_t magic = 0;
        for (int i = 0; i < 2000; i++) {
            magic = atomic_load_explicit((_Atomic uint32_t *)&s->hdr->magic,
                                         memory_order_acquire);
            if (magic == FSS_SHM_MAGIC) break;
            struct timespec ts = { 0, 1000000L };
            nanosleep(&ts, NULL);
        }
        /* Do this before calculating reply_slots or metas.  `mode` mirrors the
         * channel() ABI argument and selects the physical layout, not carrier
         * mutability: attach() has no carrier metadata from which it could infer
         * mutable_by or any other carrier property. */
        uint32_t header_version = atomic_load_explicit(
            (_Atomic uint32_t *)&s->hdr->version, memory_order_relaxed);
        uint32_t header_ring_size = atomic_load_explicit(
            (_Atomic uint32_t *)&s->hdr->ring_size, memory_order_relaxed);
        uint32_t header_mode = atomic_load_explicit(
            (_Atomic uint32_t *)&s->hdr->mode, memory_order_relaxed);
        uint32_t header_layout = atomic_load_explicit(
            (_Atomic uint32_t *)&s->hdr->layout_size, memory_order_relaxed);
        /* The backing object may have trailing bytes (for example, from a
         * provisioner that rounds allocations up).  It must contain the whole
         * locally calculated layout, but mmap only that layout.  Header fields
         * remain exact ABI checks, including layout_size. */
        if (!shm_header_is_compatible(s->hdr, round_trip, s->map_size) ||
            fstat(fd, &st) < 0 || st.st_size < 0 ||
            (uintmax_t)st.st_size < (uintmax_t)s->map_size) {
            fprintf(stderr, "fss_shm: attach: incompatible header "
                    "(magic=0x%x ver=%u ring=%u mode=%u layout=%u "
                    "expected-layout=%zu file-size=%lld)\n",
                    magic, header_version, header_ring_size, header_mode,
                    header_layout, s->map_size, (long long)st.st_size);
            munmap(s->map, sizeof(shm_header_t));
            s->map = NULL;
            s->hdr = NULL;
            close(fd);
            return -1;
        }
        munmap(s->map, sizeof(shm_header_t));
        s->map = NULL;
    }

    s->map = mmap(NULL, s->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s->map == MAP_FAILED) {
        perror("fss_shm: mmap");
        s->map = NULL;
        if (is_creator) shm_unlink(s->shm_name);
        return -1;
    }

    s->hdr   = (shm_header_t *)s->map;
    s->slots = (slot_t *)((uint8_t *)s->map + sizeof(shm_header_t));
    /* Reply ring (round_trip): immediately after the request ring. */
    s->reply_slots = round_trip ? &s->slots[FSS_SHM_RING_SIZE] : NULL;
    /* Published-meta table: immediately after the ring(s). */
    s->metas = (meta_entry_t *)&s->slots[n_rings * FSS_SHM_RING_SIZE];

    if (is_creator) {
        /* Initialise the header (magic published LAST, below) and create the
         * sems BEFORE publishing magic, so any attacher that observes magic is
         * guaranteed the sems already exist. */
        memset(s->hdr, 0, sizeof(*s->hdr));
        /* Set every fixed and live header member explicitly.  This makes the
         * initial ABI state independent of padding bytes and documents that the
         * reply counters exist (and start at zero) in both layouts. */
        s->hdr->magic       = 0;
        s->hdr->version     = FSS_SHM_VERSION;
        s->hdr->ring_size   = FSS_SHM_RING_SIZE;
        s->hdr->meta_count  = 0;
        s->hdr->head        = 0;
        s->hdr->tail        = 0;
        s->hdr->reply_head  = 0;
        s->hdr->reply_tail  = 0;
        s->hdr->mode        = round_trip ? FSS_SHM_MODE_ROUND_TRIP
                                         : FSS_SHM_MODE_ONE_WAY;
        s->hdr->layout_size = (uint32_t)s->map_size;
        memset(s->hdr->_pad2, 0, sizeof(s->hdr->_pad2));

        s->sem_empty = sem_open(s->sem_empty_name, O_CREAT | O_EXCL, 0600, FSS_SHM_RING_SIZE);
        s->sem_full  = sem_open(s->sem_full_name,  O_CREAT | O_EXCL, 0600, 0);
        if (round_trip) {
            s->sem_reply_empty = sem_open(s->sem_reply_empty_name, O_CREAT | O_EXCL, 0600, FSS_SHM_RING_SIZE);
            s->sem_reply_full  = sem_open(s->sem_reply_full_name,  O_CREAT | O_EXCL, 0600, 0);
        }
        if (s->sem_empty != SEM_FAILED && s->sem_full != SEM_FAILED
            && (!round_trip || (s->sem_reply_empty != SEM_FAILED
                                && s->sem_reply_full != SEM_FAILED))) {
            /* Publish: release-store magic after sems + header are in place. */
            atomic_store_explicit((_Atomic uint32_t *)&s->hdr->magic,
                                  FSS_SHM_MAGIC, memory_order_release);
        }
    } else {
        /* The fixed header was validated before this full mapping.  Magic also
         * implies the semaphores exist, because the creator publishes it last. */
        s->sem_empty = sem_open(s->sem_empty_name, 0);
        s->sem_full  = sem_open(s->sem_full_name,  0);
        if (round_trip) {
            s->sem_reply_empty = sem_open(s->sem_reply_empty_name, 0);
            s->sem_reply_full  = sem_open(s->sem_reply_full_name,  0);
        }
    }
    if (s->sem_empty == SEM_FAILED || s->sem_full == SEM_FAILED
        || (round_trip && (s->sem_reply_empty == SEM_FAILED
                           || s->sem_reply_full == SEM_FAILED))) {
        perror("fss_shm: sem_open");
        if (s->sem_empty != SEM_FAILED) sem_close(s->sem_empty);
        if (s->sem_full  != SEM_FAILED) sem_close(s->sem_full);
        if (s->sem_reply_empty && s->sem_reply_empty != SEM_FAILED) sem_close(s->sem_reply_empty);
        if (s->sem_reply_full  && s->sem_reply_full  != SEM_FAILED) sem_close(s->sem_reply_full);
        s->sem_empty = s->sem_full = NULL;
        s->sem_reply_empty = s->sem_reply_full = NULL;
        munmap(s->map, s->map_size); s->map = NULL;
        if (is_creator) { shm_unlink(s->shm_name);
                          sem_unlink(s->sem_empty_name); sem_unlink(s->sem_full_name);
                          sem_unlink(s->sem_reply_empty_name); sem_unlink(s->sem_reply_full_name); }
        return -1;
    }

    /* CRASH-SAFE NAME RECLAMATION: the CREATOR owns the names.  Under the R11
     * orchestrator-provisioned model a channel has ONE creator (the orchestrator)
     * and TWO attachers (the initiator node and the target node), so an attacher
     * must NOT unlink the names — a second attacher still needs to shm_open /
     * sem_open them by name.  The creator reclaims: it shm_unlink/sem_unlink
     * DEFENSIVELY before create (clearing a crashed run's stale names, above) and
     * again at destroy.  A SIGKILL of the (long-lived) orchestrator therefore
     * leaks names only until the next run's defensive pre-create unlink clears
     * them — bounded, not unbounded.  Attachers close their fds on exit; the
     * kernel objects live by the creator's fds until it unlinks. */

    /* The reader thread is NOT spawned here.  In v0.1 the channel is
     * one-directional SPSC: only the RECEIVE side (the one that registers a
     * sink via fss_shm_set_sink) consumes sem_full and delivers.  Spawning a
     * reader on the sender too would let it steal sem_full posts and silently
     * drop frames.  So the reader starts lazily when a sink is installed. */
    return 0;
}

/* ── sink install (shared by fss_shm_set_sink) ────────────────────────────── */

/* Register the receive delivery target and start the sole reader.  The first
 * successful install owns the sink for this transport lifetime.  Repeating it
 * with that same sink is idempotent; a different sink is rejected rather than
 * changing a live reader's destination. */
static int shm_install_sink(shm_state *s, fss_endpoint_t *sink)
{
    if (!sink || !s->map) return FSS_SEND_UNBOUND;

    pthread_mutex_lock(&s->sink_lock);
    if (atomic_load_explicit(&s->stop, memory_order_acquire)) {
        pthread_mutex_unlock(&s->sink_lock);
        return FSS_SEND_UNBOUND;
    }
    if (s->sink) {
        int rc = s->sink == sink ? FSS_SEND_OK : FSS_SEND_UNBOUND;
        pthread_mutex_unlock(&s->sink_lock);
        return rc;
    }

    s->sink = sink;
    if (!s->reader_started) {
        if (pthread_create(&s->reader, NULL, shm_reader, s) == 0)
            s->reader_started = 1;
        else {
            perror("fss_shm: pthread_create");
            /* Do not leave a claimed sink if thread creation failed: a later
             * install can retry and no reader can observe this pointer. */
            s->sink = NULL;
            pthread_mutex_unlock(&s->sink_lock);
            return FSS_SEND_UNBOUND;
        }
    }
    pthread_mutex_unlock(&s->sink_lock);
    return FSS_SEND_OK;
}

/* ── vtable ───────────────────────────────────────────────────────────────── */

/* Stop + join the reader (idempotent): after this no reader thread can be
 * dispatching into a sink, so a node's endpoints may be freed safely.
 * Called from destroy. */
static void shm_stop_reader(shm_state *s)
{
    pthread_mutex_lock(&s->sink_lock);
    if (!s->reader_started) {
        pthread_mutex_unlock(&s->sink_lock);
        return;
    }
    pthread_join(s->reader, NULL);
    s->reader_started = 0;
    pthread_mutex_unlock(&s->sink_lock);
}

static void shm_post_waiters(sem_t *sem, unsigned count)
{
    while (sem && count--)
        (void)sem_post(sem);
}

/* channel(name, round_trip, owner, hints, n): provision the shm medium for a
 * named channel (transport_abi.h).  `owner` = 1 creates the segment + sems
 * (O_CREAT|O_EXCL after a defensive unlink); `owner` = 0 attaches to an existing
 * one, returning NULL if it is not up yet (the caller retries).  `round_trip`
 * adds the reply ring for synchronous write-back.  In v0.1 the transport hosts a
 * single channel, so its handle IS the transport state (cast).  `carrier_hints`
 * is accepted and (v0.1) ignored — the indicative check lands in R4. */
static fss_channel_t *shm_channel_vt(void *self, const char *name, int round_trip,
                                     int owner, const uint64_t *carrier_hints, size_t n)
{
    shm_state *s = (shm_state *)self;
    (void)carrier_hints; (void)n;   /* indicative hints: R4 */
    if (!name || !*name) {
        fprintf(stderr, "fss_shm: empty channel name\n");
        return NULL;
    }
    if (s->map) {
        if (s->round_trip != !!round_trip) {
            fprintf(stderr, "fss_shm: channel mode differs from existing layout\n");
            return NULL;
        }
        return (fss_channel_t *)s;   /* already provisioned (idempotent) */
    }
    if (shm_setup(s, name, owner, round_trip) != 0)
        return NULL;   /* owner not up yet (attacher) — caller retries */
    return (fss_channel_t *)s;
}

/* attach(ch, local, role): the single vtable connect verb (transport_abi.h).
 *   FSS_ROLE_INITIATOR — install the send fn.  A round-trip channel gets the
 *     synchronous request/reply fn (status + write-back); a one-way channel gets
 *     the fire-and-forget slot writer.
 *   FSS_ROLE_TARGET — install `local` as the receive sink (and start the reader
 *     thread). */
static int shm_attach_vt(void *self, fss_channel_t *ch,
                         fss_endpoint_t *local, fss_role role)
{
    shm_state *s = (shm_state *)self;
    (void)ch;   /* v0.1: one channel per transport, ch == s */
    if (!local) return FSS_SEND_UNBOUND;
    if (!s->map) return FSS_SEND_UNBOUND;
    if (role == FSS_ROLE_TARGET) {
        return shm_install_sink(s, local);
    }
    local->send     = s->round_trip ? shm_request_reply : shm_send_fn;
    local->send_ctx = s;
    return FSS_SEND_OK;
}

static int shm_send_to_vt(void *self, fss_channel_t *ch, fss_carrier_t *carrier)
{
    shm_state *s = (shm_state *)self;
    (void)ch;   /* v0.1: one channel per transport, ch == s */
    if (!s->map) return FSS_SEND_UNBOUND;
    return s->round_trip ? shm_request_reply(s, carrier) : shm_send_fn(s, carrier);
}

/* ── public extra symbol ──────────────────────────────────────────────────── */

FSS_EXPORT void fss_shm_set_sink(fss_transport_t *transport, fss_endpoint_t *sink)
{
    if (!transport) return;
    (void)shm_install_sink((shm_state *)transport->self, sink);
}

/* ── factory ──────────────────────────────────────────────────────────────── */

/* create(config) allocates the transport and installs the vtable; the shm
 * medium itself is provisioned lazily by channel(name, round_trip, owner) —
 * role (owner vs attacher) and mode (round-trip vs one-way) are channel()
 * parameters now, not a "create[-rr]:"/"attach[-rr]:" config dialect.  `config`
 * is unused (reserved for future session-level settings) and may be NULL. */
FSS_EXPORT const char *fss_transport_name(void) { return "shm"; }
FSS_EXPORT uint32_t fss_transport_abi_version(void) { return FSS_TRANSPORT_ABI_VERSION; }
FSS_EXPORT uint64_t fss_transport_capabilities(void) { return FSS_TRANSPORT_CAP_CHANNEL_ABI; }

FSS_EXPORT fss_transport_t *fss_transport_create(const char *config)
{
    (void)config;   /* medium is provisioned per-channel via channel() */

    shm_state *s = (shm_state *)calloc(1, sizeof(shm_state));
    if (!s) return NULL;
    atomic_init(&s->stop, 0);
    if (pthread_mutex_init(&s->sink_lock, NULL) != 0) {
        free(s);
        return NULL;
    }
    if (pthread_mutex_init(&s->close_lock, NULL) != 0) {
        pthread_mutex_destroy(&s->sink_lock);
        free(s);
        return NULL;
    }
    if (pthread_mutex_init(&s->sender_lock, NULL) != 0) {
        pthread_mutex_destroy(&s->close_lock);
        pthread_mutex_destroy(&s->sink_lock);
        free(s);
        return NULL;
    }
    if (pthread_cond_init(&s->close_cond, NULL) != 0) {
        pthread_mutex_destroy(&s->sender_lock);
        pthread_mutex_destroy(&s->close_lock);
        pthread_mutex_destroy(&s->sink_lock);
        free(s);
        return NULL;
    }

    s->transport.self         = s;
    s->transport.channel      = shm_channel_vt;
    s->transport.attach       = shm_attach_vt;
    s->transport.send_to      = shm_send_to_vt;
    return &s->transport;
}

FSS_EXPORT void fss_transport_destroy(fss_transport_t *transport)
{
    if (!transport) return;
    shm_state *s = (shm_state *)transport->self;

    /* Set stop and snapshot waiters in the same critical section that registers
     * a semaphore wait.  A close post is not permission to use a ring slot:
     * shm_wait checks stop again after it wakes. */
    pthread_mutex_lock(&s->close_lock);
    atomic_store_explicit(&s->stop, 1, memory_order_release);
    unsigned wait_empty = s->wait_empty;
    unsigned wait_full = s->wait_full;
    unsigned wait_reply_empty = s->wait_reply_empty;
    unsigned wait_reply_full = s->wait_reply_full;
    pthread_mutex_unlock(&s->close_lock);

    shm_post_waiters(s->sem_empty, wait_empty);
    shm_post_waiters(s->sem_full, wait_full);
    shm_post_waiters(s->sem_reply_empty, wait_reply_empty);
    shm_post_waiters(s->sem_reply_full, wait_reply_full);

    /* A sender owns an active count until it has left the producer lock and all
     * semaphore waits.  This also covers callers blocked on sender_lock. */
    pthread_mutex_lock(&s->close_lock);
    while (s->active_senders)
        pthread_cond_wait(&s->close_cond, &s->close_lock);
    pthread_mutex_unlock(&s->close_lock);

    /* The reader may own mapped slots.  It has also been woken above. */
    shm_stop_reader(s);

    if (s->sem_empty) sem_close(s->sem_empty);
    if (s->sem_full)  sem_close(s->sem_full);
    if (s->sem_reply_empty) sem_close(s->sem_reply_empty);
    if (s->sem_reply_full)  sem_close(s->sem_reply_full);
    if (s->map)       munmap(s->map, s->map_size);

    /* Creator owns the names (design §5: two-process SPSC). */
    if (s->is_creator) {
        shm_unlink(s->shm_name);
        sem_unlink(s->sem_empty_name);
        sem_unlink(s->sem_full_name);
        sem_unlink(s->sem_reply_empty_name);
        sem_unlink(s->sem_reply_full_name);
    }
    pthread_mutex_destroy(&s->sink_lock);
    pthread_cond_destroy(&s->close_cond);
    pthread_mutex_destroy(&s->sender_lock);
    pthread_mutex_destroy(&s->close_lock);
    free(s);
}
