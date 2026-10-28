/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_plan.c — the CAN platform expressed as a plan (see can_plan.h).
 *
 *   init ──socket/tx (TLM)──▶ ctrl ──can_tx/can_rx (CAN)──▶ dev
 *
 * plus a bidirectional fss/time edge on each hop and an upward fss/sim edge, so
 * a downstream node's readiness/teardown propagates up to init.  The data edge
 * into dev carries FLAG_COUNT: dev is the sink whose delivery is the verdict.
 * Edges are listed downstream-first (hop2 before hop1); wiring happens before
 * arming, so the order is only cosmetic.  Every edge's carrier round-trip id is
 * set unconditionally — an in-proc edge binds by identity and ignores it, a
 * cross edge serves/attaches a round-trip channel with it.
 */
#include "can_plan.h"
#include "fss/runtime/control_protocol.h"    /* FSS_CFLAG_COUNT */
#include "fss/known_carriers/tlm_id.h"       /* FSS_TLM_ROUND_TRIP  (hop1 data) */
#include "fss/known_carriers/can_id.h"       /* FSS_CAN_ROUND_TRIP  (hop2 data) */
#include "fss/protocols/time_sync.h"    /* FSS_TIME_ROUND_TRIP */
#include "fss/protocols/sim.h"          /* FSS_SIM_ROUND_TRIP  */
#include "fss/protocols/config.h"       /* FSS_CONFIG_ROUND_TRIP (config fan-in) */

#include <string.h>

static void add_edge(fss_plan_t *p, const char *fn, const char *fe,
                     const char *tn, const char *te, uint32_t rt, uint32_t flags, int kind)
{
    fss_edge_spec *e = &p->edges[p->n_edges++];
    e->from_node = fn; e->from_ep = fe; e->to_node = tn; e->to_ep = te;
    e->roundtrip = rt; e->flags = flags; e->kind = kind;
}

