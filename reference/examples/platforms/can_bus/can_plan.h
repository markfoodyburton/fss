/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_plan.h — build the CAN platform's plan for the orchestrator engine.
 *
 * The platform is init -> ctrl -> dev with a round-trip data edge and
 * bidirectional fss/time per hop plus an upward fss/sim edge.  This turns the
 * CLI (per-hop transport kind + plugins + artefacts) into a data-only
 * fss_plan_t; the generic engine (fss_orchestrate) does everything else.
 */
#ifndef CAN_PLAN_H
#define CAN_PLAN_H

#include "fss/runtime/engine.h"

/* Fill `plan` from the parsed CLI.  hop1_kind / hop2_kind are FSS_XK_*.  The
 * construction transport (inproc) is passed by path; every channel transport
 * (data edges AND the control back-channel) is resolved by name from
 * `transport_dir`; the initiator/controller/device MODELS are NAMES resolved to
 * a plugin from `model_dir`.  Returns 0 on success. */
/* device2 (optional): NULL = today's single-device platform (byte-identical).
 * Non-NULL adds a SECOND CAN device fanned out from the SAME controller source
 * (ctrl/can_tx) over the hop2 transport — data-plane 1->N fan-out.
 *
 * config_kind (FSS_XK_*, or < 0 = off): < 0 keeps today's LOCAL-only config (each
 * agent serves its own nodes — byte-identical, the 576).  >= 0 adds CONFIG FAN-IN:
 * ctrl and dev PULL the run config from the root node init over a round-trip
 * fss/config edge of that transport kind (N->1 fan-in on init/fss/config). */
int can_build_plan(fss_plan_t *plan,
                   int hop1_kind, int hop2_kind,
                   const char *inproc_plugin,
                   const char *transport_dir, const char *model_dir,
                   const char *zrv, const char *agent_binary,
                   const char *initiator, const char *controller, const char *device,
                   const char *device2, int config_kind,
                   int frames, int period_us);

#endif
