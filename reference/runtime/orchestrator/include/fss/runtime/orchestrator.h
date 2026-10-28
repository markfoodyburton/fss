/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/runtime/orchestrator.h — ring-3 orchestrator scaffolding (NON-NORMATIVE).
 *
 * The shared machinery an orchestrator uses to drive agents into a federation,
 * plus the low-level transport plumbing both the orchestrator and the agents
 * lean on.  None of this is FSS-standard: it is a reference runtime.  Two
 * groups live here:
 *
 *   1. Transport plumbing (shared with the agent): load a transport plugin by
 *      the ABI queries and standard factory symbols, retry create against a not-yet-ready peer,
 *      and build the kind-aware channel config / bind-locator strings (shm vs
 *      zenoh).  The agent constructs on inproc and channels on shm/zenoh; the
 *      orchestrator builds its own half of each cross edge.
 *
 *   2. Orchestrator / harness instruments (legitimately NOT artefact
 *      endpoints): the federation-status latch (the orchestrator observing the
 *      standard sim-status protocol — ready/done — which is its job), the
 *      delivery counter (installed by whichever agent hosts the sink, since
 *      only that process sees the data wire; language-agnostic, so it does not
 *      depend on every artefact forwarding STOPPED), the config responder (the
 *      runtime supplying a node's config, resolving the reply fields by walking
 *      the carrier's own meta — no hard-coded offsets), the sim arming
 *      primitive, and the control drivers (ctrl_send + the kind-aware
 *      orch_server / orch_client channel builders).
 *
 * The agent uses the plumbing (group 1) to build its own channels; the engine
 * (engine.c) uses the instruments (group 2) plus the drivers.  Both share this
 * one component.
 */
#ifndef FSS_RUNTIME_ORCHESTRATOR_H
#define FSS_RUNTIME_ORCHESTRATOR_H

#include "fss/transport_abi.h"
#include "fss/runtime/control_protocol.h"   /* fss_ctrl_instr */

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

/* sim::event codes the readiness wave / teardown use (see protocols/sim). */
#define FSS_SIM_BOUND      0u
#define FSS_SIM_ELABORATED 1u
#define FSS_SIM_STARTED    2u
#define FSS_SIM_STOPPED    5u

/* ── transport plugin loading ───────────────────────────────────────────────
 * A transport exports ABI query symbols plus the standard factory symbols.  The
 * runtime validates the queries before it resolves a factory or reads a vtable;
 * the inbound sink is installed through attach(..., FSS_ROLE_TARGET). */
typedef fss_transport_t *(*fss_create_fn_t)(const char *);
typedef void             (*fss_destroy_fn_t)(fss_transport_t *);

/* The agent's construction transport (in-proc), after ABI negotiation. */
typedef struct { void *dl; fss_create_fn_t create; fss_destroy_fn_t destroy; } fss_inproc_lib;
int  fss_load_inproc(const char *plugin, fss_inproc_lib *lib);

/* A cross-process CHANNEL transport (shm or zenoh), after ABI negotiation; the
 * receive sink is installed via t->attach(..., FSS_ROLE_TARGET). */
typedef struct { void *dl; fss_create_fn_t create; fss_destroy_fn_t destroy; } fss_chan_lib;
int  fss_load_channel(const char *plugin, fss_chan_lib *lib);
/* Create a channel transport (retrying briefly against a peer whose session is
 * not yet up).  The channel medium itself is then provisioned by
 * fss_chan_open_retry / t->channel(). */
fss_transport_t *fss_chan_create_retry(fss_chan_lib *lib, const char *cfg);

/* Resolve a transport NAME to libfss_<name><suffix> in `dir` (or the loader
 * search path when NULL/empty). */
void fss_transport_plugin_path(const char *dir, const char *name, char *out, size_t n);
/* Resolve a model NAME to its plugin file in `dir`: lib<name>.dylib if present,
 * else lib<name>.so (bare lib<name><suffix> when `dir` is NULL/empty).  Models
 * obey the same name->plugin discipline as transports — never a path. */
void fss_model_plugin_path(const char *dir, const char *name, char *out, size_t n);
/* Load a channel transport by NAME (resolve + dlopen + confirm fss_transport_name()).
 * The single resolver a data edge and the control bootstrap both go through. */
int  fss_load_channel_named(const char *dir, const char *name, fss_chan_lib *lib);

/* ── kind-aware channel setup (shm OR zenoh) ────────────────────────────────
 * create-vs-attach (owner) and round-trip-vs-one-way are channel() parameters,
 * not a config dialect.  The only kind-specific create() input left is the
 * SESSION config: empty for shm (per-channel medium), the process rendezvous
 * (role + endpoint) for zenoh's shared session. */
void fss_chan_session_cfg(int zenoh, const char *role, const char *zrv, char *out, size_t n);
/* Open a named channel on a transport, retrying for an attacher (owner = 0)
 * until the owner has provisioned the medium; a single try for an owner. */
fss_channel_t *fss_chan_open_retry(fss_transport_t *t, const char *name, int rt, int owner);

/* ── orchestrator / harness instruments (legitimately not artefact endpoints) ─
 * The delivery counter: an endpoint whose receive forwards to an inner sink and
 * increments a counter, installed by whichever agent hosts the sink so the run
 * can measure delivery on the wire (language-agnostic). */
typedef struct { fss_endpoint_t *inner; volatile int *count; } fss_counting_ctx;
int  fss_counting_receive(void *self, fss_carrier_t *c);
int  fss_wait_for_count(volatile int *count, int target, int timeout_ms);

/* Fire a sim::event of `code` into a node's fss/sim endpoint (arming/teardown). */
void fss_local_sim(fss_endpoint_t *sim, uint32_t code);

/* The federation-status latch: watches init's fss/sim for Elaborated. */
typedef struct { volatile int ready; } fss_fed_status;
int  fss_fed_status_receive(void *self, fss_carrier_t *c);
int  fss_wait_for_flag(volatile int *flag, int timeout_ms);

/* Build the run config JSON (frames [+ period_us]) each node pulls locally. */
void fss_build_config_json(int frames, int period_us, char *out, size_t n);
/* The config responder: answers a config pull in place (self = the JSON). */
int  fss_config_responder(void *self, fss_carrier_t *c);

/* Send one prepared control instruction to a remote agent, round-trip; returns
 * the agent's status (or -1 on transport failure).  A cross edge is
 * served/attached inside whichever agent owns its endpoint (OP_SERVER /
 * OP_CLIENT), so the orchestrator drives agents and never builds a channel
 * itself.  The instruction (including its data-edge transport name) is built by
 * the caller; on return `in->status` holds the agent's verdict. */
int  fss_ctrl_send(fss_endpoint_t *cep, fss_ctrl_instr *in);

/* Stateful topology RESPONDER (R11 pull): install as the orchestrator's control-
 * channel TARGET sink with `self` = the agent's build+wire plan (JSON step array).
 * The agent calls back with an empty control::plan request; this fills PlanJson +
 * Status=Ok over the round-trip.  Topology only — lifecycle rides a separate
 * sim-control channel. */
int  fss_plan_responder(void *self, fss_carrier_t *c);

/* Small sleep helper (ms). */
void fss_msleep(long ms);

#ifdef __cplusplus
}
#endif
#endif
