/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_fmi_tx_paced.c — TIME-SYNCED FMI CAN source (wraps the can_initiator FMU).
 *
 * FMI source-role flavour of the windowed clock: the clock thread PACES EMISSION
 * to the federation quantum rather than clamping the FMU step.  The can_initiator
 * FMU's Tx_Clock fires on its natural FMU_STEP_SIZE (1 ms); that period is far
 * longer than the 1 µs federation quantum, so the FMU is driven by a SEPARATE
 * `fmu_time` accumulator (advanced by FMU_STEP_SIZE, NOT now_ps).  The clock
 * thread is the sole FMU driver — every PRODUCE_EVERY quanta it does ONE FMU
 * event-step at fmu_time, harvests the CanTransmit op, and emits a REGISTER WRITE
 * on "socket".  Production stops at 16 frames; after that the clock keeps ticking
 * as a live timing peer until the controller detaches (stop_on_detach = true for
 * a source/relay: the downstream detach cascades the teardown upstream).
 *
 * R12: hop1 is a REGISTER WRITE into the controller, and this initiator emits its
 * OWN native carrier for it — fmireg::write ({bus::Locator u32@0, bus::Operation
 * u32@4, bus::Length u32@8, bus::Status i32@12, bus::Data@16(8)}), NOT a repacked
 * flat TLM.  All fmireg fields are bus:: atoms, so the controller reads it by
 * meta-resolving qnames with no bridge.  The emitter owns its layout (fixed
 * offsets below); a consumer never needs them.
 *
 * Contrast with the device (can_fmi_rx_paced.c) which CLAMPS the FMU step (the
 * sink-role flavour): both honour the time::window, but a sink's FMU must not
 * step past the window while a source's FMU is purely an internal generator.
 *
 *   "socket"   outbound — fmireg::write register-write (FMI's own hop1 carrier)
 *   "fss/time" timing peer (single, idx 0)
 *   "fss/sim"  upward readiness wave; starts clock when gate opens
 */
#include "fmi.h"
#include "fmi3LsBusUtil.h"
#include "fmi3LsBusUtilCan.h"

#include "fss/artefact_abi.h"
#include "fss/carrier.h"
#include "fss/endpoint.h"
#include "fss/transport_abi.h"
#include "fss/known_carriers/fmireg_write.h"  /* FSS_FMIREG_ID / FSS_FMIREG_META (FMI's own hop1 carrier) */
#include "fss/protocols/config.h"   /* FSS_CONFIG_ID */
#include "fss/c/meta.h"                  /* fss_meta_flatten / fss_meta_resolve */
#include "fss/c/cfg_json.h"              /* fss_cfg_json_get_u64 */
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

/* VR constants — can_initiator FMU. */
#define VR_TX_DATA   1u
#define VR_TX_CLOCK  2u

/* Pacing (shared time-sync constants are in common/fmi_timesync.h). */
#define MAX_SIM_DOWN    4               /* max inbound fss/sim/<tag> sinks (array dim) */
#define FMU_STEP_SIZE   0.001               /* FMU natural period (1 ms) */
#define PRODUCE_EVERY   12                  /* emit 1 frame per 12 quanta */
#define PRODUCE_MAX     16

/* fmireg::write layout — the carrier THIS initiator emits (it owns its layout).
 * bus::Locator u32@0, bus::Operation u32@4, bus::Length u32@8, bus::Status i32@12,
 * bus::Data@16 (8 inline bytes).  24 bytes total. */
#define FMIREG_OFF_LOCATOR    0
#define FMIREG_OFF_OPERATION  4
#define FMIREG_OFF_LENGTH     8
#define FMIREG_OFF_STATUS    12
#define FMIREG_OFF_DATA      16
#define FMIREG_BYTES         24
#define BUS_OP_WRITE          1u   /* bus::Operation::Write */

typedef struct {
    fss_fmi_t       *fmi;
    fss_endpoint_t   socket;       /* outbound flat tlm::transaction TX-writes */
    fss_endpoint_t   time_ep;      /* fss/time peer port                       */
    fss_endpoint_t   sim;          /* fss/sim UP-report port (Elaborated sent here) */
    /* fss/sim/<tag> — one inbound sink per DOWNSTREAM peer; n_sim_down IS this
     * node's n_downstream.  Decoupled from the (symmetric) time peers. */
    fss_endpoint_t   sim_down[MAX_SIM_DOWN];
    char             sim_down_names[MAX_SIM_DOWN][32];
    int              n_sim_down;
    fss_endpoint_t   config_ep;    /* fss/config control port (run length/rate)*/
    char             name[32];

    /* fmireg::write carrier buffer (FMI's own hop1 layout; offsets are fixed). */
    uint8_t          reg[FMIREG_BYTES];
    fss_meta_cache_t *cache;       /* (id,qname) cache for INBOUND meta-resolve */

    /* windowed clock */
    uint64_t         now_ps, granted_upper_ps, max_skew_ps;
    uint64_t         peer_last_from, peer_upper;
    int              peer_seen;
    double           fmu_time;     /* FMU's own time — advanced by FMU_STEP_SIZE */
    int              produced;
    uint64_t         produce_every; /* quanta between emissions (= period_us)    */
    uint64_t         produce_max;   /* total frames to emit (= "frames")         */
    int              ts_stop, ts_running, fmi_failed;
    /* upward readiness wave */
    int              wired, downstream_ready, n_downstream;
    int              chain_classified;
    int              config_pulled;     /* one-shot guard for the wired-time config pull */
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

/* granted_upper = the peer's grant: seen → its Upper (open → MAX); UNSEEN (not
 * yet joined) → the bootstrap ceiling (one quantum) so we emit our first window
 * but never advance past it until the peer joins — stops a source lapping a peer
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

/* Inbound on fss/time: fold the peer's window.  Open window = detach → stop. */
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
        n->peer_upper = UINT64_MAX;   /* detached: source/relay stops */
        n->ts_stop    = 1;
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
    memcpy(buf + 0,  &unit,     4); memcpy(buf + 4,  &pad,      4);
    memcpy(buf + 8,  &lower_ps, 8); memcpy(buf + 16, &upper,    8);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_TIME_ID; c.data_ptr = buf; c.data_size = TW_SIZE;
    c.meta_ptr = (void *)FSS_TIME_META; c.meta_size = FSS_TIME_META_LEN;
    return n->time_ep.send(n->time_ep.send_ctx, &c);
}

/* Build and emit one fmireg::write REGISTER WRITE on "socket" (FMI's own hop1
 * carrier).  bus::Data is carried INLINE (≤8 bytes, the classic register form).
 * Round-trip: bus::Status is written back by the controller (we zero it here). */
static void emit_reg_write(node_t *n, uint32_t id, uint16_t dlc, const uint8_t *payload)
{
    uint16_t ncopy = dlc <= 8 ? dlc : 8;
    memset(n->reg, 0, FMIREG_BYTES);
    uint32_t locator = id, op = BUS_OP_WRITE, len = dlc;
    memcpy(n->reg + FMIREG_OFF_LOCATOR,   &locator, sizeof(locator));
    memcpy(n->reg + FMIREG_OFF_OPERATION, &op,      sizeof(op));
    memcpy(n->reg + FMIREG_OFF_LENGTH,    &len,     sizeof(len));
    /* bus::Status i32@12 left 0 (Incomplete); the controller writes it back. */
    if (payload && ncopy) memcpy(n->reg + FMIREG_OFF_DATA, payload, ncopy);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id        = FSS_FMIREG_ID;
    c.data_ptr  = n->reg;  c.data_size = FMIREG_BYTES;
    c.meta_ptr  = (void *)FSS_FMIREG_META; c.meta_size = FSS_FMIREG_META_LEN;
    if (n->socket.send) n->socket.send(n->socket.send_ctx, &c);
}

/* Warning supplies usable FMI results.  A discarded, terminated, or partial
 * step must not be treated as a produced frame. */
static int fmi_step_completed(fss_fmi_step_result_t result)
{
    return (result.status == FSS_FMI_STATUS_OK ||
            result.status == FSS_FMI_STATUS_WARNING) &&
           !result.terminate_simulation && !result.early_return;
}

/* Clock thread: advance now by quantum, every PRODUCE_EVERY quanta drive the FMU
 * once at fmu_time (NOT now_ps) to harvest the next CanTransmit op.  Emission is
 * paced to the federation clock; the FMU step is decoupled from now_ps. */
static void *clock_thread(void *arg)
{
    node_t *n = (node_t *)arg;
    const uint64_t q = TIME_QUANTUM_PS;
    uint64_t quantum_count = 0;
    send_sim_up(n);   /* announce Elaborated UP before advancing time */
    for (;;) {
        uint64_t old_now, next;
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
        if (n->peer_seen && n->peer_upper != UINT64_MAX) {
            uint64_t a = next, b = n->peer_last_from;
            uint64_t gap = a > b ? a - b : b - a;
            if (gap > n->max_skew_ps) n->max_skew_ps = gap;
        }
        int produced = n->produced;
        pthread_mutex_unlock(&n->mu);

        quantum_count++;

        /* Every produce_every quanta and still below cap: drive FMU once. */
        if (quantum_count % n->produce_every == 0 && (uint64_t)produced < n->produce_max) {
            double ft = n->fmu_time;
            int fmu_ok = fss_fmi_enter_event_status(n->fmi) <= FSS_FMI_STATUS_WARNING &&
                fmi_step_completed(fss_fmi_advance_status(n->fmi, ft, FMU_STEP_SIZE));
            if (!fmu_ok) {
                /* The operation runs on the async clock.  Stop it and propagate
                 * the FSS failure through the normal open-window teardown. */
                pthread_mutex_lock(&n->mu);
                n->fmi_failed = 1;
                n->ts_stop = 1;
                pthread_cond_broadcast(&n->cv);
                pthread_mutex_unlock(&n->mu);
                break;
            }
            n->fmu_time += FMU_STEP_SIZE;
            int tx_clock = 0;
            fss_fmi_status_t status = fss_fmi_get_clock_status(n->fmi, VR_TX_CLOCK, &tx_clock);
            if (status != FSS_FMI_STATUS_OK && status != FSS_FMI_STATUS_WARNING) {
                pthread_mutex_lock(&n->mu);
                n->fmi_failed = 1;
                n->ts_stop = 1;
                pthread_cond_broadcast(&n->cv);
                pthread_mutex_unlock(&n->mu);
                break;
            }
            if (tx_clock) {
                const void *op = NULL; size_t oplen = 0;
                status = fss_fmi_get_binary_status(n->fmi, VR_TX_DATA, &op, &oplen);
                if (status != FSS_FMI_STATUS_OK && status != FSS_FMI_STATUS_WARNING) {
                    pthread_mutex_lock(&n->mu);
                    n->fmi_failed = 1;
                    n->ts_stop = 1;
                    pthread_cond_broadcast(&n->cv);
                    pthread_mutex_unlock(&n->mu);
                    break;
                }
                if (op &&
                    oplen >= sizeof(fmi3LsBusCanOperationCanTransmit)) {
                    const fmi3LsBusCanOperationCanTransmit *t = op;
                    emit_reg_write(n, (uint32_t)t->id, (uint16_t)t->dataLength, t->data);
                }
            }
            pthread_mutex_lock(&n->mu);
            n->produced++;
            pthread_mutex_unlock(&n->mu);
        }
        /* After all frames sent, keep advancing as a live peer until detach. */

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
 * field stores are safe).  "frames" = total frames to emit; "period_us" = the
 * production period in µs == quanta between emissions (the quantum is 1 µs). */
static int config_receive(void *self, fss_carrier_t *c)
{
    node_t *n = (node_t *)self;
    if (!c || !c->data_ptr) return FSS_SEND_NO_RECEIVER;
    uint32_t pv_off, st_off, st_sz;
    if (!node_field(n, c, "config::ParamValue", &pv_off, NULL)) return FSS_SEND_NO_RECEIVER;
    const char *json = (const char *)c->data_ptr + pv_off;   /* ParamValue (flat JSON) */
    uint64_t v = 0;
    if (fss_cfg_json_get_u64(json, "frames", &v))               n->produce_max   = v;
    if (fss_cfg_json_get_u64(json, "period_us", &v) && v > 0)   n->produce_every = v;
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
 *   Bound|Started → classify chain + mark wired + try_start;
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
            /* The source's downstream sink ("fss/sim/ctrl") was allocated by the
             * orchestrator before Bound; its Elaborated is what the gate awaits.
             * n_sim_down IS n_downstream. */
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

FSS_EXPORT fss_node_t *fss_create(fss_transport_t *transport, const char *name)
{
    (void)transport;
    node_t *n = (node_t *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    strncpy(n->name, (name && *name) ? name : "fmi-src", sizeof(n->name) - 1);
    n->fmi = fss_fmi_open(FSS_FMU_PATH);
    if (!n->fmi) { free(n); return NULL; }

    /* Cache for INBOUND meta-resolve (time/config/sim carriers).  The OUTBOUND
     * fmireg::write layout is fixed (this initiator owns it) — no flatten needed. */
    n->cache = fss_meta_cache_create();
    if (!n->cache) { fss_fmi_close(n->fmi); free(n); return NULL; }

    n->granted_upper_ps = TIME_QUANTUM_PS;
    n->produce_every    = PRODUCE_EVERY;
    n->produce_max      = PRODUCE_MAX;
    pthread_mutex_init(&n->mu, NULL);
    pthread_cond_init(&n->cv, NULL);
    pthread_mutex_init(&n->meta_mu, NULL);

    n->socket.self  = n;   /* outbound: send installed by host bind */
    n->time_ep.self = n; n->time_ep.receive = time_receive;
    n->sim.self     = n; n->sim.receive     = sim_receive;
    n->config_ep.self = n; n->config_ep.receive = config_receive;
    return (fss_node_t *)n;
}

FSS_EXPORT fss_endpoint_t *fss_get_endpoint(fss_node_t *node, const char *endpoint_name)
{
    node_t *n = (node_t *)node;
    if (strcmp(endpoint_name, "socket") == 0)   return &n->socket;
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
    pthread_mutex_destroy(&n->meta_mu);
    if (n->cache) fss_meta_cache_destroy(n->cache);
    fss_fmi_close(n->fmi);
    free(n);
}
