/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * matrix_host.c — the CAN platform driven by the data-driven orchestrator.
 *
 * This replaces the three hand-coded launchers (and the privileged in-proc
 * path) of the old agent_host.c with a thin main: parse the CLI, build the CAN
 * plan (can_plan.c), and hand it to the generic engine (fss_orchestrate).  The
 * engine derives placement from the plan's per-hop transport kinds, hosts the
 * init group in-proc and execs the rest, and drives every node uniformly over
 * the control protocol.  The reference platform_host.c stays the untouched
 * green oracle.
 *
 * CLI (drop-in with platform_host.c):
 *   --hop1 <kind>:<plugin> --hop2 <kind>:<plugin>
 *   --initiator <p> --controller <p> --device <p> [--frames N] [--period-us U]
 *   [--inproc <plugin>] [--shm <plugin>] [--agent-c <binary>]
 * where the construction transport (--inproc) and the control back-channel
 * (--shm) are intrinsic to an agent, independent of the data hops; --agent-c is
 * the agent binary the engine execs for any cross-process group.
 */
#include "can_plan.h"
#include "fss/runtime/engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_FRAMES    16
#define DEFAULT_PERIOD_US 0

enum { K_INPROC, K_SHM, K_ZENOH };

static int kind_of(const char *k)
{
    if (!strcmp(k, "inproc")) return K_INPROC;
    if (!strcmp(k, "shm"))    return K_SHM;
    if (!strcmp(k, "zenoh"))  return K_ZENOH;
    return -1;
}
static int xk_of(int k)   /* CLI kind -> engine transport kind */
{
    return k == K_SHM ? FSS_XK_SHM : k == K_ZENOH ? FSS_XK_ZENOH : FSS_XK_INPROC;
}
static const char *opt_value(int argc, char **argv, const char *key)
{
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], key) == 0) return argv[i + 1];
    return NULL;
}
static int parse_hop(const char *s, int *kind, const char **plugin)
{
    if (!s) return 0;
    const char *colon = strchr(s, ':');
    if (!colon) return 0;
    static char kbuf[16];
    size_t klen = (size_t)(colon - s);
    if (klen == 0 || klen >= sizeof(kbuf)) return 0;
    memcpy(kbuf, s, klen); kbuf[klen] = '\0';
    *kind = kind_of(kbuf);
    *plugin = colon + 1;
    return *kind >= 0;
}

int main(int argc, char *argv[])
{
    const char *initiator  = opt_value(argc, argv, "--initiator");   /* model NAME */
    const char *controller = opt_value(argc, argv, "--controller");  /* model NAME */
    const char *device     = opt_value(argc, argv, "--device");      /* model NAME */
    const char *device2    = opt_value(argc, argv, "--device2");     /* optional 2nd device (fan-out); NULL = single */
    const char *cfg_edge   = opt_value(argc, argv, "--config-edge"); /* optional config FAN-IN transport; absent = local config */
    const char *model_dir  = opt_value(argc, argv, "--model-dir");   /* resolves the names above */
    const char *framess    = opt_value(argc, argv, "--frames");
    const char *periodss   = opt_value(argc, argv, "--period-us");
    const char *inproc_cli = opt_value(argc, argv, "--inproc");
    const char *shm_cli    = opt_value(argc, argv, "--shm");
    const char *agent_c    = opt_value(argc, argv, "--agent-c");
    int hop1_kind, hop2_kind;
    const char *hop1_plugin, *hop2_plugin;
    int frames    = framess  ? atoi(framess)  : DEFAULT_FRAMES;
    int period_us = periodss ? atoi(periodss) : DEFAULT_PERIOD_US;

    if (!initiator || !controller || !device ||
        !parse_hop(opt_value(argc, argv, "--hop1"), &hop1_kind, &hop1_plugin) ||
        !parse_hop(opt_value(argc, argv, "--hop2"), &hop2_kind, &hop2_plugin)) {
        fprintf(stderr, "usage: %s --hop1 <kind>:<plugin> --hop2 <kind>:<plugin> "
                        "--initiator <p> --controller <p> --device <p> "
                        "[--inproc <plugin>] [--shm <plugin>] [--agent-c <binary>] "
                        "[--frames N] [--period-us U]\n", argv[0]);
        return 2;
    }

    /* The construction transport is ALWAYS inproc (construct-here), passed by
     * path.  Every channel transport (data edges AND the control back-channel,
     * shm for a local fork) is resolved BY NAME from the transport dir, so no
     * channel-plugin path is passed here. */
    const char *inproc_plugin = inproc_cli ? inproc_cli
                              : (hop1_kind == K_INPROC) ? hop1_plugin
                              : (hop2_kind == K_INPROC) ? hop2_plugin : NULL;
    if (!inproc_plugin) { fprintf(stderr, "[FAIL] matrix_host: need --inproc <plugin> (construction)\n"); return 2; }

    /* Channel transports are resolved by NAME (control::Transport / breadcrumb)
     * from a single directory (libfss_<name>.<ext>).  Take --transport-dir if
     * given, else derive it from any passed plugin path's dirname (all plugins
     * share one output dir); --shm is accepted only as a dir hint. */
    static char xdir[512];
    const char *transport_dir = opt_value(argc, argv, "--transport-dir");
    if (!transport_dir) {
        const char *any = (hop1_plugin && *hop1_plugin) ? hop1_plugin
                        : (hop2_plugin && *hop2_plugin) ? hop2_plugin
                        : shm_cli ? shm_cli : inproc_plugin;
        snprintf(xdir, sizeof(xdir), "%s", any ? any : "");
        char *slash = strrchr(xdir, '/');
        if (slash) *slash = '\0'; else xdir[0] = '\0';
        transport_dir = xdir;
    }

    /* One zenoh rendezvous endpoint per run, pid-keyed (the ctrl-hosting process
     * listens here, the others connect).  Unused for shm/inproc-only topologies. */
    char zrv[64]; snprintf(zrv, sizeof(zrv), "tcp/127.0.0.1:%d", 21000 + (int)((long)getpid() % 40000));

    /* Any cross-process topology execs an agent (the model lives in the agent,
     * spawned by the engine); the control back-channel transport is resolved by
     * name from the transport dir. */
    if (hop1_kind != K_INPROC || hop2_kind != K_INPROC) {
        if (!transport_dir || !transport_dir[0]) { fprintf(stderr, "[FAIL] matrix_host: cross-process needs --transport-dir (or a plugin path to derive it)\n"); return 2; }
        if (!agent_c)    { fprintf(stderr, "[FAIL] matrix_host: cross-process needs --agent-c <binary>\n"); return 2; }
    }

    /* --config-edge <kind> turns on config FAN-IN (ctrl+dev PULL config from init
     * over that transport); absent = today's LOCAL config (config_kind < 0). */
    int config_kind = -1;
    if (cfg_edge) { int k = kind_of(cfg_edge); if (k < 0) { fprintf(stderr, "[FAIL] matrix_host: bad --config-edge %s\n", cfg_edge); return 2; } config_kind = xk_of(k); }

    fss_plan_t plan;
    can_build_plan(&plan, xk_of(hop1_kind), xk_of(hop2_kind),
                   inproc_plugin, transport_dir, model_dir, zrv, agent_c,
                   initiator, controller, device, device2, config_kind, frames, period_us);
    return fss_orchestrate(&plan);
}
