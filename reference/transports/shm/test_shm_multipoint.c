/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * test_shm_multipoint.c — spike proving the SHM transport supports
 * cross-PROCESS multipoint with NO change to shm.c.  The hypothesis: shm is a
 * 1:1 SPSC primitive (ONE channel per transport instance, one segment per
 * channel, one reader per segment — shm.c:588-597, :556-565).  Multipoint is
 * therefore realised as N INDEPENDENT 1:1 SPSC segments, entirely in the
 * WIRING, with shm.c untouched:
 *
 *   FAN-OUT 1->N : the sender opens N distinct one-way channels ("mp_out_0"..),
 *                  attaches INITIATOR to each, and broadcasts by sending on each
 *                  in turn (a fan-out shim).  Each of the N targets is its own
 *                  process owning its own 1:1 segment.  NO shared segment, NO
 *                  two readers on one ring.  Every target must receive all
 *                  N_FRAMES.
 *   FAN-IN  N->1 : N initiator processes each open their OWN round-trip channel
 *                  ("mp_in_0"..) to the one responder.  The responder attaches
 *                  its ONE sink endpoint as TARGET to all N channels -> N reader
 *                  threads all delivering into the same sink.  The sink is
 *                  REENTRANT (it writes value+DELTA back purely from the request
 *                  bytes into the per-call carrier — no shared mutable state).
 *                  Each initiator's round-trip returns on ITS OWN segment's reply
 *                  ring, so each querier asserts it got the reply for ITS OWN
 *                  request (req_id==idx, value==idx+DELTA) — no cross-talk.
 *
 * The OWNER (parent) provisions ALL segments as owner=1 BEFORE forking (the
 * orchestrator-provisions model), so children attach owner=0 to a segment that
 * already exists — no O_EXCL create/attach race.
 *
 * A trivial raw carrier (no meta / no pointer fields) is used, matching the
 * zenoh multipoint spike: data is
 *   [u32 req_id][u8 value]
 * which shm copies verbatim into a ring slot and (round-trip) copies the mutated
 * bytes back over the caller's carrier — the real wire path, honestly.
 *
 *   argv[1] = fss_shm transport module path.
 *   argv[2] = "fanout" | "fanin".
 */
#include "fss/transport_abi.h"
#include "fss/endpoint.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define N_PROC       3          /* number of child processes (>= 2)          */
#define N_FRAMES     8          /* fan-out frames the sender emits per target */
#define RAW_ID       UINT64_C(0xABCD0001)
#define REPLY_DELTA  100        /* responder returns value + REPLY_DELTA      */

#define CH_FANOUT    "mp_out_"  /* per-target channel prefix ("mp_out_0"..)  */
#define CH_FANIN     "mp_in_"   /* per-querier channel prefix ("mp_in_0"..)  */

typedef fss_transport_t *(*create_fn_t)(const char *);
typedef void             (*destroy_fn_t)(fss_transport_t *);

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── the raw carrier body ─────────────────────────────────────────────────── */
typedef struct { uint32_t req_id; uint8_t value; } rawmsg;

static void put_msg(uint8_t *buf, uint32_t req_id, uint8_t value)
{
    memset(buf, 0, sizeof(rawmsg));
    memcpy(buf, &req_id, sizeof(req_id));
    buf[sizeof(uint32_t)] = value;
}
static void get_msg(const uint8_t *buf, uint32_t *req_id, uint8_t *value)
{
    memcpy(req_id, buf, sizeof(*req_id));
    *value = buf[sizeof(uint32_t)];
}

/* ══ FAN-OUT ══════════════════════════════════════════════════════════════ */

typedef struct {
    volatile unsigned count;
    volatile uint32_t last_id;
    volatile uint8_t  last_value;
} rx_record;

static int fanout_sink_receive(void *self, fss_carrier_t *c)
{
    rx_record *r = (rx_record *)self;
    if (c && c->data_ptr && c->data_size >= sizeof(rawmsg)) {
        uint32_t id; uint8_t v;
        get_msg((const uint8_t *)c->data_ptr, &id, &v);
        r->last_id    = id;
        r->last_value = v;
    }
    r->count++;
    return FSS_SEND_OK;
}

