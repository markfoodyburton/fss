/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_fmi_ctrl_queued.c — TIME-SYNCED QUEUED FMI CAN controller (relay).
 *
 * Wraps the can_controller FMU.  Acts as a RELAY with TWO timing peers:
 *   peer 0 ("fss/time")     — upstream initiator
 *   peer 1 ("fss/time/dev") — downstream device
 *
 * N-PEER CLOCK: arrays of peer_last_from/peer_upper/peer_seen indexed by [idx].
 * recompute_granted = MIN over peers that have sent any window (open → MAX drops
 * out).  Each fss/time port has a per-peer context (time_peer_ctx) so time_receive
 * knows which peer sent the window.
 *
 * FIFO (CAP=4): tx_receive queues transmit requests (producer); clock thread drains
 * one per DRAIN_EVERY quanta (consumer).  The mutex guards both the queue and clock
 * state — tx_receive NEVER touches the FMU (clock thread is the sole FMU driver).
 *
 * tx_receive read-poll (Operation==0): the SC initiator READ-polls the FIFO free
 * space before writing; reply with u32 free in the data buffer, Status=Ok — no
 * enqueue, no FMU touch.  (The FMI paced initiator has no in-band poll; this path
 * exists for SC/Rust/Python initiators.)
 *
 * Clock thread (sole FMU driver): each quantum, advance now, skew; every
 * DRAIN_EVERY quanta pop one queued request, set Rx_Clock/Rx_Data, advance the
 * FMU by FMU_STEP_SIZE (the FMU is event-driven like the device; a small dt may
 * not fire Tx — use the full FMU_STEP_SIZE for the drain step), harvest Tx, emit
 * fmican::frame on "can_tx".  Send windows to BOTH peers each quantum.
 *
 * stop_on_detach: any peer open_window sets ts_stop → clock sends open_window to
 * both peers (cascades upstream + downstream) and reports skew.
 *
 * R12: this controller accepts ANY framework's hop1 register-write — tx_receive
 * META-RESOLVES bus::Operation/Locator/Length/Data by qname from the incoming
 * carrier's own meta (no id gate), so the SystemC (tlm::transaction, Data by
 * pointer), FMI (fmireg::write), Rust (rustreg::write) or Python (pyreg::write,
 * one-way) initiator all interoperate.  Its OUTBOUND hop2 is FMI's OWN native
 * carrier fmican::frame: the FMU's fmi3LsBusCanOperationCanTransmit shipped
 * VERBATIM (no repack), its fields described in place by the meta.
 *
 *   "tx"            inbound register-write from ANY initiator (meta-resolved)
 *   "can_tx"        outbound fmican::frame (FMI's own hop2 carrier)
 *   "fss/time"      timing peer 0 (upstream initiator)
 *   "fss/time/dev"  timing peer 1 (downstream device)
 *   "fss/sim"       upward readiness wave; starts clock when gate opens
 */
#include "fmi.h"
#include "fmi3LsBusUtil.h"
#include "fmi3LsBusUtilCan.h"

#include "fss/artefact_abi.h"
#include "fss/carrier.h"
#include "fss/endpoint.h"
#include "fss/transport_abi.h"
#include "fss/known_carriers/fmican_frame.h"  /* FSS_FMICAN_ID / FSS_FMICAN_META (FMI's own hop2 carrier) */
#include "fss/c/meta.h"
#include "fss/protocols/sim.h"      /* FSS_SIM_ID/META          */
#include "fss/protocols/time_sync.h"/* FSS_TIME_ID/META         */

#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FSS_FMU_PATH
#  error "FSS_FMU_PATH must be set by CMake (target_compile_definitions)"
#endif

/* VR constants — can_controller FMU. */
#define VR_RX_DATA   1u
#define VR_RX_CLOCK  2u
#define VR_TX_DATA   3u
#define VR_TX_CLOCK  4u

/* N-peer clock and FIFO sizing (shared time-sync constants in common/fmi_timesync.h). */
#define MAX_SIM_DOWN    4               /* max inbound fss/sim/<tag> sinks (array dim) */
#define MAX_TIME_PEERS  4
#define FMU_STEP_SIZE   0.001               /* FMU event-step (1 ms) */
#define CAP             4                   /* FIFO capacity          */
#define DRAIN_EVERY     10                  /* drain one frame per 10 quanta */
#define OP_BUF_SIZE     256

/* fmican::frame is the FMU's fmi3LsBusCanOperationCanTransmit shipped verbatim;
 * the meta (fmican_frame.jsonnet) maps bus:: fields onto the op's real offsets, so
 * this controller needs no layout macros of its own. */

/* Per-peer context so time_receive knows which peer sent the window. */
typedef struct { void *node; int idx; } time_peer_ctx;

/* One queued transmit request. */
typedef struct { uint32_t id; uint16_t dlc; uint8_t payload[8]; } queued_tx_t;

typedef struct {
    fss_fmi_t       *fmi;
    fss_endpoint_t   tx;           /* inbound TLM register-write (round-trip) */
    fss_endpoint_t   can_tx;       /* outbound can::frame                     */
    fss_endpoint_t   time_eps[MAX_TIME_PEERS];
    time_peer_ctx    time_ctx[MAX_TIME_PEERS];
    int              n_time;       /* 2 for this artefact                     */
    fss_endpoint_t   sim;          /* fss/sim UP-report port (Elaborated sent here) */
    /* fss/sim/<tag> — one inbound sink per DOWNSTREAM peer; n_sim_down IS this
     * node's n_downstream.  Decoupled from the (symmetric) time peers. */
    fss_endpoint_t   sim_down[MAX_SIM_DOWN];
    char             sim_down_names[MAX_SIM_DOWN][32];
    int              n_sim_down;
    char             name[32];

    /* Inbound hop1 carriers are meta-resolved PER CARRIER by qname (any framework's
     * register-write is accepted); the cache keys results by (id,qname). */
    int              ready;
    fss_meta_cache_t *cache;

    /* FIFO (producer: tx_receive; consumer: clock thread) */
    queued_tx_t      q[CAP];
    int              q_head, q_tail, q_len;

    /* windowed N-peer clock */
    uint64_t         now_ps, granted_upper_ps, max_skew_ps;
    uint64_t         peer_last_from[MAX_TIME_PEERS];
    uint64_t         peer_upper[MAX_TIME_PEERS];
    int              peer_seen[MAX_TIME_PEERS];
    double           sim_time;     /* FMU time accumulator */
    int              ts_stop, ts_running, fmi_failed;
    /* upward readiness wave */
    int              wired, downstream_ready, n_downstream;
    int              chain_classified;
    pthread_t        clock_thread;
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    pthread_mutex_t  meta_mu;
} node_t;

static void *clock_thread(void *arg);   /* forward — defined after helpers */

#include "../common/fmi_timesync.h"  /* TIME_*, SIM_* constants; ts_to_ps, send_sim_up, try_start */

/* Resolve (byte_offset[,byte_size]) of `qname` from carrier c's own meta.  Returns
 * 1 on success, 0 if the meta is absent (the carrier contract requires it present
 * at the artefact), foreign, or malformed.  id is only the cache key, never a gate. */
static int node_field(node_t *n, const fss_carrier_t *c, const char *qname,
                      uint32_t *off, uint32_t *sz)
{
    if (!c->meta_ptr || !c->meta_size) return 0;   /* meta MUST be present */
    pthread_mutex_lock(&n->meta_mu);
    int ok = fss_meta_resolve(n->cache, c->id, c->meta_ptr, c->meta_size,
                              qname, off, sz);
    pthread_mutex_unlock(&n->meta_mu);
    return ok;
}

/* granted = min over ALL configured peers (SC "anding": every peer must grant).
 * seen → its Upper; detached (Upper=MAX) → drops out; UNSEEN (not yet joined) →
 * the bootstrap ceiling (one quantum) so we emit our first window but never
 * advance past it until the peer joins — stops the federation lapping a late
 * wall-clock joiner (cross-process initiator post connect/settle).  Under mu. */
static void recompute_granted(node_t *n)
{
    if (n->n_time <= 0) { n->granted_upper_ps = n->now_ps + TIME_QUANTUM_PS; return; }
    uint64_t lo = UINT64_MAX;
    for (int i = 0; i < n->n_time; ++i) {
        uint64_t cap;
        if (n->peer_seen[i])                     cap = n->peer_upper[i];
        else if (n->peer_upper[i] == UINT64_MAX) cap = UINT64_MAX;
        else                                     cap = TIME_QUANTUM_PS;
        if (cap < lo) lo = cap;
    }
    n->granted_upper_ps = lo;
}

/* Inbound on fss/time[idx]: fold peer's window; any detach stops the relay. */
static int time_receive(void *self, fss_carrier_t *c)
{
    time_peer_ctx *e = (time_peer_ctx *)self;
    node_t *n = (node_t *)e->node;
    int idx   = e->idx;
    if (!c || !c->data_ptr) return FSS_SEND_NO_RECEIVER;
    /* Walk the carrier's own meta for the window fields (no id gate, no hard-coded
     * offsets); a foreign/meta-less window simply fails to resolve and is dropped. */
    uint32_t uo, lo_off, up_off, usz, lsz, upsz;
    if (!node_field(n, c, "time::Unit",  &uo,     &usz)  || usz  < 4 ||
        !node_field(n, c, "time::Lower", &lo_off, &lsz)  || lsz  < 8 ||
        !node_field(n, c, "time::Upper", &up_off, &upsz) || upsz < 8 ||
        c->data_size < (uint64_t)up_off + 8)
        return FSS_SEND_NO_RECEIVER;
    const uint8_t *d = (const uint8_t *)c->data_ptr;
    int32_t  unit;  memcpy(&unit,  d + uo,     4);
    uint64_t lower; memcpy(&lower, d + lo_off, 8);
    uint64_t upper; memcpy(&upper, d + up_off, 8);
    pthread_mutex_lock(&n->mu);
    if (upper != TIME_OPEN_UPPER) {
        n->peer_seen[idx]      = 1;
        n->peer_last_from[idx] = ts_to_ps(unit, lower);
        n->peer_upper[idx]     = ts_to_ps(unit, upper);
    } else {
        n->peer_upper[idx] = UINT64_MAX;   /* detached */
        n->ts_stop         = 1;            /* any peer detach stops the relay */
    }
    recompute_granted(n);
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    return FSS_SEND_OK;
}

/* Send a time::window to peer idx. */
static int ts_send(node_t *n, int idx, uint64_t lower_ps, uint64_t upper)
{
    fss_endpoint_t *ep = &n->time_eps[idx];
    if (!ep->send) return FSS_SEND_OK;
    uint8_t buf[TW_SIZE];
    int32_t unit = TIME_UNIT_PS; uint32_t pad = 0;
    memcpy(buf + 0,  &unit,     4); memcpy(buf + 4,  &pad,      4);
    memcpy(buf + 8,  &lower_ps, 8); memcpy(buf + 16, &upper,    8);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_TIME_ID; c.data_ptr = buf; c.data_size = TW_SIZE;
    c.meta_ptr = (void *)FSS_TIME_META; c.meta_size = FSS_TIME_META_LEN;
    return ep->send(ep->send_ctx, &c);
}

/* Is the carrier's bus::Data carried INLINE (layout "") rather than by pointer
 * (fss:local-ptr — the live TLM GP form)?  Read from the meta so tx_receive
 * aliases the payload the right way regardless of which framework sent it. */
static int data_is_inline(const void *meta, uint64_t meta_size)
{
    int cnt = fss_meta_field_count(meta, meta_size);
    for (int i = 0; i < cnt; ++i) {
        fss_meta_field_t f;
        if (!fss_meta_field_at(meta, meta_size, i, &f)) break;
        if (strcmp(f.qname, "bus::Data") == 0) return f.layout[0] == '\0';
    }
    return 0;   /* default: pointer (the TLM live form) */
}

/* Inbound on "tx": a register-write from ANY framework's initiator.  We do NOT
 * gate on carrier id — we META-RESOLVE bus::Operation/Locator/Length/Data (and
 * optional bus::Status for the round-trip reply) by qname from the carrier's OWN
 * meta, so SystemC/FMI/Rust/Python initiators all interoperate with no bridge. */
static int tx_receive(void *self, fss_carrier_t *c)
{
    node_t *n = (node_t *)self;
    if (!n->ready || !c || !c->data_ptr || !c->meta_ptr) return FSS_SEND_NO_RECEIVER;
    pthread_mutex_lock(&n->mu);
    int fmi_failed = n->fmi_failed;
    pthread_mutex_unlock(&n->mu);
    if (fmi_failed) return FSS_SEND_NO_RECEIVER;

    uint32_t o_op, o_loc, o_len, o_data, o_stat, sz_op, sz_loc, sz_len, sz;
    if (!node_field(n, c, "bus::Operation", &o_op,   &sz_op)  ||
        !node_field(n, c, "bus::Locator",   &o_loc,  &sz_loc) ||
        !node_field(n, c, "bus::Length",    &o_len,  &sz_len) ||
        !node_field(n, c, "bus::Data",      &o_data, &sz))
        return FSS_SEND_NO_RECEIVER;   /* required field absent — reject */
    int have_status = node_field(n, c, "bus::Status", &o_stat, &sz);
    int inline_data = data_is_inline(c->meta_ptr, c->meta_size);

    uint8_t *region = (uint8_t *)c->data_ptr;
    /* Operation width varies (u8 for fmican-style, u32 for reg carriers): read the
     * resolved width into a zeroed u32 so any width up to 4 bytes reads correctly. */
    uint32_t op = 0; memcpy(&op, region + o_op, sz_op < 4 ? sz_op : 4);

    if (op == 0) {
        /* READ poll (SC initiator probing free space): report free slots. */
        pthread_mutex_lock(&n->mu);
        uint32_t free_slots = (uint32_t)(CAP - n->q_len);
        pthread_mutex_unlock(&n->mu);
        uint8_t *data_ptr = NULL;
        if (inline_data) data_ptr = region + o_data;
        else             memcpy(&data_ptr, region + o_data, sizeof(data_ptr));
        if (data_ptr) memcpy(data_ptr, &free_slots, sizeof(free_slots));
        if (have_status) {
            int32_t ok = 1;
            memcpy(region + o_stat, &ok, sizeof(ok));
        }
        return FSS_SEND_OK;
    }

    /* WRITE: enqueue the request.  Locator/Length widths vary by carrier (u32 or
     * u64) — read the resolved width into zeroed scratch so both work. */
    uint64_t locator = 0, len = 0; const uint8_t *payload = NULL;
    memcpy(&locator, region + o_loc, sz_loc < 8 ? sz_loc : 8);
    memcpy(&len,     region + o_len, sz_len < 8 ? sz_len : 8);
    if (inline_data) payload = region + o_data;
    else             memcpy(&payload, region + o_data, sizeof(payload));
    uint16_t dlc = len <= 8 ? (uint16_t)len : 8;

    pthread_mutex_lock(&n->mu);
    if (n->q_len >= CAP) {
        /* Queue full: signal not-Ok so the producer backs off. */
        pthread_mutex_unlock(&n->mu);
        if (have_status) {
            int32_t notok = 0;
            memcpy(region + o_stat, &notok, sizeof(notok));
        }
        return FSS_SEND_OK;
    }
    queued_tx_t *slot = &n->q[n->q_tail];
    slot->id  = (uint32_t)locator;
    slot->dlc = dlc;
    memset(slot->payload, 0, 8);
    if (payload && dlc) memcpy(slot->payload, payload, dlc);
    n->q_tail = (n->q_tail + 1) % CAP;
    n->q_len++;
    pthread_mutex_unlock(&n->mu);

    if (have_status) {
        int32_t ok = 1;
        memcpy(region + o_stat, &ok, sizeof(ok));
    }
    return FSS_SEND_OK;
}

/* Drain one queued request: feed FMU (enter_event + set Rx + advance by FMU_STEP),
 * harvest Tx, emit fmican::frame.  Called from the clock thread (sole FMU driver). */
/* Warning supplies usable FMI results.  Discard, termination, and early return
 * do not complete this fixed-time operation. */
static int fmi_step_completed(fss_fmi_step_result_t result)
{
    return (result.status == FSS_FMI_STATUS_OK ||
            result.status == FSS_FMI_STATUS_WARNING) &&
           !result.terminate_simulation && !result.early_return;
}

static int fmu_drain_one(node_t *n, const queued_tx_t *req)
{
    uint8_t op_buf[OP_BUF_SIZE];
    fmi3LsBusUtilBufferInfo info;
    FMI3_LS_BUS_BUFFER_INFO_INIT(&info, op_buf, OP_BUF_SIZE);
    FMI3_LS_BUS_CAN_CREATE_OP_CAN_TRANSMIT(&info, req->id, /*ide=*/0, /*rtr=*/0,
                                            req->dlc, req->payload);
    if (!info.status) return 0;
    size_t op_len = (size_t)FMI3_LS_BUS_BUFFER_LENGTH(&info);

    /* Use FMU_STEP_SIZE for the drain step (like the plain controller) — the FMU
     * is event-driven on Rx, and a tiny capped dt may not fire Tx. */
    double t = n->sim_time;
    if (fss_fmi_enter_event_status(n->fmi) > FSS_FMI_STATUS_WARNING ||
        fss_fmi_set_clock(n->fmi, VR_RX_CLOCK) != 0 ||
        fss_fmi_set_binary(n->fmi, VR_RX_DATA, op_buf, op_len) != 0)
        return 0;
    if (!fmi_step_completed(fss_fmi_advance_status(n->fmi, t, FMU_STEP_SIZE)))
        return 0;
    n->sim_time += FMU_STEP_SIZE;

    int tx_clock = 0;
    fss_fmi_status_t status = fss_fmi_get_clock_status(n->fmi, VR_TX_CLOCK, &tx_clock);
    if (status != FSS_FMI_STATUS_OK && status != FSS_FMI_STATUS_WARNING)
        return 0;
    if (!tx_clock) return 1;
    const void *txop = NULL; size_t txlen = 0;
    status = fss_fmi_get_binary_status(n->fmi, VR_TX_DATA, &txop, &txlen);
    if (status != FSS_FMI_STATUS_OK && status != FSS_FMI_STATUS_WARNING)
        return 0;
    if (!txop || txlen < sizeof(fmi3LsBusCanOperationCanTransmit)) return 1;
    /* Ship the FMI-LS-BUS operation VERBATIM as fmican::frame: no repack.  The
     * carrier's data region IS the packed fmi3LsBusCanOperationCanTransmit the FMU
     * produced; FSS_FMICAN_META describes where id/ide/rtr/dataLength/data sit
     * inside it (see fmican_frame.jsonnet), so any device reads it by bus:: qname.
     * The FMI analogue of the SystemC binding shipping the live generic payload. */
    fss_carrier_t out; memset(&out, 0, sizeof(out));
    out.id       = FSS_FMICAN_ID;
    out.data_ptr = (void *)txop; out.data_size = txlen;
    out.meta_ptr = (void *)FSS_FMICAN_META; out.meta_size = FSS_FMICAN_META_LEN;
    if (n->can_tx.send) n->can_tx.send(n->can_tx.send_ctx, &out);
    return 1;
}

/* Clock thread: advance now per quantum, drain queue every DRAIN_EVERY quanta,
 * idle-step FMU otherwise; send windows to both peers each quantum. */
static void *clock_thread(void *arg)
{
    node_t *n = (node_t *)arg;
    const uint64_t q = TIME_QUANTUM_PS;
    uint64_t quantum_count = 0;
    send_sim_up(n);   /* announce Elaborated UP before advancing time */
    for (;;) {
        uint64_t old_now, next; int have; queued_tx_t req;
        pthread_mutex_lock(&n->mu);
        /* Channels confirmed live: a plain wait suffices — a peer's window WILL
         * arrive and broadcast cv. */
        while (!n->ts_stop && n->granted_upper_ps <= n->now_ps)
            pthread_cond_wait(&n->cv, &n->mu);
        if (n->ts_stop) { pthread_mutex_unlock(&n->mu); break; }
        old_now = n->now_ps;
        next = old_now + q;
        if (next > n->granted_upper_ps) next = n->granted_upper_ps;
        n->now_ps = next;
        /* Compute skew over all active peers. */
        for (int i = 0; i < n->n_time; ++i) {
            if (n->peer_seen[i] && n->peer_upper[i] != UINT64_MAX) {
                uint64_t a = next, b = n->peer_last_from[i];
                uint64_t gap = a > b ? a - b : b - a;
                if (gap > n->max_skew_ps) n->max_skew_ps = gap;
            }
        }
        /* Pop one request if it's a drain quantum. */
        have = 0;
        quantum_count++;
        if (quantum_count % DRAIN_EVERY == 0 && n->q_len > 0) {
            req  = n->q[n->q_head];
            n->q_head = (n->q_head + 1) % CAP;
            n->q_len--;
            have = 1;
        }
        pthread_mutex_unlock(&n->mu);

        int fmu_ok;
        if (have) {
            fmu_ok = fmu_drain_one(n, &req);
        } else {
            /* Idle step: keep FMU time in sync (capped dt like the device). */
            double dt = (double)(next - old_now) / 1e12;
            fmu_ok = fmi_step_completed(fss_fmi_step_status(n->fmi, n->sim_time, dt));
            if (fmu_ok) n->sim_time += dt;
        }
        if (!fmu_ok) {
            /* The operation runs on the async clock.  Stop it and propagate the
             * FSS failure through the standard open-window teardown. */
            pthread_mutex_lock(&n->mu);
            n->fmi_failed = 1;
            n->ts_stop = 1;
            pthread_cond_broadcast(&n->cv);
            pthread_mutex_unlock(&n->mu);
            break;
        }

        /* Send window to both peers each quantum. */
        for (int i = 0; i < n->n_time; ++i) ts_send(n, i, next, next + q);
    }
    uint64_t skew; int any = 0;
    pthread_mutex_lock(&n->mu);
    skew = n->max_skew_ps;
    for (int i = 0; i < n->n_time; ++i) if (n->peer_seen[i]) any = 1;
    pthread_mutex_unlock(&n->mu);
    /* Detach: send open_window to both peers (cascade upstream + downstream). */
    for (int i = 0; i < n->n_time; ++i) ts_send(n, i, n->now_ps, TIME_OPEN_UPPER);
    if (any) {
        /* Diagnostic, NOT a verdict: bounded multi-quantum skew is real processing
         * latency, not a lockstep violation.  See fss-skew-bounded-transient. */
        printf("FSS TIMESYNC %s: max_clock_gap=%g ns\n", n->name, (double)skew / 1000.0);
        fflush(stdout);
    }
    return NULL;
}

/* fss/sim: upward readiness wave dispatcher.
 *   Bound|Started → classify chain + mark wired + try_start;
 *   Elaborated    → count downstream peer ("fss/time/dev") + try_start;
 *   Stopped       → existing stop. */
static int sim_receive(void *self, fss_carrier_t *c)
{
    node_t *n = (node_t *)self;
    if (!c || !c->data_ptr) return FSS_SEND_OK;
    uint32_t ec_off, ec_sz, code;
    if (!node_field(n, c, "sim::EventCode", &ec_off, &ec_sz) || ec_sz < 4 ||
        c->data_size < (uint64_t)ec_off + 4)
        return FSS_SEND_OK;
    memcpy(&code, (const char *)c->data_ptr + ec_off, 4);
    if (code == SIM_BOUND || code == SIM_STARTED) {
        if (!n->chain_classified) {
            /* The fss/sim/<tag> sinks (allocated by the orchestrator before Bound)
             * ARE the downstream peers whose Elaborated the gate awaits — for the
             * relay that is one ("fss/sim/dev").  Time peers are symmetric. */
            n->n_downstream = n->n_sim_down;
            n->chain_classified = 1;
        }
        n->wired = 1;
        try_start(n);
    } else if (code == SIM_ELABORATED) {
        ++n->downstream_ready;
        try_start(n);
    } else if (code == SIM_STOPPED) {
        pthread_mutex_lock(&n->mu);
        n->ts_stop = 1;
        pthread_cond_broadcast(&n->cv);
        pthread_mutex_unlock(&n->mu);
    }
    return FSS_SEND_OK;
}

FSS_EXPORT fss_node_t *fss_create(fss_transport_t *transport, const char *name)
{
    (void)transport;
    node_t *n = (node_t *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    strncpy(n->name, (name && *name) ? name : "fmi-ctrl", sizeof(n->name) - 1);
    n->fmi = fss_fmi_open(FSS_FMU_PATH);
    if (!n->fmi) { free(n); return NULL; }

    /* One cache for ALL inbound meta-resolve (hop1 register-writes from any
     * framework, plus time/sim windows).  No flatten: offsets are per-carrier. */
    n->cache = fss_meta_cache_create();
    if (!n->cache) { fss_fmi_close(n->fmi); free(n); return NULL; }
    n->ready = 1;

    n->granted_upper_ps = TIME_QUANTUM_PS;
    pthread_mutex_init(&n->mu, NULL);
    pthread_cond_init(&n->cv, NULL);
    pthread_mutex_init(&n->meta_mu, NULL);

    n->tx.self      = n; n->tx.receive     = tx_receive;
    /* can_tx: outbound, send installed at bind */
    n->sim.self     = n; n->sim.receive    = sim_receive;

    /* Wire the two timing-peer endpoints (peer 0 = upstream, peer 1 = downstream). */
    n->n_time = 2;
    for (int i = 0; i < n->n_time; ++i) {
        n->time_ctx[i].node = n;
        n->time_ctx[i].idx  = i;
        n->time_eps[i].self    = &n->time_ctx[i];
        n->time_eps[i].receive = time_receive;
    }
    return (fss_node_t *)n;
}

FSS_EXPORT fss_endpoint_t *fss_get_endpoint(fss_node_t *node, const char *endpoint_name)
{
    node_t *n = (node_t *)node;
    if (strcmp(endpoint_name, "tx") == 0)           return &n->tx;
    if (strcmp(endpoint_name, "can_tx") == 0)       return &n->can_tx;
    if (strcmp(endpoint_name, "fss/time") == 0)     return &n->time_eps[0];
    if (strcmp(endpoint_name, "fss/time/dev") == 0) return &n->time_eps[1];
    if (strcmp(endpoint_name, "fss/sim") == 0)      return &n->sim;
    /* "fss/sim/<tag>" — one inbound sink per DOWNSTREAM peer; n_sim_down IS
     * n_downstream.  Allocated on first request, inbound only (never sends). */
    if (strncmp(endpoint_name, "fss/sim/", 8) == 0) {
        for (int i = 0; i < n->n_sim_down; ++i)
            if (strcmp(n->sim_down_names[i], endpoint_name) == 0) return &n->sim_down[i];
        if (n->n_sim_down >= MAX_SIM_DOWN) return NULL;
        int i = n->n_sim_down++;
        strncpy(n->sim_down_names[i], endpoint_name, sizeof(n->sim_down_names[i]) - 1);
        n->sim_down[i].self    = n;            /* sim_receive recovers node_t from self */
        n->sim_down[i].receive = sim_receive;
        return &n->sim_down[i];
    }
    return NULL;
}

FSS_EXPORT void fss_destroy(fss_node_t *node)
{
    node_t *n = (node_t *)node;
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    n->ts_stop = 1; pthread_cond_broadcast(&n->cv);
    int running = n->ts_running;
    pthread_mutex_unlock(&n->mu);
    if (running) pthread_join(n->clock_thread, NULL);
    pthread_mutex_destroy(&n->mu);
    pthread_cond_destroy(&n->cv);
    pthread_mutex_destroy(&n->meta_mu);
    if (n->cache) fss_meta_cache_destroy(n->cache);
    fss_fmi_close(n->fmi);
    free(n);
}
