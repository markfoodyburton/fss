/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_fmi_rx_paced.c — a TIME-SYNCED FMI CAN device (wraps the can_target FMU).
 *
 * The FMI counterpart of fss_can_target_paced_{systemc,rust,python}: the FMI CAN
 * receiver plus a windowed invented clock so it is a first-class federation
 * timing peer.  FMI's distinctive way of HONOURING the time::window is the FMU
 * STEP CLAMP: the clock thread advances the FMU with fss_fmi_advance(t, dt) where
 * dt is capped so t+dt never exceeds the granted Upper — the FMU genuinely never
 * steps past the window.  (The clock protocol itself is native C here, per the
 * all-native-per-language choice — the analogue of the SC sc_sync_window.)
 *
 * Threading: the CLOCK THREAD is the sole FMU driver.  can_rx_receive (a foreign
 * transport thread) only QUEUES the frame + counts — it never touches the FMU —
 * so no FMU lock is needed; the mutex guards only the clock state + the queue.
 * Each quantum the clock pops one queued frame (event-mode delivery) or idle-
 * steps, always by the capped dt.  Being the SINK it drives teardown: at the
 * expected frame count it stops, sending an open_window (detach) to its peer,
 * cascading the federation stop.
 *
 *   "can_rx"   inbound — a CAN frame from ANY framework's controller.  We do NOT
 *              gate on carrier id: can_rx_receive META-RESOLVES bus::Locator/
 *              Length/Data by qname from the carrier's OWN meta (R12), so the
 *              SystemC (sccan::frame), FMI (fmican::frame), Rust (rustcan::frame)
 *              or Python (pycan::frame) controller all interoperate with no bridge.
 *   "fss/time" — the timing-peer port (the windowed clock).
 *   "fss/sim"  upward readiness wave; starts clock when gate opens (sink: on Bound)
 *
 * Prints "FMI CAN TARGET PASS: rx=16 last_id=0x100 last_b0=15" + the skew line.
 */
#include "fmi.h"
#include "fmi3LsBusUtil.h"
#include "fmi3LsBusUtilCan.h"

#include "fss/artefact_abi.h"
#include "fss/carrier.h"
#include "fss/endpoint.h"
#include "fss/transport_abi.h"
#include "fss/c/meta.h"                  /* fss_meta_resolve — the walk primitive */
#include "fss/protocols/config.h"   /* FSS_CONFIG_ID            */
#include "fss/protocols/sim.h"      /* FSS_SIM_ID/META          */
#include "fss/protocols/time_sync.h"/* FSS_TIME_ID/META         */
#include "fss/c/cfg_json.h"              /* fss_cfg_json_get_u64 */

#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FSS_FMU_PATH
#  error "FSS_FMU_PATH must be set by CMake (target_compile_definitions)"
#endif

/* can_target FMU value references (modelDescription.xml). */
#define VR_RX_DATA   1u
#define VR_RX_CLOCK  2u

/* Device sizing (shared time-sync constants are in common/fmi_timesync.h). */
#define MAX_SIM_DOWN    4               /* max inbound fss/sim/<tag> sinks (array dim) */
#define EXPECT_FRAMES 16
#define QUEUE_CAP     64
#define OP_BUF_SIZE   256

/* A frame normalised out of whatever carrier arrived (parsed by meta-resolve in
 * can_rx_receive), so the FMU-driving clock thread reads uniform fields. */
typedef struct { uint32_t id; uint16_t dlc; uint8_t payload[8]; } can_frame_t;

