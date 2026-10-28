/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss-agent-c — the generic agent, using the cdylib model loader.
 *
 * The agent is a generic, artefact-agnostic body (agent-core) plus a model
 * loader.  This binary uses the cdylib loader, which brings any artefact-ABI
 * DLL to life through the in-proc construction transport — including today's
 * Python and FMI models, which are compiled as artefact-ABI adapter DLLs (a
 * baked CPython wrapper / a per-role FMU wrapper), so one cdylib agent hosts all
 * four languages.  The model-loader vtable is the seam where a future RUNTIME
 * loader (embed CPython for a bare .py, dlopen a bare .fmu) would plug in; note
 * a single agent process may co-host nodes of different languages (mixed groups
 * in the matrix), so it bundles whatever loaders its nodes need rather than
 * being one binary per language.  main() only parses the born-with facts off
 * argv (the private launcher<->agent contract) and hands them to fss_agent_run.
 * Which MODEL to construct is NOT born-with: it arrives over the control path
 * (OP_CONSTRUCT).
 */
#include "fss/runtime/agent_core.h"
#include "fss/runtime/model_cdylib.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static const char *opt(int argc, char **argv, const char *key)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], key)) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv)
{
    const char *inproc = opt(argc, argv, "--inproc");
    const char *xdir   = opt(argc, argv, "--transport-dir");  /* data-edge plugins resolved by name here */
    const char *mdir   = opt(argc, argv, "--model-dir");      /* MODEL plugins resolved by name here */
    const char *zrole  = opt(argc, argv, "--zrole");
    const char *zrv    = opt(argc, argv, "--zrv");
    const char *parent = opt(argc, argv, "--fss-parent");
    const char *frames = opt(argc, argv, "--frames");
    const char *period = opt(argc, argv, "--period-us");
    const char *status_fd_s = opt(argc, argv, "--fss-status-fd");
    const char *release_fd_s = opt(argc, argv, "--fss-release-fd");
    const char *barrier_nonce_s = opt(argc, argv, "--fss-barrier-nonce");
    int status_fd = -1, release_fd = -1;
    uint64_t barrier_nonce = 0;

    if (status_fd_s || release_fd_s || barrier_nonce_s) {
        char *end = NULL;
        errno = 0;
        long fd = status_fd_s ? strtol(status_fd_s, &end, 10) : -1;
        if (!status_fd_s || !end || *end || errno == ERANGE || fd < 3 || fd > INT32_MAX) {
            fprintf(stderr, "fss-agent-c: invalid status fd\n");
            return 2;
        }
        status_fd = (int)fd;
        errno = 0; end = NULL;
        fd = release_fd_s ? strtol(release_fd_s, &end, 10) : -1;
        if (!release_fd_s || !end || *end || errno == ERANGE || fd < 3 || fd > INT32_MAX) {
            fprintf(stderr, "fss-agent-c: invalid release fd\n");
            close(status_fd);
            return 2;
        }
        release_fd = (int)fd;
        errno = 0; end = NULL;
        barrier_nonce = barrier_nonce_s ? strtoull(barrier_nonce_s, &end, 10) : 0;
        if (!barrier_nonce_s || !end || *end || errno == ERANGE) {
            fprintf(stderr, "fss-agent-c: invalid barrier nonce\n");
            close(status_fd); close(release_fd);
            return 2;
        }
    }

    if (!inproc || !parent) {
        fprintf(stderr, "fss-agent-c: need --inproc --fss-parent "
                        "(and --transport-dir --zrole --zrv --frames --period-us)\n");
        close(status_fd); close(release_fd);
        return 2;
    }

    return fss_agent_run(&fss_model_cdylib, inproc, xdir, mdir,
                          zrole, zrv, parent,
                          frames ? atoi(frames) : 0,
                          period ? atoi(period) : 0,
                           status_fd, release_fd, barrier_nonce);
}
