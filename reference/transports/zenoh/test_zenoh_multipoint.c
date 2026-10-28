/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * test_zenoh_multipoint.c — spike proving the zenoh transport supports
 * cross-PROCESS multipoint with NO change to zenoh.c.  The hypothesis: because
 * zenoh pub/sub is natively 1->N and get/queryable natively N->1, if each
 * participant is its OWN process (its own zenoh_state + session), the single
 * `sink`-per-instance cap never bites — it only bites when two sinks share one
 * key in ONE process, which does not happen here.
 *
 *   FAN-OUT : 1 publisher (INITIATOR, one-way) + N subscriber processes on key
 *             "fss/mp/fanout".  Every subscriber must receive all N frames.
 *   FAN-IN  : 1 queryable/responder (TARGET, round-trip) + N querier processes
 *             on key "fss/mp/fanin".  The responder writes a per-request
 *             distinguishable reply (echoes req_id, value+DELTA); each querier
 *             asserts the reply it gets back is the one for ITS OWN request,
 *             proving replies route per-querier.
 *
 * The "1" side (parent) LISTENS on a fixed loopback endpoint; the N children
 * CONNECT to it — a deterministic same-host rendezvous (no multicast).  Each
 * process opens its own zenoh session AFTER fork (never inherit a session).
 *
 * A trivial raw carrier is used (no meta / no pointer fields): data is
 *   [u32 req_id][u8 value]
 * which build_frame carries verbatim and (round-trip) copies the mutated bytes
 * back over the caller's carrier — exercising the real wire path honestly.
 *
 *   argv[1] = fss_zenoh transport module path.
 *   argv[2] = "fanout" | "fanin" | "ready-roundtrip" | "ready-pubsub" |
 *             "overlong-channel".
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
#define N_FRAMES     8          /* fan-out frames the publisher emits        */
#define RAW_ID       UINT64_C(0xABCD0001)
#define REPLY_DELTA  100        /* responder returns value + REPLY_DELTA      */

#define KEY_FANOUT   "fss/mp/fanout"
#define KEY_FANIN    "fss/mp/fanin"
#define EP_FANOUT    "tcp/127.0.0.1:17795"
#define EP_FANIN     "tcp/127.0.0.1:17796"
#define EP_READY_Q   "tcp/127.0.0.1:17797"
#define EP_READY_P   "tcp/127.0.0.1:17798"

typedef fss_transport_t *(*create_fn_t)(const char *);
typedef void             (*destroy_fn_t)(fss_transport_t *);
typedef int (*set_edge_fn_t)(fss_transport_t *, const char *, const char *, int);
typedef int (*wait_ready_fn_t)(fss_transport_t *, uint32_t);

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

/* Child = subscriber.  Connects to the publisher, declares its subscriber,
 * signals readiness on `wfd`, then waits to receive all N frames.  Asserts it
 * saw exactly the frames the publisher sent (all N, last value N-1). */
static int fanout_subscriber(create_fn_t create, destroy_fn_t destroy, int wfd, int idx)
{
    fss_transport_t *t = create("connect=" EP_FANOUT);
    if (!t) { fprintf(stderr, "[FAIL] sub %d create\n", idx); return 1; }

    fss_channel_t *ch = t->channel(t->self, KEY_FANOUT, 0 /*one-way*/, 0, NULL, 0);
    if (!ch) { fprintf(stderr, "[FAIL] sub %d channel\n", idx); destroy(t); return 1; }

    rx_record rec = {0};
    fss_endpoint_t sink = {0};
    sink.self = &rec; sink.receive = fanout_sink_receive;
    t->attach(t->self, ch, &sink, FSS_ROLE_TARGET);

    /* Subscriber declared — signal the publisher we're ready. */
    unsigned char ready = 1;
    if (write(wfd, &ready, 1) != 1) { /* best-effort */ }
    close(wfd);

    /* Wait for all N frames (bounded). */
    for (int i = 0; i < 2000 && rec.count < N_FRAMES; i++) msleep(5);

    int pass = (rec.count == N_FRAMES &&
                rec.last_id == (uint32_t)RAW_ID &&
                rec.last_value == (uint8_t)(N_FRAMES - 1));
    if (!pass)
        fprintf(stderr, "[FAIL] sub %d: count=%u (want %d) last_id=0x%x last_value=%u\n",
                idx, rec.count, N_FRAMES, rec.last_id, rec.last_value);
    else
        fprintf(stderr, "[ok]   sub %d: received all %u frames, last_value=%u\n",
                idx, rec.count, rec.last_value);
    destroy(t);
    return pass ? 0 : 1;
}

