/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/runtime/agent_core.h — the generic agent (NON-NORMATIVE ring-3 runtime).
 *
 * An AGENT is the loadable unit that brings a model to life in a process and
 * wires it into a federation on instruction.  It is generic and
 * artefact-agnostic: it constructs its nodes on ONE local construction
 * transport (in-proc, construct-here), attaches per-edge channels on
 * shm/zenoh, and obeys a private control protocol from the orchestrator.
 *
 * The ONE per-language seam is the MODEL LOADER: how a node is actually
 * brought into being (a C/SystemC/Rust cdylib constructed via the artefact
 * ABI; a .fmu dlopened by an FMI wrapper; a .py loaded into an embedded
 * CPython).  agent-core knows only this vtable; everything else — control
 * interpretation, channel attach, arming, teardown — is shared.  So "an agent
 * per language, generic core, much shared" is literal: one agent-core linked
 * against one model-loader front per language.
 */
#ifndef FSS_RUNTIME_AGENT_CORE_H
#define FSS_RUNTIME_AGENT_CORE_H

#include "fss/transport_abi.h"
#include "fss/runtime/control_protocol.h"   /* fss_ctrl_instr (in-proc drive) */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── the model-loader vtable (the per-language seam) ─────────────────────────
 * The loader brings a node into being and resolves its endpoints.  Construction
 * is handled by the placement facility (fss/runtime/placement.h), not by the
 * transport — the transport vtable no longer carries open/close/get_endpoint.
 * A cdylib loader delegates to fss_place_construct / fss_place_endpoint /
 * fss_place_destroy; a Python/FMI loader constructs differently but presents
 * the same three verbs.
 *   - construct(node, locator): bring the node to life, return its handle.
 *   - get_endpoint(node, ep):   resolve one of the node's endpoints.
 *   - destroy(node):            tear the node down (sits exactly at placement destroy). */
typedef struct fss_model_loader {
    void *self;
    fss_node_t     *(*construct)   (void *self,
                                    const char *node, const char *model_locator);
    fss_endpoint_t *(*get_endpoint)(void *self,
                                    fss_node_t *node, const char *ep);
    void            (*destroy)     (void *self,
                                    fss_node_t *node);
} fss_model_loader_t;

/* The agent's node registry: name -> constructed node handle. */
#define FSS_MAX_NODES 4
typedef struct { const char *name; fss_node_t *node; } fss_reg_entry;
fss_node_t *fss_reg_find(fss_reg_entry *reg, int n, const char *name);

/* The uniform agent entry point.  Born with a directory in which to resolve
 * data-edge transport plugins by name (libfss_<name>.<ext>), a home breadcrumb
 * (the control transport + phone-home locator), and a completion bound (frames);
 * plus this process's zenoh rendezvous role/endpoint for any zenoh edge.
 * Data-edge transports load LAZILY: an OP_SERVER/OP_CLIENT names one
 * (control::Transport) and the agent dlopens + confirms it (fss_transport_name)
 * on first use.  `frames` must be positive. Construction of every node goes through `loader` (via the
 * placement facility).  Returns 0 on success.
 * `inproc_plugin` is accepted but unused (kept for launcher compatibility). */
int fss_agent_run(const fss_model_loader_t *loader,
                  const char *inproc_plugin, const char *xport_dir,
                  const char *model_dir,
                  const char *zrole, const char *zrv,
                   const char *breadcrumb, int frames, int period_us,
                    int status_fd, int release_fd, uint64_t barrier_nonce);

/* ── in-proc agent lifecycle (the orchestrator's own process-0 agent) ─────────
 * The SAME agent, driven WITHOUT a home control channel: the orchestrator hosts
 * it in its own process and feeds it instructions by direct call
 * (fss_agent_ctrl) rather than over shm.  This is what makes node handling
 * uniform — a co-located node runs through exactly the same agent-core as a
 * remote one, so there is no privileged "direct-hosted" path.  Data-edge
 * transports load lazily by name from `xport_dir` (an all-inproc process names
 * no cross transport and opens none, wiring every edge with OP_BIND).
 *
 * Lifecycle: open -> {ctrl}* (construct/bind/server/client) -> arm -> [the
 * orchestrator waits for the federation] -> stop_and_close.  fss_agent_endpoint
 * exposes a hosted node's endpoint so the orchestrator can attach its readiness
 * latch to the root node's fss/sim.
 * `inproc_plugin` is accepted but unused (kept for launcher compatibility). */
typedef struct fss_agent fss_agent_t;
int fss_topology_transport_init(fss_agent_t *A, const char *breadcrumb_json); /* standard breadcrumb bootstrap */

fss_agent_t    *fss_agent_open(const fss_model_loader_t *loader,
                               const char *inproc_plugin, const char *xport_dir,
                               const char *model_dir,
                               const char *zrole, const char *zrv,
                               int frames, int period_us);
int             fss_agent_ctrl(fss_agent_t *A, fss_ctrl_instr *in);  /* interpret one instruction */
int             fss_agent_run_plan(fss_agent_t *A, const char *plan_json); /* run a JSON plan (R5): all steps via agent_do_instr */
fss_endpoint_t *fss_agent_endpoint(fss_agent_t *A, const char *node, const char *ep);
void            fss_agent_prepare(fss_agent_t *A);        /* install local config responders */
/* Wait for all transport peers that this agent attached.  This is separate from
 * Bound so a network edge is known ready before any model can send on it. */
int             fss_agent_transport_ready(fss_agent_t *A);
void            fss_agent_bound(fss_agent_t *A);          /* send SIM_BOUND, sink-first */
void            fss_agent_arm(fss_agent_t *A);            /* compatibility: prepare + bound */
int             fss_agent_is_counting(fss_agent_t *A);    /* hosts the counting data sink? */
int             fss_agent_wait_delivery(fss_agent_t *A);  /* block until frames land / OP_STOP; 1 = ok */
void            fss_agent_stop_and_close(fss_agent_t *A); /* SIM_STOPPED + teardown + free */

#ifdef __cplusplus
}
#endif
#endif