typedef struct {
    fss_fmi_t      *fmi;
    fss_endpoint_t  can_rx;        /* inbound can::frame (queues, no FMU touch)   */
    fss_endpoint_t  time_ep;       /* fss/time peer port                          */
    fss_endpoint_t  sim;           /* fss/sim UP-report port (Elaborated sent here)*/
    /* fss/sim/<tag> — one inbound sink per DOWNSTREAM peer; n_sim_down IS this
     * node's n_downstream.  Decoupled from the (symmetric) time peers. */
    fss_endpoint_t  sim_down[MAX_SIM_DOWN];
    char            sim_down_names[MAX_SIM_DOWN][32];
    int             n_sim_down;
    fss_endpoint_t  config_ep;     /* fss/config control port (expected count)    */
    char            name[32];

    /* counts / oracle (tracked directly from received frames) */
    uint32_t        rx, last_id;
    uint8_t         last_b0;
    uint64_t        expect_frames; /* frames to await before teardown (= "frames")*/

    /* received-frame queue (producer: can_rx_receive; consumer: clock thread) */
    can_frame_t     q[QUEUE_CAP];
    int             q_head, q_tail, q_len;

    /* windowed clock */
    uint64_t        now_ps, granted_upper_ps, max_skew_ps;
    uint64_t        peer_last_from, peer_upper;
    int             peer_seen;
    int             ts_stop, ts_running, fmi_failed;
    /* upward readiness wave */
    int             wired, downstream_ready, n_downstream;
    int             chain_classified;
    int             config_pulled;     /* one-shot guard for the wired-time config pull */
    pthread_t       clock_thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;

    /* carrier-meta walk: every control interface resolves field offsets from the
     * carrier's own meta (NOT hard-coded offsets); (id,qname) cache makes repeats
     * O(1), guarded by meta_mu (receivers run on several threads). */
    fss_meta_cache_t *meta_cache;
    pthread_mutex_t   meta_mu;
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
    int ok = fss_meta_resolve(n->meta_cache, c->id, c->meta_ptr, c->meta_size,
                              qname, off, sz);
    pthread_mutex_unlock(&n->meta_mu);
    return ok;
}

/* granted_upper = the peer's grant: seen → its Upper (open → MAX); UNSEEN (not
 * yet joined) → the bootstrap ceiling (one quantum) so we emit our first window
 * but never advance past it until the peer joins — stops a sink lapping a peer
 * that joins late in wall-clock (cross-process connect/settle).  Call under mu. */
static void recompute_granted(node_t *n)
{
    if (n->peer_seen)
        n->granted_upper_ps = n->peer_upper;             /* real, or MAX once detached */
    else if (n->peer_upper == UINT64_MAX)
        n->granted_upper_ps = UINT64_MAX;                /* detached unseen → run free */
    else
        n->granted_upper_ps = TIME_QUANTUM_PS;           /* unseen & live → hold */
}

/* Inbound on fss/time: fold the peer's window in. */
static int time_receive(void *self, fss_carrier_t *c)
{
    node_t *n = (node_t *)self;
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
        n->peer_seen      = 1;
        n->peer_last_from = ts_to_ps(unit, lower);
        n->peer_upper     = ts_to_ps(unit, upper);
    } else {
        n->peer_upper = UINT64_MAX;
    }
    recompute_granted(n);
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    return FSS_SEND_OK;
}

static int ts_send(node_t *n, uint64_t lower_ps, uint64_t upper)
{
    if (!n->time_ep.send) return FSS_SEND_OK;
    uint8_t buf[TW_SIZE];
    int32_t unit = TIME_UNIT_PS; uint32_t pad = 0;
    memcpy(buf + 0, &unit, 4); memcpy(buf + 4, &pad, 4);
    memcpy(buf + 8, &lower_ps, 8); memcpy(buf + 16, &upper, 8);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_TIME_ID; c.data_ptr = buf; c.data_size = TW_SIZE;
    c.meta_ptr = (void *)FSS_TIME_META; c.meta_size = FSS_TIME_META_LEN;
    return n->time_ep.send(n->time_ep.send_ctx, &c);
}

/* Deliver one queued frame to the FMU and step by dt (event mode). */
/* The binding accepts OK and Warning only.  A discard has no completed step, and
 * early return cannot be reconciled with this artefact's fixed clock quantum. */
static int fmi_step_completed(fss_fmi_step_result_t result)
{
    return (result.status == FSS_FMI_STATUS_OK ||
            result.status == FSS_FMI_STATUS_WARNING) &&
           !result.terminate_simulation && !result.early_return;
}