/* Child = target idx.  Attaches owner=0 to its OWN one-way segment "mp_out_idx"
 * (the parent created it before forking) with an rx_record sink, then waits to
 * receive all N_FRAMES.  Asserts exactly N_FRAMES, last value N_FRAMES-1. */
static int fanout_target(create_fn_t create, destroy_fn_t destroy, int idx)
{
    char ch_name[32];
    snprintf(ch_name, sizeof(ch_name), "%s%d", CH_FANOUT, idx);

    fss_transport_t *t = create("");
    if (!t) { fprintf(stderr, "[FAIL] target %d create\n", idx); return 1; }

    /* Segment exists (parent = owner, created pre-fork); short retry anyway. */
    fss_channel_t *ch = NULL;
    for (int i = 0; i < 400 && !ch; i++) {
        ch = t->channel(t->self, ch_name, 0 /*one-way*/, 0 /*attacher*/, NULL, 0);
        if (!ch) msleep(5);
    }
    if (!ch) { fprintf(stderr, "[FAIL] target %d channel %s\n", idx, ch_name); destroy(t); return 1; }

    rx_record rec = {0};
    fss_endpoint_t sink = {0};
    sink.self = &rec; sink.receive = fanout_sink_receive;
    t->attach(t->self, ch, &sink, FSS_ROLE_TARGET);

    for (int i = 0; i < 2000 && rec.count < N_FRAMES; i++) msleep(5);

    int pass = (rec.count == N_FRAMES &&
                rec.last_id == (uint32_t)RAW_ID &&
                rec.last_value == (uint8_t)(N_FRAMES - 1));
    if (!pass)
        fprintf(stderr, "[FAIL] target %d: count=%u (want %d) last_id=0x%x last_value=%u\n",
                idx, rec.count, N_FRAMES, rec.last_id, rec.last_value);
    else
        fprintf(stderr, "[ok]   target %d: received all %u frames, last_value=%u\n",
                idx, rec.count, rec.last_value);
    destroy(t);
    return pass ? 0 : 1;
}

static int run_fanout(create_fn_t create, destroy_fn_t destroy)
{
    /* Parent = sender.  Provision all N one-way segments as OWNER first (the
     * orchestrator-provisions model), THEN fork the targets — each attaches
     * owner=0 to a segment that already exists (no O_EXCL create/attach race). */
    fss_transport_t *tx[N_PROC] = {0};
    fss_channel_t   *chs[N_PROC] = {0};
    for (int i = 0; i < N_PROC; i++) {
        char ch_name[32];
        snprintf(ch_name, sizeof(ch_name), "%s%d", CH_FANOUT, i);
        tx[i] = create("");
        if (!tx[i]) { fprintf(stderr, "[FAIL] sender %d create\n", i); return 1; }
        chs[i] = tx[i]->channel(tx[i]->self, ch_name, 0 /*one-way*/, 1 /*owner*/, NULL, 0);
        if (!chs[i]) { fprintf(stderr, "[FAIL] sender %d owner channel %s\n", i, ch_name); return 1; }
    }

    pid_t kids[N_PROC];
    for (int i = 0; i < N_PROC; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            /* Child owns nothing the parent created; drop the parent's handles
             * (do NOT destroy — that would unlink the owner's names) and open
             * only its own segment. */
            int rc = fanout_target(create, destroy, i);
            _exit(rc);
        }
        kids[i] = pid;
    }

    /* Attach INITIATOR to each of the N segments; fan-out = send on each. */
    fss_endpoint_t ep[N_PROC]; memset(ep, 0, sizeof(ep));
    for (int i = 0; i < N_PROC; i++) {
        if (tx[i]->attach(tx[i]->self, chs[i], &ep[i], FSS_ROLE_INITIATOR) != FSS_SEND_OK || !ep[i].send) {
            fprintf(stderr, "[FAIL] sender %d attach INITIATOR\n", i);
            return 1;
        }
    }

    int send_err = 0;
    for (int f = 0; f < N_FRAMES && !send_err; f++) {
        uint8_t buf[sizeof(rawmsg)];
        put_msg(buf, (uint32_t)RAW_ID, (uint8_t)f);
        for (int i = 0; i < N_PROC && !send_err; i++) {     /* the fan-out shim */
            fss_carrier_t c; memset(&c, 0, sizeof(c));
            c.id = RAW_ID; c.data_ptr = buf; c.data_size = sizeof(buf);
            if (ep[i].send(ep[i].send_ctx, &c) != FSS_SEND_OK) send_err = 1;
        }
    }
    if (send_err) fprintf(stderr, "[FAIL] sender: a send returned non-OK\n");

    int all_ok = !send_err;
    for (int i = 0; i < N_PROC; i++) {
        int ws = 0; waitpid(kids[i], &ws, 0);
        if (!(WIFEXITED(ws) && WEXITSTATUS(ws) == 0)) all_ok = 0;
    }
    for (int i = 0; i < N_PROC; i++) if (tx[i]) destroy(tx[i]);   /* owner unlinks */

    if (all_ok) { printf("[OK] shm fan-out: all %d targets got all %d frames (N independent 1:1 segments)\n",
                         N_PROC, N_FRAMES); return 0; }
    fprintf(stderr, "[FAIL] shm fan-out\n");
    return 1;
}