static int run_fanout(create_fn_t create, destroy_fn_t destroy)
{
    /* One pipe per child: child writes a readiness byte after it declares its
     * subscriber; the parent reads all N before it starts publishing. */
    int pfd[N_PROC][2];
    pid_t kids[N_PROC];
    for (int i = 0; i < N_PROC; i++) {
        if (pipe(pfd[i]) != 0) { perror("pipe"); return 1; }
    }

    for (int i = 0; i < N_PROC; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            /* child: close every read end + other children's write ends */
            for (int j = 0; j < N_PROC; j++) {
                close(pfd[j][0]);
                if (j != i) close(pfd[j][1]);
            }
            int rc = fanout_subscriber(create, destroy, pfd[i][1], i);
            _exit(rc);
        }
        kids[i] = pid;
        close(pfd[i][1]);   /* parent keeps only read ends */
    }

    /* Parent = publisher.  Listen so the children's connects rendezvous here. */
    fss_transport_t *t = create("listen=" EP_FANOUT);
    if (!t) { fprintf(stderr, "[FAIL] pub create\n"); return 1; }
    fss_channel_t *ch = t->channel(t->self, KEY_FANOUT, 0 /*one-way*/, 1, NULL, 0);
    if (!ch) { fprintf(stderr, "[FAIL] pub channel\n"); destroy(t); return 1; }
    fss_endpoint_t ep = {0};
    if (t->attach(t->self, ch, &ep, FSS_ROLE_INITIATOR) != FSS_SEND_OK || !ep.send) {
        fprintf(stderr, "[FAIL] pub attach\n"); destroy(t); return 1;
    }

    /* Barrier: wait for every subscriber to declare, then settle so zenoh's
     * async pub/sub matching completes before we publish (matched + BLOCK
     * congestion control => reliable delivery, so we assert EXACT counts). */
    for (int i = 0; i < N_PROC; i++) {
        unsigned char b; ssize_t n = read(pfd[i][0], &b, 1); (void)n;
        close(pfd[i][0]);
    }
    msleep(1000);

    int send_err = 0;
    for (int i = 0; i < N_FRAMES && !send_err; i++) {
        uint8_t buf[sizeof(rawmsg)];
        put_msg(buf, (uint32_t)RAW_ID, (uint8_t)i);
        fss_carrier_t c; memset(&c, 0, sizeof(c));
        c.id = RAW_ID; c.data_ptr = buf; c.data_size = sizeof(buf);
        if (ep.send(ep.send_ctx, &c) != FSS_SEND_OK) { send_err = 1; }
    }
    if (send_err) fprintf(stderr, "[FAIL] pub: a publish returned non-OK\n");

    int all_ok = !send_err;
    for (int i = 0; i < N_PROC; i++) {
        int ws = 0; waitpid(kids[i], &ws, 0);
        int ok = WIFEXITED(ws) && WEXITSTATUS(ws) == 0;
        if (!ok) all_ok = 0;
    }
    destroy(t);

    if (all_ok) { printf("[OK] zenoh fan-out: all %d subscribers got all %d frames\n",
                         N_PROC, N_FRAMES); return 0; }
    fprintf(stderr, "[FAIL] zenoh fan-out\n");
    return 1;
}

/* ══ FAN-IN ═══════════════════════════════════════════════════════════════ */

/* Responder sink: echo req_id unchanged, write value += REPLY_DELTA back in
 * place.  The transport ferries the mutated bytes back to the querier. */
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

/* Child = querier idx.  Round-trip: sends [req_id=idx][value=idx]; the reply
 * copied back must be [req_id=idx][value=idx+DELTA] — proving it got ITS OWN
 * reply, not another querier's. */
