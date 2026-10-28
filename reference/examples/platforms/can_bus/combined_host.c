/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * combined_host.c — driver + oracle for the SINGLE-artefact CAN federation.
 *
 * Where platform_host.c constructs THREE independent artefacts (init, ctrl, dev)
 * — three SystemC kernels, three threads, cross-kernel doorbell + windowed
 * time-sync on every hop — this host constructs ONE artefact
 * (fss_can_combined_systemc) that builds all three CAN models inside ONE kernel.
 * The two internal edges (init/socket -> ctrl/tx, ctrl/can_tx -> dev/can_rx) are
 * wired as ordinary in-process FSS edges (local->send = peer->receive), so the
 * carriers still travel THROUGH FSS — but because sender and receiver share the
 * one kernel thread, each hits the SAME-THREAD FAST PATH in the binding's
 * endpoint_receive (a direct b_transport, no doorbell, no queue).  One kernel
 * means ONE clock: there is NO inter-model fss/time window negotiation and NO
 * inter-model fss/sim wave at all.
 *
 * This is the paper's "an artefact builds multiple models" made concrete, and the
 * measurement that isolates the FSS carrier cost from the federation/time-sync
 * cost: contrast the PERF line here with the three-kernel platform_host number.
 *
 *   PLATFORM CAN COMBINED PASS: acked=F/F
 *   PLATFORM CAN COMBINED PERF: frames=F ns_per_txn=N txns_per_sec=T
 *
 * CLI:  combined_host --combined <plugin.dylib> [--frames N] [--period-us U]
 */
#include "fss/transport_abi.h"
#include "fss/runtime/placement.h"
#include "fss/endpoint.h"
#include "fss/protocols/sim.h"        /* FSS_SIM_ID / FSS_SIM_META */
#include "fss/protocols/config.h"     /* FSS_CONFIG_* (config responder carrier) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FRAMES    16
#define DEFAULT_PERIOD_US 0
#define SIM_BOUND      0u
#define SIM_STOPPED    5u

/* ── small helpers (mirrors platform_host.c) ─────────────────────────────── */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static const char *opt_value(int argc, char **argv, const char *key)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], key) == 0) return argv[i + 1];
    return NULL;
}

/* Resolve a model NAME to its plugin file against a search dir (mirrors the
 * runtime's fss_model_plugin_path): lib<name>.dylib if present, else lib<name>.so.
 * A model is passed BY NAME, never as a path. */
static void model_path(const char *dir, const char *name, char *out, size_t n)
{
    if (dir && dir[0]) {
        snprintf(out, n, "%s/lib%s.dylib", dir, name);
        if (access(out, F_OK) == 0) return;
        snprintf(out, n, "%s/lib%s.so", dir, name);
    } else {
        snprintf(out, n, "lib%s.so", name);
    }
}

/* Data sink counter on the device's inbound: count each delivered can::frame. */
typedef struct { fss_endpoint_t *inner; volatile int *count; } counting_ctx;
static int counting_receive(void *self, fss_carrier_t *c)
{
    counting_ctx *cs = (counting_ctx *)self;
    int rc = (cs->inner && cs->inner->receive)
                 ? cs->inner->receive(cs->inner->self, c) : FSS_SEND_NO_RECEIVER;
    if (rc == FSS_SEND_OK) (*cs->count)++;
    return rc;
}

static int wait_for_count(volatile int *count, int target, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (*count >= target) return 1;
        msleep(5);
    }
    return *count >= target;
}

static void local_sim(fss_endpoint_t *sim, uint32_t code)
{
    if (!sim || !sim->receive) return;
    uint8_t b[4]; memcpy(b, &code, 4);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_SIM_ID; c.data_ptr = b; c.data_size = sizeof(b);
    c.meta_ptr = (void *)FSS_SIM_META; c.meta_size = FSS_SIM_META_LEN;
    sim->receive(sim->self, &c);
}

static void build_config_json(int frames, int period_us, char *out, size_t n)
{
    if (period_us > 0) snprintf(out, n, "{\"frames\":%d,\"period_us\":%d}", frames, period_us);
    else               snprintf(out, n, "{\"frames\":%d}", frames);
}

/* Single fss/config responder (as in platform_host): fill ParamValue = run JSON,
 * config::Status = Ok, in place; the in-proc pull returns the mutated carrier. */
static int config_responder(void *self, fss_carrier_t *c)
{
    const char *json = (const char *)self;
    if (!c || !c->data_ptr || c->data_size < 60 || !json) return FSS_SEND_NO_RECEIVER;
    uint8_t *base = (uint8_t *)c->data_ptr;
    size_t len = strlen(json);
    if (len > 39) len = 39;
    memset(base + 16, 0, 40); memcpy(base + 16, json, len);
    uint32_t ok = 0; memcpy(base + 56, &ok, sizeof(ok));
    return FSS_SEND_OK;
}