/* ══ FAN-IN ═══════════════════════════════════════════════════════════════ */

/* Responder sink: echo req_id, write value += REPLY_DELTA back in place.  This
 * runs CONCURRENTLY on N reader threads (one per segment).  It is REENTRANT: it
 * touches only the per-call carrier's own bytes (each thread's own segment slot)
 * and holds NO shared mutable state, so no lock is needed. */
static int fanin_sink_receive(void *self, fss_carrier_t *c)
{
    (void)self;
    if (c && c->data_ptr && c->data_size >= sizeof(rawmsg)) {
        uint8_t *d = (uint8_t *)c->data_ptr;
        uint32_t id; uint8_t v;
        get_msg(d, &id, &v);
        put_msg(d, id, (uint8_t)(v + REPLY_DELTA));   /* per-request reply */
    }
    return FSS_SEND_OK;
}

/* Child = querier idx.  Round-trip on its OWN segment "mp_in_idx": sends
 * [req_id=idx][value=idx]; the reply copied back must be [idx][idx+DELTA] —
 * proving it got ITS OWN reply on ITS OWN reply ring, not another querier's. */
static int fanin_querier(create_fn_t create, destroy_fn_t destroy, int idx)
{
    char ch_name[32];
    snprintf(ch_name, sizeof(ch_name), "%s%d", CH_FANIN, idx);

    fss_transport_t *t = create("");
    if (!t) { fprintf(stderr, "[FAIL] q %d create\n", idx); return 1; }

    fss_channel_t *ch = NULL;
    for (int i = 0; i < 400 && !ch; i++) {
        ch = t->channel(t->self, ch_name, 1 /*round-trip*/, 0 /*attacher*/, NULL, 0);
        if (!ch) msleep(5);
    }
    if (!ch) { fprintf(stderr, "[FAIL] q %d channel %s\n", idx, ch_name); destroy(t); return 1; }

    fss_endpoint_t ep = {0};
    if (t->attach(t->self, ch, &ep, FSS_ROLE_INITIATOR) != FSS_SEND_OK || !ep.send) {
        fprintf(stderr, "[FAIL] q %d attach\n", idx); destroy(t); return 1;
    }

    uint8_t buf[sizeof(rawmsg)];
    put_msg(buf, (uint32_t)idx, (uint8_t)idx);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = RAW_ID; c.data_ptr = buf; c.data_size = sizeof(buf);
    int rc = ep.send(ep.send_ctx, &c);   /* synchronous round-trip: blocks for reply */

    uint32_t rid = 0; uint8_t rval = 0;
    get_msg(buf, &rid, &rval);

    int pass = (rc == FSS_SEND_OK &&
                rid == (uint32_t)idx && rval == (uint8_t)(idx + REPLY_DELTA));
    if (!pass)
        fprintf(stderr, "[FAIL] q %d: rc=%d reply req_id=%u (want %d) value=%u (want %d)\n",
                idx, rc, rid, idx, rval, idx + REPLY_DELTA);
    else
        fprintf(stderr, "[ok]   q %d: reply req_id=%u value=%u (its own)\n", idx, rid, rval);
    destroy(t);
    return pass ? 0 : 1;
}