static int fmu_deliver_and_step(node_t *n, const can_frame_t *f, double t, double dt)
{
    uint32_t id  = f->id;
    uint16_t dlc = f->dlc > 8 ? 8 : f->dlc;
    uint8_t op_buf[OP_BUF_SIZE];
    fmi3LsBusUtilBufferInfo info;
    FMI3_LS_BUS_BUFFER_INFO_INIT(&info, op_buf, OP_BUF_SIZE);
    FMI3_LS_BUS_CAN_CREATE_OP_CAN_TRANSMIT(&info, id, /*ide=*/0, /*rtr=*/0, dlc, f->payload);
    if (!info.status)
        return fmi_step_completed(fss_fmi_step_status(n->fmi, t, dt));
    size_t op_len = (size_t)FMI3_LS_BUS_BUFFER_LENGTH(&info);
    if (fss_fmi_enter_event_status(n->fmi) > FSS_FMI_STATUS_WARNING ||
        fss_fmi_set_clock(n->fmi, VR_RX_CLOCK) != 0 ||
        fss_fmi_set_binary(n->fmi, VR_RX_DATA, op_buf, op_len) != 0)
        return 0;
    /* dt is the CAPPED step — the FMU step clamp. */
    return fmi_step_completed(fss_fmi_advance_status(n->fmi, t, dt));
}

/* The clock thread: advance now toward granted_upper in quantum steps, stepping
 * the FMU by the CAPPED dt each time (delivering a queued frame if any), send our
 * window, block at the bound; on stop send open_window + report. */
static void *clock_thread(void *arg)
{
    node_t *n = (node_t *)arg;
    const uint64_t q = TIME_QUANTUM_PS;
    send_sim_up(n);   /* announce Elaborated UP before advancing time */
    for (;;) {
        uint64_t old_now, next; int have; can_frame_t f;
        pthread_mutex_lock(&n->mu);
        /* Channel confirmed live: a plain wait suffices — the peer's window WILL
         * arrive and broadcast cv. */
        while (!n->ts_stop && n->granted_upper_ps <= n->now_ps)
            pthread_cond_wait(&n->cv, &n->mu);
        if (n->ts_stop) { pthread_mutex_unlock(&n->mu); break; }
        old_now = n->now_ps;
        next = old_now + q;
        if (next > n->granted_upper_ps) next = n->granted_upper_ps;
        n->now_ps = next;
        have = 0;
        if (n->q_len > 0) {
            f = n->q[n->q_head];
            n->q_head = (n->q_head + 1) % QUEUE_CAP;
            n->q_len--;
            have = 1;
        }
        if (n->peer_seen && n->peer_upper != UINT64_MAX) {
            uint64_t a = next, b = n->peer_last_from;
            uint64_t gap = a > b ? a - b : b - a;
            if (gap > n->max_skew_ps) n->max_skew_ps = gap;
        }
        pthread_mutex_unlock(&n->mu);

        double t  = (double)old_now / 1e12;
        double dt = (double)(next - old_now) / 1e12;
        int fmu_ok = have ? fmu_deliver_and_step(n, &f, t, dt) :
            fmi_step_completed(fss_fmi_step_status(n->fmi, t, dt));
        if (!fmu_ok) {
            /* The async clock has no carrier call to return from.  Its FSS
             * failure propagation is the normal open-window teardown below. */
            pthread_mutex_lock(&n->mu);
            n->fmi_failed = 1;
            n->ts_stop = 1;
            pthread_cond_broadcast(&n->cv);
            pthread_mutex_unlock(&n->mu);
            break;
        }

        ts_send(n, next, next + q);
    }
    uint64_t skew; int any;
    pthread_mutex_lock(&n->mu);
    skew = n->max_skew_ps; any = n->peer_seen;
    pthread_mutex_unlock(&n->mu);
    ts_send(n, n->now_ps, TIME_OPEN_UPPER);   /* detach → cascade */
    if (any) {
        /* Diagnostic, NOT a verdict: bounded multi-quantum skew is real processing
         * latency, not a lockstep violation.  See fss-skew-bounded-transient. */
        printf("FSS TIMESYNC %s: max_clock_gap=%g ns\n", n->name, (double)skew / 1000.0);
        fflush(stdout);
    }
    return NULL;
}

/* fss/config: JSON form pushed before SIM_STARTED (host thread, pre-clock — plain
 * field stores are safe).  "frames" = how many frames to await before teardown
 * (mirrors the initiator's production count). */