int can_build_plan(fss_plan_t *plan,
                   int hop1_kind, int hop2_kind,
                   const char *inproc_plugin,
                   const char *transport_dir, const char *model_dir,
                   const char *zrv, const char *agent_binary,
                   const char *initiator, const char *controller, const char *device,
                   const char *device2, int config_kind,
                   int frames, int period_us)
{
    memset(plan, 0, sizeof(*plan));

    plan->nodes[plan->n_nodes++] = (fss_node_spec){ "init", initiator };   /* depth 0 (root) */
    plan->nodes[plan->n_nodes++] = (fss_node_spec){ "ctrl", controller };  /* depth 1 */
    plan->nodes[plan->n_nodes++] = (fss_node_spec){ "dev",  device };      /* depth 2 (sink) */
    if (device2) plan->nodes[plan->n_nodes++] = (fss_node_spec){ "dev2", device2 };  /* fan-out sink */

    plan->root_node = "init";   /* hosted in-proc; its fss/sim is the readiness latch */
    plan->ctrl_node = "ctrl";   /* its host process is the zenoh star centre */

    plan->inproc_plugin = inproc_plugin;
    plan->transport_dir = transport_dir;
    plan->model_dir     = model_dir;
    plan->zrv           = zrv;
    plan->agent_binary  = agent_binary;
    plan->frames        = frames;
    plan->period_us     = period_us;
    plan->label         = "CAN";

    /* hop2 (ctrl <-> dev): data (the counted delivery edge), time both ways, sim up. */
    add_edge(plan, "ctrl", "can_tx",       "dev",  "can_rx",        FSS_CAN_ROUND_TRIP,  FSS_CFLAG_COUNT, hop2_kind);
    add_edge(plan, "ctrl", "fss/time/dev", "dev",  "fss/time",      FSS_TIME_ROUND_TRIP, 0,               hop2_kind);
    add_edge(plan, "dev",  "fss/time",      "ctrl", "fss/time/dev", FSS_TIME_ROUND_TRIP, 0,               hop2_kind);
    add_edge(plan, "dev",  "fss/sim",       "ctrl", "fss/sim/dev",  FSS_SIM_ROUND_TRIP,  0,               hop2_kind);

    /* DATA-PLANE FAN-OUT: a 2nd device fanned out from the SAME controller source
     * ctrl/can_tx over hop2, mirroring the dev edges (its own fss/time peer + sim-up
     * sink on the controller — ctrl now gates the run's start on BOTH devices being
     * up, and cascades teardown when they detach).  VERDICT CHOICE: BOTH data edges
     * carry FLAG_COUNT so BOTH devices are counted, and the engine requires EVERY
     * count-sink group to complete.  Chosen over "one count edge + dev2 self-check"
     * because it makes the fan-out non-vacuous UNIFORMLY across all three transports
     * with no per-transport special-casing: fanned into separate processes
     * (shm/zenoh) each device's agent exit code is its own delivery verdict; fanned
     * within one process (in-proc) that agent counts frames*n_counted.  Either way
     * a device that silently received nothing fails the run. */
    if (device2) {
        add_edge(plan, "ctrl", "can_tx",        "dev2", "can_rx",         FSS_CAN_ROUND_TRIP,  FSS_CFLAG_COUNT, hop2_kind);
        add_edge(plan, "ctrl", "fss/time/dev2", "dev2", "fss/time",       FSS_TIME_ROUND_TRIP, 0,               hop2_kind);
        add_edge(plan, "dev2", "fss/time",       "ctrl", "fss/time/dev2", FSS_TIME_ROUND_TRIP, 0,               hop2_kind);
        add_edge(plan, "dev2", "fss/sim",        "ctrl", "fss/sim/dev2",  FSS_SIM_ROUND_TRIP,  0,               hop2_kind);
    }

    /* hop1 (init <-> ctrl): data, time both ways, sim up. */
    add_edge(plan, "init", "socket",    "ctrl", "tx",           FSS_TLM_ROUND_TRIP,  0, hop1_kind);
    add_edge(plan, "init", "fss/time",  "ctrl", "fss/time",     FSS_TIME_ROUND_TRIP, 0, hop1_kind);
    add_edge(plan, "ctrl", "fss/time",  "init", "fss/time",     FSS_TIME_ROUND_TRIP, 0, hop1_kind);
    add_edge(plan, "ctrl", "fss/sim",   "init", "fss/sim/ctrl", FSS_SIM_ROUND_TRIP,  0, hop1_kind);

    /* CONFIG FAN-IN (N pullers -> 1 responder, round-trip) — the MIRROR of the
     * data-plane fan-out above.  Instead of each agent serving the run config to
     * its own nodes LOCALLY, ctrl and dev PULL their config from the ROOT node init
     * (always hosted in-proc in p0) over a round-trip fss/config edge.  Two
     * initiators, one shared target (init/fss/config) => N->1 fan-in, keyed on the
     * target (chan_ident: a round-trip edge keys on its TARGET).  init keeps its
     * LOCAL config — it is the source/responder.  Edges are FSS_CFLAG_OPTIONAL so a
     * puller whose model lacks fss/config is skipped (status ok).  BOTH pullers use
     * the SAME config transport (config_kind): dev reaches init (p0) DIRECTLY — a
     * host-local shm segment or the shared zenoh key — NOT routed through ctrl, so
     * no two-hop path is needed and the "dev can't reach p0" concern does not arise.
     * Default (config_kind < 0): NO config edges, so the 576 stays byte-identical. */
    if (config_kind >= 0) {
        add_edge(plan, "ctrl", "fss/config", "init", "fss/config", FSS_CONFIG_ROUND_TRIP, FSS_CFLAG_OPTIONAL, config_kind);
        add_edge(plan, "dev",  "fss/config", "init", "fss/config", FSS_CONFIG_ROUND_TRIP, FSS_CFLAG_OPTIONAL, config_kind);
    }

    return 0;
}