static int run_fanin(create_fn_t create, destroy_fn_t destroy)
{
    /* Parent = responder.  Provision all N round-trip segments as OWNER first,
     * then attach its ONE sink endpoint as TARGET to all N -> N reader threads
     * into one reentrant sink.  Fork the queriers only after the sinks (and
     * their reader threads) are live, so a querier's synchronous round-trip is
     * always serviced. */
    fss_transport_t *tx[N_PROC] = {0};
    fss_endpoint_t sink = {0};
    sink.receive = fanin_sink_receive;   /* stateless + reentrant: one sink, N readers */

    for (int i = 0; i < N_PROC; i++) {
        char ch_name[32];
        snprintf(ch_name, sizeof(ch_name), "%s%d", CH_FANIN, i);
        tx[i] = create("");
        if (!tx[i]) { fprintf(stderr, "[FAIL] resp %d create\n", i); return 1; }
        fss_channel_t *ch = tx[i]->channel(tx[i]->self, ch_name, 1 /*round-trip*/, 1 /*owner*/, NULL, 0);
        if (!ch) { fprintf(stderr, "[FAIL] resp %d owner channel %s\n", i, ch_name); return 1; }
        tx[i]->attach(tx[i]->self, ch, &sink, FSS_ROLE_TARGET);   /* starts reader i */
    }

    pid_t kids[N_PROC];
    for (int i = 0; i < N_PROC; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            int rc = fanin_querier(create, destroy, i);
            _exit(rc);
        }
        kids[i] = pid;
    }

    int all_ok = 1;
    for (int i = 0; i < N_PROC; i++) {
        int ws = 0; waitpid(kids[i], &ws, 0);
        if (!(WIFEXITED(ws) && WEXITSTATUS(ws) == 0)) all_ok = 0;
    }
    for (int i = 0; i < N_PROC; i++) if (tx[i]) destroy(tx[i]);   /* joins reader, owner unlinks */

    if (all_ok) { printf("[OK] shm fan-in: all %d queriers got their own reply (N readers -> 1 reentrant sink)\n",
                         N_PROC); return 0; }
    fprintf(stderr, "[FAIL] shm fan-in\n");
    return 1;
}

/* ══ main ═════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <fss_shm-module> <fanout|fanin>\n", argv[0]);
        return 2;
    }
    const char *module = argv[1];
    const char *mode   = argv[2];

    void *dl = dlopen(module, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { fprintf(stderr, "[FAIL] dlopen: %s\n", dlerror()); return 1; }
    fss_transport_abi_version_fn_t version =
        (fss_transport_abi_version_fn_t)dlsym(dl, "fss_transport_abi_version");
    fss_transport_capabilities_fn_t capabilities =
        (fss_transport_capabilities_fn_t)dlsym(dl, "fss_transport_capabilities");
    if (!version || !capabilities || !fss_transport_abi_compatible(version(), capabilities())) {
        fprintf(stderr, "[FAIL] transport ABI query/capability check\n");
        dlclose(dl);
        return 1;
    }
    create_fn_t  create  = (create_fn_t) dlsym(dl, "fss_transport_create");
    destroy_fn_t destroy = (destroy_fn_t)dlsym(dl, "fss_transport_destroy");
    if (!create || !destroy) { fprintf(stderr, "[FAIL] dlsym: %s\n", dlerror()); return 1; }

    int rc;
    if      (strcmp(mode, "fanout") == 0) rc = run_fanout(create, destroy);
    else if (strcmp(mode, "fanin")  == 0) rc = run_fanin(create, destroy);
    else { fprintf(stderr, "unknown mode '%s'\n", mode); rc = 2; }

    dlclose(dl);
    return rc;
}
