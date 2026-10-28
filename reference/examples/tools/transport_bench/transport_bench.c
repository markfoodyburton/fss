/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * transport_bench.c — hot-path send latency micro-benchmark for FSS transports.
 *
 * Measures ns/send for a 16-byte flat carrier over two transports:
 *
 *   inproc mode:
 *     - create(NULL), open fss_minimal_echo artefact, bind to "echo/in".
 *     - Time N ep.send() calls in a tight loop.
 *     - Each call resolves to an in-process function pointer: no IPC, no syscall.
 *     - The echo artefact increments data[0] so the compiler cannot dead-strip
 *       the call.
 *
 *   shm mode:
 *     - fork(); child = create("") + channel("bench", owner=1) + attach TARGET
 *       (counting sink that increments a counter until N frames received, then
 *       signals done via a pipe).  Parent = create("") + channel("bench",
 *       owner=0) + attach INITIATOR + time N ep.send() calls.  The SPSC ring
 *       (256 slots) uses sem_wait/sem_post as doorbell;
 *       each send crosses the process boundary via two semaphore ops PLUS one
 *       memcpy of the data region into the ring slot.
 *
 *   NOTE — the shm path copies the payload into the ring slot (shm.c
 *   shm_write_slot).  This is by design, not a shortcut: the copy is negligible
 *   against the doorbell (a 16-byte memcpy is ~4 ns; one semaphore wakeup is
 *   ~2.4 us), so a zero-copy shared arena would buy nothing measurable here —
 *   see README.md "Why copy".  The shm number below is dominated by the two
 *   semaphore ops.  The inproc path IS zero-copy (a by-reference call within
 *   one address space).
 *
 * Usage:
 *   transport_bench inproc <inproc_module> <echo_module> [N]
 *   transport_bench shm    <shm_module>                  [N]
 *
 * N defaults to 200000.  Print ns/send with two back-to-back runs for stability.
 */

#include "fss/transport_abi.h"
#include "fss/endpoint.h"
#include "fss/runtime/placement.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* ── timing ─────────────────────────────────────────────────────────────── */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ── transport plugin helpers ────────────────────────────────────────────── */

typedef fss_transport_t *(*create_fn_t)(const char *);
typedef void             (*destroy_fn_t)(fss_transport_t *);
typedef void             (*set_sink_fn_t)(fss_transport_t *, fss_endpoint_t *);

static int check_transport_abi(void *dl)
{
    fss_transport_abi_version_fn_t version =
        (fss_transport_abi_version_fn_t)dlsym(dl, "fss_transport_abi_version");
    fss_transport_capabilities_fn_t capabilities =
        (fss_transport_capabilities_fn_t)dlsym(dl, "fss_transport_capabilities");
    return version && capabilities &&
           fss_transport_abi_compatible(version(), capabilities());
}

/* ── build a flat 16-byte carrier ────────────────────────────────────────── */

static void make_carrier(fss_carrier_t *c, uint8_t *buf)
{
    memset(buf, 0xAB, 16);
    memset(c, 0, sizeof(*c));
    c->id        = 0;
    c->data_ptr  = buf;
    c->data_size = 16;
}

/* ── sleep helper ────────────────────────────────────────────────────────── */

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── inproc bench ────────────────────────────────────────────────────────── */

