/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * launcher.c — reference local launcher (see launcher.h).  NON-NORMATIVE.
 *
 * fork + execv the agent binary, born-with marshalled onto argv with explicit
 * flags (order-independent, so the agent's parse is robust).  The flag names
 * here and the agent's parser are a PRIVATE contract between this launcher and
 * this agent, not an FSS ABI: a different launcher/agent pair may agree on env,
 * a control-fd handshake, or anything else.
 */
#include "fss/runtime/launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>

pid_t fss_launch_agent(const char *agent_path, const fss_born_with *bw)
{
    char frames[16], period[16], status_fd[16], release_fd[16], barrier_nonce[32];
    int child_status_fd = -1, child_release_fd = -1;
    int have_barrier;

    if (!agent_path || !bw) return -1;
    /* A valid descriptor includes stdin, stdout, and stderr.  The agent command
     * line reserves those descriptors for normal process use, so duplicate low
     * numbered barrier descriptors before fork.  Do this in the parent: argv is
     * complete before fork and the parent's original pipe ends stay unchanged. */
    have_barrier = bw->status_fd >= 0 && bw->release_fd >= 0;
    if (have_barrier) {
        child_status_fd = bw->status_fd < 3 ? fcntl(bw->status_fd, F_DUPFD, 3) : bw->status_fd;
        if (child_status_fd < 0) return -1;
        child_release_fd = bw->release_fd < 3 ? fcntl(bw->release_fd, F_DUPFD, 3) : bw->release_fd;
        if (child_release_fd < 0) {
            if (child_status_fd != bw->status_fd) close(child_status_fd);
            return -1;
        }
    }
    snprintf(frames, sizeof(frames), "%d", bw->frames);
    snprintf(period, sizeof(period), "%d", bw->period_us);

    /* Fixed-slot argv; data-edge transports are resolved by name from the
     * transport dir, so no per-transport plugin path is passed (only the
     * construction plugin and the control breadcrumb).  NULL-terminated for execv. */
    const char *argv[32];
    int n = 0;
    argv[n++] = agent_path;
    argv[n++] = "--inproc";        argv[n++] = bw->inproc_plugin;
    argv[n++] = "--transport-dir"; argv[n++] = bw->transport_dir ? bw->transport_dir : "";
    argv[n++] = "--model-dir";     argv[n++] = bw->model_dir ? bw->model_dir : "";
    argv[n++] = "--zrole";         argv[n++] = bw->zrole ? bw->zrole : "connect";
    argv[n++] = "--zrv";           argv[n++] = bw->zrv ? bw->zrv : "";
    argv[n++] = "--fss-parent";    argv[n++] = bw->breadcrumb;
    argv[n++] = "--frames";        argv[n++] = frames;
    argv[n++] = "--period-us";     argv[n++] = period;
    if (have_barrier) {
        snprintf(status_fd, sizeof(status_fd), "%d", child_status_fd);
        snprintf(release_fd, sizeof(release_fd), "%d", child_release_fd);
        snprintf(barrier_nonce, sizeof(barrier_nonce), "%" PRIu64, bw->barrier_nonce);
        argv[n++] = "--fss-status-fd"; argv[n++] = status_fd;
        argv[n++] = "--fss-release-fd"; argv[n++] = release_fd;
        argv[n++] = "--fss-barrier-nonce"; argv[n++] = barrier_nonce;
    }
    argv[n]   = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fss_launch_agent: fork");
        if (child_status_fd != bw->status_fd) close(child_status_fd);
        if (child_release_fd != bw->release_fd) close(child_release_fd);
        return -1;
    }
    if (pid == 0) {
        if (have_barrier) {
            (void)fcntl(child_status_fd, F_SETFD, 0);
            (void)fcntl(child_release_fd, F_SETFD, 0);
            /* Do not retain a second inherited writer/reader if a low descriptor
             * was normalized.  A retained writer can hide EOF from the parent. */
            if (child_status_fd != bw->status_fd) close(bw->status_fd);
            if (child_release_fd != bw->release_fd && bw->release_fd != bw->status_fd)
                close(bw->release_fd);
        }
        execv(agent_path, (char *const *)argv);
        perror("fss_launch_agent: execv");   /* only reached on failure */
        _exit(127);
    }
    if (child_status_fd != bw->status_fd) close(child_status_fd);
    if (child_release_fd != bw->release_fd) close(child_release_fd);
    return pid;
}