static int fanin_querier(create_fn_t create, destroy_fn_t destroy, int idx)
{
    fss_transport_t *t = create("connect=" EP_FANIN);
    if (!t) { fprintf(stderr, "[FAIL] q %d create\n", idx); return 1; }
    fss_channel_t *ch = t->channel(t->self, KEY_FANIN, 1 /*round-trip*/, 0, NULL, 0);
    if (!ch) { fprintf(stderr, "[FAIL] q %d channel\n", idx); destroy(t); return 1; }
    fss_endpoint_t ep = {0};
    if (t->attach(t->self, ch, &ep, FSS_ROLE_INITIATOR) != FSS_SEND_OK || !ep.send) {
        fprintf(stderr, "[FAIL] q %d attach\n", idx); destroy(t); return 1;
    }
    /* Re-attaching the same endpoint is a no-op.  In particular, it must keep
     * the original send context instead of allocating and overwriting it. */
    void *send_ctx = ep.send_ctx;
    if (t->attach(t->self, ch, &ep, FSS_ROLE_INITIATOR) != FSS_SEND_OK ||
        ep.send_ctx != send_ctx) {
        fprintf(stderr, "[FAIL] q %d duplicate attach\n", idx); destroy(t); return 1;
    }

    /* Discovery is async and the responder may not be up yet — retry the query
     * until we get a well-formed reply (or give up). */
    int got = 0; uint32_t rid = 0; uint8_t rval = 0;
    for (int attempt = 0; attempt < 40 && !got; attempt++) {
        uint8_t buf[sizeof(rawmsg)];
        put_msg(buf, (uint32_t)idx, (uint8_t)idx);
        fss_carrier_t c; memset(&c, 0, sizeof(c));
        c.id = RAW_ID; c.data_ptr = buf; c.data_size = sizeof(buf);
        int rc = ep.send(ep.send_ctx, &c);
        if (rc == FSS_SEND_OK) {
            get_msg(buf, &rid, &rval);
            got = 1;
        } else {
            msleep(100);
        }
    }

    int pass = got && rid == (uint32_t)idx && rval == (uint8_t)(idx + REPLY_DELTA);
    if (!pass)
        fprintf(stderr, "[FAIL] q %d: got=%d reply req_id=%u (want %d) value=%u (want %d)\n",
                idx, got, rid, idx, rval, idx + REPLY_DELTA);
    else
        fprintf(stderr, "[ok]   q %d: reply req_id=%u value=%u (its own)\n", idx, rid, rval);
    destroy(t);
    return pass ? 0 : 1;
}

static int run_fanin(create_fn_t create, destroy_fn_t destroy)
{
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

    /* Parent = responder.  Listen + declare a queryable; the queriers' retry
     * loops tolerate us not being up yet, so no reverse barrier is needed. */
    fss_transport_t *t = create("listen=" EP_FANIN);
    if (!t) { fprintf(stderr, "[FAIL] resp create\n"); return 1; }
    fss_channel_t *ch = t->channel(t->self, KEY_FANIN, 1 /*round-trip*/, 1, NULL, 0);
    if (!ch) { fprintf(stderr, "[FAIL] resp channel\n"); destroy(t); return 1; }
    fss_endpoint_t sink = {0};
    sink.receive = fanin_sink_receive;
    t->attach(t->self, ch, &sink, FSS_ROLE_TARGET);

    int all_ok = 1;
    for (int i = 0; i < N_PROC; i++) {
        int ws = 0; waitpid(kids[i], &ws, 0);
        int ok = WIFEXITED(ws) && WEXITSTATUS(ws) == 0;
        if (!ok) all_ok = 0;
    }
    destroy(t);

    if (all_ok) { printf("[OK] zenoh fan-in: all %d queriers got their own reply\n",
                         N_PROC); return 0; }
    fprintf(stderr, "[FAIL] zenoh fan-in\n");
    return 1;
}

/* Local regression for the remote-ready contract.  The target sink must not
 * see readiness envelopes: a successful wait with count zero proves both that
 * the real Zenoh primitive was used and that probes stay out of application
 * delivery.  Each carrier shape runs in a separate test process.  This keeps
 * fork before any Zenoh/Tokio runtime is created in that process. */
