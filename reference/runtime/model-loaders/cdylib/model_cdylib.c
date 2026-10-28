/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * model_cdylib.c — the model loader for a C / SystemC / Rust cdylib.
 *
 * The simplest possible loader: the model IS an artefact-ABI DLL, so the
 * placement facility knows how to bring it to life (fss_place_construct /
 * fss_place_endpoint / fss_place_destroy).  This loader therefore delegates
 * straight to those functions — no language runtime to embed, no wrapper.
 * It is what makes the cdylib languages fall out of the agent model for free
 * (byte-identical to the pre-loader path).
 *
 * A Python or FMI loader will present the SAME three verbs while constructing
 * very differently (embed CPython + load a .py; dlopen a .fmu behind a per-role
 * wrapper) — that is the whole point of the model-loader seam.
 */
#include "fss/runtime/agent_core.h"
#include "fss/runtime/placement.h"

#include <stdio.h>

static fss_node_t *cdylib_construct(void *self,
                                    const char *node, const char *model_locator)
{
    (void)self;
    return fss_place_construct(node, model_locator);
}

static fss_endpoint_t *cdylib_get_endpoint(void *self,
                                           fss_node_t *node, const char *ep)
{
    (void)self;
    return fss_place_endpoint(node, ep);
}

static void cdylib_destroy(void *self, fss_node_t *node)
{
    (void)self;
    fss_place_destroy(node);
}

/* The exported cdylib loader.  An agent links this and hands &fss_model_cdylib
 * to fss_agent_run. */
const fss_model_loader_t fss_model_cdylib = {
    NULL, cdylib_construct, cdylib_get_endpoint, cdylib_destroy
};