static int bench_inproc(const char *inproc_mod, const char *echo_mod, long N)
{
    void *dl = dlopen(inproc_mod, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { fprintf(stderr, "[FAIL] dlopen inproc: %s\n", dlerror()); return 1; }
    if (!check_transport_abi(dl)) {
        fprintf(stderr, "[FAIL] inproc ABI query/capability check\n");
        dlclose(dl); return 1;
    }

    create_fn_t  create_fn  = (create_fn_t) dlsym(dl, "fss_transport_create");
    destroy_fn_t destroy_fn = (destroy_fn_t)dlsym(dl, "fss_transport_destroy");
    if (!create_fn || !destroy_fn) {
        fprintf(stderr, "[FAIL] dlsym inproc factory: %s\n", dlerror());
        dlclose(dl); return 1;
    }

    fss_transport_t *t = create_fn(NULL);
    if (!t) { fprintf(stderr, "[FAIL] inproc create\n"); dlclose(dl); return 1; }

    /* open the echo artefact as node "echo" via placement */
    fss_node_t *node = fss_place_construct("echo", echo_mod);
    if (!node) { fprintf(stderr, "[FAIL] open echo node\n"); destroy_fn(t); dlclose(dl); return 1; }

    fss_endpoint_t ep = {0};
    /* Direct wire: resolve echo/in and install send */
    fss_endpoint_t *peer = fss_place_resolve("echo/in");
    if (!peer) {
        fprintf(stderr, "[FAIL] bind echo/in\n");
        fss_place_destroy(node); destroy_fn(t); dlclose(dl); return 1;
    }
    ep.send = peer->receive; ep.send_ctx = peer->self;

    uint8_t buf[16];
    fss_carrier_t c;
    make_carrier(&c, buf);

    printf("inproc: N=%ld\n", N);
    for (int run = 0; run < 2; run++) {
        buf[0] = 0;   /* reset so echo increments don't wrap in a way that matters */
        double t0 = now_ns();
        for (long i = 0; i < N; i++)
            ep.send(ep.send_ctx, &c);
        double t1 = now_ns();
        printf("  run %d: %.2f ns/send  (data[0]=%u after %ld sends)\n",
               run + 1, (t1 - t0) / (double)N, (unsigned)buf[0], N);
    }

    fss_place_destroy(node);
    destroy_fn(t);
    dlclose(dl);
    return 0;
}

/* ── shm bench (child = receiver/creator) ────────────────────────────────── */

/* Shared state between the sink callback and the wait loop (child-process local
 * memory — no cross-process sharing needed here). */
typedef struct {
    volatile long count;
    long          target;
    int           done_pipe_wr;   /* write end: child writes 1 byte when done */
} shm_rx_state;

static int shm_sink_receive(void *self, fss_carrier_t *c)
{
    (void)c;
    shm_rx_state *rx = (shm_rx_state *)self;
    rx->count++;
    if (rx->count >= rx->target) {
        /* Signal done: write one byte to pipe so parent can read */
        char b = 1;
        write(rx->done_pipe_wr, &b, 1);
    }
    return FSS_SEND_OK;
}

#define SHM_NRUNS 2

static int bench_shm(const char *shm_mod, long N)
{
    /* Total frames the child must receive across all runs. */
    long total = (long)SHM_NRUNS * N;

    /* pipe: child writes 1 byte after all runs' frames received; parent reads */
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* ===== CHILD = RECEIVER/CREATOR ===== */
        close(pipefd[0]);   /* child doesn't read */

        void *dl = dlopen(shm_mod, RTLD_NOW | RTLD_LOCAL);
        if (!dl) { fprintf(stderr, "[FAIL] child dlopen shm: %s\n", dlerror()); _exit(1); }
        if (!check_transport_abi(dl)) {
            fprintf(stderr, "[FAIL] child shm ABI query/capability check\n");
            dlclose(dl); _exit(1);
        }

        create_fn_t   create_fn  = (create_fn_t)  dlsym(dl, "fss_transport_create");
        destroy_fn_t  destroy_fn = (destroy_fn_t) dlsym(dl, "fss_transport_destroy");
        set_sink_fn_t set_sink   = (set_sink_fn_t)dlsym(dl, "fss_shm_set_sink");
        if (!create_fn || !destroy_fn || !set_sink) {
            fprintf(stderr, "[FAIL] child dlsym: %s\n", dlerror()); _exit(1);
        }

        fss_transport_t *t = create_fn("");
        if (!t) { fprintf(stderr, "[FAIL] child create transport\n"); _exit(1); }

        /* Provision the one-way channel (owner), then attach the counting sink. */
        fss_channel_t *ch = t->channel(t->self, "bench", 0 /*round_trip*/, 1 /*owner*/, NULL, 0);
        if (!ch) { fprintf(stderr, "[FAIL] child channel bench\n"); _exit(1); }

        /* The child must drain ALL runs' frames before signalling done. */
        shm_rx_state rx = {0, total, pipefd[1]};
        fss_endpoint_t sink = {0};
        sink.self    = &rx;
        sink.receive = shm_sink_receive;
        t->attach(t->self, ch, &sink, FSS_ROLE_TARGET);
        (void)set_sink;

        /* Wait with generous timeout (60 s) for all total frames. */
        for (int i = 0; i < 60000 && rx.count < total; i++)
            msleep(1);

        destroy_fn(t);
        dlclose(dl);
        close(pipefd[1]);
        _exit(rx.count >= total ? 0 : 1);
    }

    /* ===== PARENT = SENDER ===== */
    close(pipefd[1]);   /* parent doesn't write */

    void *dl = dlopen(shm_mod, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { fprintf(stderr, "[FAIL] parent dlopen shm: %s\n", dlerror()); return 1; }
    if (!check_transport_abi(dl)) {
        fprintf(stderr, "[FAIL] parent shm ABI query/capability check\n");
        dlclose(dl); return 1;
    }

    create_fn_t  create_fn  = (create_fn_t) dlsym(dl, "fss_transport_create");
    destroy_fn_t destroy_fn = (destroy_fn_t)dlsym(dl, "fss_transport_destroy");
    if (!create_fn || !destroy_fn) {
        fprintf(stderr, "[FAIL] parent dlsym: %s\n", dlerror()); return 1;
    }

    /* Create the session, then attach to the channel, retrying while the child
     * creates the segment. */
    fss_transport_t *t = create_fn("");
    if (!t) {
        fprintf(stderr, "[FAIL] parent create transport\n");
        dlclose(dl);
        int ws = 0; waitpid(pid, &ws, 0);
        return 1;
    }
    fss_channel_t *ch = NULL;
    for (int i = 0; i < 400 && !ch; i++) {
        ch = t->channel(t->self, "bench", 0 /*round_trip*/, 0 /*attacher*/, NULL, 0);
        if (!ch) msleep(5);
    }
    if (!ch) {
        fprintf(stderr, "[FAIL] parent attach channel (child never created segment)\n");
        destroy_fn(t); dlclose(dl);
        int ws = 0; waitpid(pid, &ws, 0);
        return 1;
    }

    fss_endpoint_t ep = {0};
    if (t->attach(t->self, ch, &ep, FSS_ROLE_INITIATOR) != FSS_SEND_OK || !ep.send) {
        fprintf(stderr, "[FAIL] parent bind bench/bench_rx\n");
        destroy_fn(t); dlclose(dl);
        int ws = 0; waitpid(pid, &ws, 0);
        return 1;
    }

    uint8_t buf[16];
    fss_carrier_t c;
    make_carrier(&c, buf);

    printf("shm:    N=%ld (2 runs)\n", N);
    int rc = 0;
    for (int run = 0; run < SHM_NRUNS; run++) {
        double t0 = now_ns();
        for (long i = 0; i < N; i++) {
            if (ep.send(ep.send_ctx, &c) != FSS_SEND_OK) {
                fprintf(stderr, "[FAIL] shm send returned non-OK at i=%ld\n", i);
                rc = 1; break;
            }
        }
        double t1 = now_ns();
        if (!rc)
            printf("  run %d: %.2f ns/send\n", run + 1, (t1 - t0) / (double)N);
    }

    /* Wait for child to drain all runs' frames (pipe signals when count >= total). */
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(pipefd[0], &fds);
        struct timeval tv = {60, 0};
        select(pipefd[0] + 1, &fds, NULL, NULL, &tv);
        char b = 0;
        read(pipefd[0], &b, 1);
    }
    close(pipefd[0]);

    int ws = 0;
    waitpid(pid, &ws, 0);
    int child_ok = WIFEXITED(ws) && WEXITSTATUS(ws) == 0;

    destroy_fn(t);
    dlclose(dl);

    if (!child_ok)
        fprintf(stderr, "[WARN] shm child exited with status %d\n",
                WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
    return rc;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
            "usage:\n"
            "  %s inproc <inproc_module> <echo_module> [N]\n"
            "  %s shm    <shm_module>                  [N]\n",
            argv[0], argv[0]);
        return 2;
    }

    const char *mode = argv[1];
    long N = 200000;

    if (strcmp(mode, "inproc") == 0) {
        if (argc < 4) {
            fprintf(stderr, "inproc mode needs <inproc_module> <echo_module>\n");
            return 2;
        }
        if (argc >= 5) N = atol(argv[4]);
        return bench_inproc(argv[2], argv[3], N);
    } else if (strcmp(mode, "shm") == 0) {
        if (argc < 3) {
            fprintf(stderr, "shm mode needs <shm_module>\n");
            return 2;
        }
        if (argc >= 4) N = atol(argv[3]);
        return bench_shm(argv[2], N);
    } else {
        fprintf(stderr, "unknown mode '%s'; use inproc or shm\n", mode);
        return 2;
    }
}