static int config_receive(void *self, fss_carrier_t *c)
{
    node_t *n = (node_t *)self;
    if (!c || !c->data_ptr) return FSS_SEND_NO_RECEIVER;
    uint32_t pv_off, st_off, st_sz;
    if (!node_field(n, c, "config::ParamValue", &pv_off, NULL)) return FSS_SEND_NO_RECEIVER;
    const char *json = (const char *)c->data_ptr + pv_off;   /* ParamValue (flat JSON) */
    uint64_t v = 0;
    if (fss_cfg_json_get_u64(json, "frames", &v)) n->expect_frames = v;
    if (node_field(n, c, "config::Status", &st_off, &st_sz) && st_sz >= 4) {
        int32_t st = 0; memcpy((char *)c->data_ptr + st_off, &st, 4);   /* Status = Ok */
    }
    return FSS_SEND_OK;
}

/* PULL config at wired time: the orchestrator installed our fss/config port's send
 * at bind (its single responder is the fan-in sink).  Send a config::param_request
 * {Get}; the round-trip reply lands in-place (synchronous for inproc; the transport
 * blocks for shm/zenoh), then apply it via config_receive — the path the old push
 * used, inverted to pull.  One-shot; no-op if nothing bound our send (a node nobody
 * configures runs on model defaults).  Mirrors the SC artefact's pull_config. */
static void pull_config(node_t *n)
{
    if (n->config_pulled) return;
    n->config_pulled = 1;
    fss_endpoint_t *cfg = &n->config_ep;
    if (!cfg->send) return;
    /* RETRY across an async transport's route setup: immediate for inproc/shm (first
     * pass, no sleep), loops only while a freshly bound zenoh querier waits for its
     * peer queryable.  Bounded; falls back to model defaults if never reachable. */
    for (int attempt = 0; attempt < 300; ++attempt) {
        uint8_t buf[60]; memset(buf, 0, sizeof(buf));   /* OpCode u32@0 = Get(0); ParamValue@16; Status@56 */
        fss_carrier_t c; memset(&c, 0, sizeof(c));
        c.id = FSS_CONFIG_ID; c.data_ptr = buf; c.data_size = sizeof(buf);
        c.meta_ptr = (void *)FSS_CONFIG_META; c.meta_size = FSS_CONFIG_META_LEN;
        uint32_t pv_off;
        if (cfg->send(cfg->send_ctx, &c) == FSS_SEND_OK &&
            node_field(n, &c, "config::ParamValue", &pv_off, NULL) &&
            ((const uint8_t *)c.data_ptr)[pv_off] == '{') {
            config_receive(n, &c);
            return;
        }
        struct timespec ts = { 0, 10 * 1000000L }; nanosleep(&ts, NULL);
    }
}

/* fss/sim: upward readiness wave dispatcher.
 *   Bound|Started → classify chain + mark wired + try_start (sink starts immediately);
 *   Elaborated    → count downstream peer + try_start;
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
             * ARE the downstream peers whose Elaborated the gate awaits.  A pure
             * sink has none and starts as soon as wired. */
            n->n_downstream = n->n_sim_down;
            n->chain_classified = 1;
        }
        n->wired = 1;
        pull_config(n);   /* in-band config BEFORE the clock arms */
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

/* Is the carrier's bus::Data carried INLINE (layout "") rather than by pointer?
 * Every hop2 CAN frame carries ≤8 inline bytes, but read it from the meta so the
 * device never assumes a layout. */
static int data_is_inline(const void *meta, uint64_t meta_size)
{
    int cnt = fss_meta_field_count(meta, meta_size);
    for (int i = 0; i < cnt; ++i) {
        fss_meta_field_t f;
        if (!fss_meta_field_at(meta, meta_size, i, &f)) break;
        if (strcmp(f.qname, "bus::Data") == 0) return f.layout[0] == '\0';
    }
    return 1;   /* default for a CAN frame: inline */
}

/* Inbound CAN frame from ANY controller: META-RESOLVE bus::Locator/Length/Data by
 * qname (no id gate), normalise into a can_frame_t, queue + count.  Stop the clock
 * at EXPECT.  Accepts sccan/fmican/rustcan/pycan frames — distinct layouts, one
 * consumer. */