static int run_ready_one(create_fn_t create, destroy_fn_t destroy,
                         set_edge_fn_t set_edge, wait_ready_fn_t wait_ready,
                         const char *key, const char *endpoint, int pubsub)
{
    pid_t kid = fork();
    if (kid < 0) return 1;
    if (kid == 0) {
        char cfg[320]; snprintf(cfg, sizeof(cfg), "connect=%s", endpoint);
        fss_transport_t *t = create(cfg);
        if (!t) _exit(1);
        fss_channel_t *ch = t->channel(t->self, key, pubsub ? 0 : 1, 0, NULL, 0);
        rx_record rec = {0}; fss_endpoint_t sink = {0};
        sink.self = &rec; sink.receive = fanout_sink_receive;
        int ok = ch && set_edge(t, "ready-target", "ready-initiator", FSS_ROLE_TARGET) == FSS_SEND_OK &&
                 t->attach(t->self, ch, &sink, FSS_ROLE_TARGET) == FSS_SEND_OK;
        if (ok) msleep(4500);
        ok = ok && rec.count == 0;
        destroy(t); _exit(ok ? 0 : 1);
    }
    char cfg[320]; snprintf(cfg, sizeof(cfg), "listen=%s", endpoint);
    fss_transport_t *t = create(cfg);
    int ok = t != NULL;
    fss_channel_t *ch = ok ? t->channel(t->self, key, pubsub ? 0 : 1, 1, NULL, 0) : NULL;
    fss_endpoint_t ep = {0};
    ok = ok && ch && set_edge(t, "ready-initiator", "ready-target", FSS_ROLE_INITIATOR) == FSS_SEND_OK &&
         t->attach(t->self, ch, &ep, FSS_ROLE_INITIATOR) == FSS_SEND_OK &&
         wait_ready(t, 3500) == FSS_SEND_OK;
    if (t) destroy(t);
    int status = 0; waitpid(kid, &status, 0);
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

/* Channel keys are copied into a fixed transport field.  Reject an oversized
 * key rather than silently truncating it into a different channel. */
static int run_overlong_channel(create_fn_t create, destroy_fn_t destroy)
{
    char key[257];
    memset(key, 'x', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    fss_transport_t *t = create(NULL);
    if (!t) return 1;
    fss_channel_t *ch = t->channel(t->self, key, 1, 0, NULL, 0);
    destroy(t);
    return ch == NULL ? 0 : 1;
}

/* ══ main ═════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <fss_zenoh-module> <fanout|fanin|ready-roundtrip|ready-pubsub|overlong-channel>\n", argv[0]);
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
    if (!version || !capabilities || !fss_transport_abi_compatible(version(), capabilities()) ||
        !(capabilities() & FSS_TRANSPORT_CAP_REMOTE_READY)) {
        fprintf(stderr, "[FAIL] transport ABI query/capability check\n");
        dlclose(dl);
        return 1;
    }
    create_fn_t  create  = (create_fn_t) dlsym(dl, "fss_transport_create");
    destroy_fn_t destroy = (destroy_fn_t)dlsym(dl, "fss_transport_destroy");
    set_edge_fn_t set_edge = (set_edge_fn_t)dlsym(dl, "fss_zenoh_set_edge");
    wait_ready_fn_t wait_ready = (wait_ready_fn_t)dlsym(dl, "fss_zenoh_wait_ready");
    if (!create || !destroy || !set_edge || !wait_ready) { fprintf(stderr, "[FAIL] dlsym: %s\n", dlerror()); return 1; }

    int rc;
    if      (strcmp(mode, "fanout") == 0) rc = run_fanout(create, destroy);
    else if (strcmp(mode, "fanin")  == 0) rc = run_fanin(create, destroy);
    else if (strcmp(mode, "ready-roundtrip") == 0)
        rc = run_ready_one(create, destroy, set_edge, wait_ready,
                           "fss/mp/ready-query", EP_READY_Q, 0);
    else if (strcmp(mode, "ready-pubsub") == 0)
        rc = run_ready_one(create, destroy, set_edge, wait_ready,
                           "fss/mp/ready-pub", EP_READY_P, 1);
    else if (strcmp(mode, "overlong-channel") == 0)
        rc = run_overlong_channel(create, destroy);
    else { fprintf(stderr, "unknown mode '%s'\n", mode); rc = 2; }

    dlclose(dl);
    return rc;
}
