/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/runtime/engine.h — the data-driven orchestrator engine (NON-NORMATIVE).
 *
 * The orchestrator holds a PLAN — nodes to bring to life and directed edges to
 * wire between them — and drives it into a running federation.  Placement is
 * DATA, not code: the engine groups nodes joined by in-proc edges into
 * processes, hosts the group holding the root node as an in-proc agent in its
 * own process, execs one agent per remaining group, and then drives EVERY node
 * uniformly over the private control protocol (construct / bind / server /
 * client / arm / stop).  There is no per-topology launcher and no privileged
 * direct-hosted node: a co-located node runs through the same agent-core as a
 * remote one.
 *
 * One engine replaces the three hand-coded launchers: the shape of the run
 * (which nodes co-locate, which edges cross, who is the zenoh star centre)
 * falls out of the plan's per-edge transport kinds.
 */
#ifndef FSS_RUNTIME_ENGINE_H
#define FSS_RUNTIME_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The transport that carries an edge.  In-proc edges keep their two nodes in
 * one process (they wire with an OP_BIND); shm/zenoh edges cross a process
 * boundary (a served channel at the receiver, an attached client at the sender). */
enum { FSS_XK_INPROC = 0, FSS_XK_SHM = 1, FSS_XK_ZENOH = 2 };

/* The transport NAME (fss_transport_name(), resolved to libfss_<name>.<ext>) an
 * agent loads for a cross edge of the given kind.  In-proc edges never cross a
 * process, so they name no channel transport (they wire with OP_BIND). */
static inline const char *fss_xk_name(int kind)
{
    return kind == FSS_XK_ZENOH ? "zenoh" : kind == FSS_XK_SHM ? "shm" : "";
}

#define FSS_PLAN_MAX_NODES 8
#define FSS_PLAN_MAX_EDGES 32

/* A node to bring to life: a name (unique within the plan) and a model locator
 * (the artefact the agent's model-loader constructs). */
typedef struct { const char *name; const char *model; } fss_node_spec;

/* A directed edge: from_node/from_ep SENDS to to_node/to_ep.  roundtrip = the
 * carrier's round-trip id (0 = one-way).  flags = FSS_CFLAG_* from the control
 * protocol (FSS_CFLAG_COUNT marks the delivery edge; the node it targets is the
 * counting sink whose completion is the run's verdict).  kind = FSS_XK_*. */
typedef struct {
    const char *from_node, *from_ep;
    const char *to_node,   *to_ep;
    uint32_t    roundtrip;
    uint32_t    flags;
    int         kind;
} fss_edge_spec;

/* The whole plan for one run. */
typedef struct {
    fss_node_spec nodes[FSS_PLAN_MAX_NODES]; int n_nodes;   /* list root-first, sink-last */
    fss_edge_spec edges[FSS_PLAN_MAX_EDGES]; int n_edges;

    const char *root_node;      /* hosted in-proc by the orchestrator; its fss/sim is the readiness latch */
    const char *ctrl_node;      /* the node whose host process is the zenoh star centre (role "listen") */

    const char *inproc_plugin;  /* construction transport (always) */
    const char *transport_dir;  /* dir where transport plugins (libfss_<name>.<ext>) live: data edges
                                 * AND the control back-channel resolve names here */
    const char *model_dir;      /* dir where MODEL plugins (lib<name>.<ext>) live: OP_CONSTRUCT
                                 * resolves a node's model NAME to a plugin file here */
    const char *zrv;            /* zenoh rendezvous endpoint (NULL if no zenoh edge) */
    const char *agent_binary;   /* the agent to exec per remote group (NULL if all-inproc) */

    int frames, period_us; /* frames must be positive */

    /* Protocol label used in the PASS/PERF verdict lines; e.g. "CAN".  The
     * caller sets this to identify the protocol under test.  When NULL the
     * engine falls back to "FSS". */
    const char *label;
} fss_plan_t;

/* Bring the plan up, run it, tear it down.  Prints the platform PASS/PERF
 * lines on success using the caller-supplied plan->label (defaults to "FSS").
 * Returns 0 on a clean PASS, non-zero otherwise. */
int fss_orchestrate(const fss_plan_t *plan);

#ifdef __cplusplus
}
#endif
#endif