static int can_rx_receive(void *self, fss_carrier_t *c)
{
    node_t *n = (node_t *)self;
    if (!c || !c->data_ptr || !c->meta_ptr) return FSS_SEND_NO_RECEIVER;

    uint32_t o_loc, o_len, o_data, sz_loc, sz_len, sz;
    if (!node_field(n, c, "bus::Locator", &o_loc,  &sz_loc) ||
        !node_field(n, c, "bus::Length",  &o_len,  &sz_len) ||
        !node_field(n, c, "bus::Data",    &o_data, &sz))
        return FSS_SEND_NO_RECEIVER;   /* required field absent — reject */

    const uint8_t *region = (const uint8_t *)c->data_ptr;
    /* Locator/Length widths vary (u32 or u64): read the resolved width into
     * zeroed scratch so both work. */
    uint64_t locator = 0, len = 0;
    memcpy(&locator, region + o_loc, sz_loc < 8 ? sz_loc : 8);
    memcpy(&len,     region + o_len, sz_len < 8 ? sz_len : 8);
    uint16_t dlc = len <= 8 ? (uint16_t)len : 8;

    const uint8_t *payload;
    if (data_is_inline(c->meta_ptr, c->meta_size)) payload = region + o_data;
    else                                           memcpy(&payload, region + o_data, sizeof(payload));

    pthread_mutex_lock(&n->mu);
    if (n->fmi_failed) {
        pthread_mutex_unlock(&n->mu);
        return FSS_SEND_NO_RECEIVER;
    }
    n->last_id = (uint32_t)locator;
    n->last_b0 = (payload && dlc) ? payload[0] : 0;
    n->rx++;
    if (n->q_len < QUEUE_CAP) {
        can_frame_t *slot = &n->q[n->q_tail];
        slot->id  = (uint32_t)locator;
        slot->dlc = dlc;
        memset(slot->payload, 0, 8);
        if (payload && dlc) memcpy(slot->payload, payload, dlc);
        n->q_tail = (n->q_tail + 1) % QUEUE_CAP;
        n->q_len++;
    }
    if (n->rx >= n->expect_frames) n->ts_stop = 1;
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    return FSS_SEND_OK;
}

FSS_EXPORT fss_node_t *fss_create(fss_transport_t *transport, const char *name)
{
    (void)transport;
    node_t *n = (node_t *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    strncpy(n->name, (name && *name) ? name : "fmi-dev", sizeof(n->name) - 1);
    n->fmi = fss_fmi_open(FSS_FMU_PATH);
    if (!n->fmi) { free(n); return NULL; }
    n->granted_upper_ps = TIME_QUANTUM_PS;
    n->expect_frames = EXPECT_FRAMES;
    pthread_mutex_init(&n->mu, NULL);
    pthread_cond_init(&n->cv, NULL);
    pthread_mutex_init(&n->meta_mu, NULL);
    n->meta_cache = fss_meta_cache_create();
    n->can_rx.self    = n; n->can_rx.receive  = can_rx_receive;
    n->time_ep.self   = n; n->time_ep.receive = time_receive;
    n->sim.self       = n; n->sim.receive     = sim_receive;
    n->config_ep.self = n; n->config_ep.receive = config_receive;
    return (fss_node_t *)n;
}

FSS_EXPORT fss_endpoint_t *fss_get_endpoint(fss_node_t *node, const char *endpoint_name)
{
    node_t *n = (node_t *)node;
    if (strcmp(endpoint_name, "can_rx") == 0)   return &n->can_rx;
    if (strcmp(endpoint_name, "fss/time") == 0) return &n->time_ep;
    if (strcmp(endpoint_name, "fss/config") == 0) return &n->config_ep;
    if (strcmp(endpoint_name, "fss/sim") == 0)  return &n->sim;
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
    fss_meta_cache_destroy(n->meta_cache);
    pthread_mutex_destroy(&n->meta_mu);
    printf("FMI CAN TARGET PASS: rx=%u last_id=0x%X last_b0=%u\n",
           n->rx, n->last_id, n->last_b0);
    fflush(stdout);
    fss_fmi_close(n->fmi);
    free(n);
}
