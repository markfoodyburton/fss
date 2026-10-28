/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/runtime/launcher.h — spawn an agent + hand it its born-with (NON-NORMATIVE).
 *
 * The launcher is the thinnest, least standard part of the runtime: FSS fixes
 * WHAT must reach an agent (the transport to load, the home locator, the
 * completion bound), not HOW it is spawned or how the facts arrive.  This
 * reference launcher spawns a local agent binary with fork + execv and hands
 * the born-with over argv.  A remote deployment would ssh/scheduler-launch and
 * pass the same facts however it likes; nothing here is an ABI.
 *
 * The born-with does NOT include the model: an agent is told which model to
 * construct later, over the control path (OP_CONSTRUCT), by the orchestrator.
 * What it IS born with is its construction transport, its back-channel medium +
 * home locator, and (for zenoh edges) the zenoh plugin + rendezvous role/point.
 */
#ifndef FSS_RUNTIME_LAUNCHER_H
#define FSS_RUNTIME_LAUNCHER_H

#include <sys/types.h>   /* pid_t */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The born-with facts an agent is spawned with (everything but the model). */
typedef struct {
    const char *inproc_plugin;   /* construction transport (always) */
    const char *transport_dir;   /* dir to resolve data-edge transport plugins (libfss_<name>.<ext>) in */
    const char *model_dir;       /* dir to resolve MODEL plugins by name (lib<name>.<ext>) in */
    const char *zrole;           /* "listen" (star centre) or "connect" (leaf) */
    const char *zrv;             /* the process's zenoh rendezvous endpoint */
    const char *breadcrumb;      /* JSON: which control transport to load + its config (phone-home) */
    int         frames;          /* completion bound */
    int         period_us;
    /* Private two-phase launcher barrier.  Negative fds disable it, preserving
     * standalone-agent behaviour.  Valid low descriptors are duplicated above
     * stderr before exec.  The child writes ATTACHED/TRANSPORT_READY/READY and
     * reads nonce-validated PROBE, RELEASE, then post-READY STOP commands. */
    int         status_fd;
    int         release_fd;
    uint64_t    barrier_nonce;
} fss_born_with;

/* fork + execv `agent_path`, marshalling `bw` onto its argv.  Returns the child
 * pid (> 0) in the parent, or -1 on fork failure.  The child never returns. */
pid_t fss_launch_agent(const char *agent_path, const fss_born_with *bw);

#ifdef __cplusplus
}
#endif
#endif