/* Install the responder as a node's fss/config send (pull-on-wire, fan-in). */
static void wire_local_config(fss_endpoint_t *cfg, const char *json)
{
    if (!cfg) return;
    cfg->send = config_responder;
    cfg->send_ctx = (void *)json;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *plugin    = opt_value(argc, argv, "--combined");   /* model NAME */
    const char *model_dir = opt_value(argc, argv, "--model-dir");  /* resolves it */
    const char *framess   = opt_value(argc, argv, "--frames");
    const char *perss     = opt_value(argc, argv, "--period-us");
    if (!plugin) {
        fprintf(stderr, "usage: %s --combined <name> --model-dir <dir> [--frames N] [--period-us U]\n", argv[0]);
        return 2;
    }
    int frames    = framess ? atoi(framess) : DEFAULT_FRAMES;
    int period_us = perss   ? atoi(perss)   : DEFAULT_PERIOD_US;

    int rc = 1;

    /* ONE artefact builds all three models in one kernel; resolve its NAME. */
    char plugin_path[512];
    model_path(model_dir, plugin, plugin_path, sizeof(plugin_path));
    fss_node_t *node = fss_place_construct("can", plugin_path);
    if (!node) { fprintf(stderr, "[FAIL] construct combined artefact\n"); return 1; }

    /* Namespaced endpoints (the binding maps '/' -> '.' onto the hub hierarchy). */
    fss_endpoint_t *init_socket = fss_place_endpoint(node, "init/socket");   /* outbound -> ctrl/tx */
    fss_endpoint_t *ctrl_tx     = fss_place_endpoint(node, "ctrl/tx");        /* inbound  (TLM reg)  */
    fss_endpoint_t *ctrl_can_tx = fss_place_endpoint(node, "ctrl/can_tx");    /* outbound -> dev     */
    fss_endpoint_t *dev_can_rx  = fss_place_endpoint(node, "dev/can_rx");     /* inbound  (CAN frame)*/
    fss_endpoint_t *sim         = fss_place_endpoint(node, "fss/sim");        /* single start barrier */
    if (!init_socket || !ctrl_tx || !ctrl_can_tx || !dev_can_rx) {
        fprintf(stderr, "[FAIL] resolve combined endpoints (init/socket=%p ctrl/tx=%p ctrl/can_tx=%p dev/can_rx=%p)\n",
                (void *)init_socket, (void *)ctrl_tx, (void *)ctrl_can_tx, (void *)dev_can_rx);
        goto done;
    }

    /* ONE hub-level fss/config feeds every sub-model (the hub's fss_configure
     * forwards frames/period to init + dev). */
    char cfg_json[64]; build_config_json(frames, period_us, cfg_json, sizeof(cfg_json));
    wire_local_config(fss_place_endpoint(node, "fss/config"), cfg_json);

    /* Internal edge 1 (hop1): init/socket -> ctrl/tx.  Direct in-proc wire; the
     * carrier flows through FSS and, being on the kernel thread, takes the
     * same-thread fast path. */
    init_socket->send = ctrl_tx->receive; init_socket->send_ctx = ctrl_tx->self;

    /* Internal edge 2 (hop2): ctrl/can_tx -> dev/can_rx, via the frame counter. */
    volatile int dev_count = 0;
    counting_ctx dev_cs = { dev_can_rx, &dev_count };
    ctrl_can_tx->send = counting_receive; ctrl_can_tx->send_ctx = &dev_cs;

    /* Arm the single start barrier.  No fss/time, no downstream fss/sim peers:
     * one kernel, one clock — the three models run cooperatively.  The device's
     * heartbeat sc_stop()s when it has received `frames`, ending the kernel. */
    local_sim(sim, SIM_BOUND);

    /* Stopwatch over the delivery window. */
    double t0 = now_ns();
    int ok = wait_for_count(&dev_count, frames, 30000);
    double t1 = now_ns();

    if (ok) {
        double ns = t1 - t0;
        double ns_per_txn = ns / (double)frames;
        double tps = ns > 0.0 ? (double)frames * 1e9 / ns : 0.0;
        printf("PLATFORM CAN COMBINED PASS: acked=%d/%d\n", frames, frames);
        printf("PLATFORM CAN COMBINED PERF: frames=%d ns_per_txn=%.1f txns_per_sec=%.0f\n",
               frames, ns_per_txn, tps);
        rc = 0;
    } else {
        fprintf(stderr, "[FAIL] combined: delivered=%d/%d\n", dev_count, frames);
    }

    local_sim(sim, SIM_STOPPED);

done:
    fss_place_destroy(node);
    return rc;
}
