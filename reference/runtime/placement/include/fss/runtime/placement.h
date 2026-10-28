/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/runtime/placement.h — process-level artefact placement facility.
 *
 * Construction of artefact nodes was previously split across every transport
 * plugin (inproc, shm, zenoh all carried identical open/close/get_endpoint
 * code).  The placement facility centralises that code once, linked into
 * EXECUTABLES only (NOT into transport plugins — the plugins are dlopened
 * RTLD_LOCAL and must not share a registry with each other or the host).
 *
 * Placement rule: construct-here, always.  fss_place_construct dlopens the
 * artefact DLL and calls its fss_create in the CALLING process.  A
 * cross-process deployment has the launcher call fss_place_construct in the
 * far process; the two processes then rendezvous with transport bind, not by
 * placement.
 *
 * The registry is process-global (a static std::map inside placement.cpp),
 * guarded by a mutex.
 */
#ifndef FSS_RUNTIME_PLACEMENT_H
#define FSS_RUNTIME_PLACEMENT_H

#include "fss/transport_abi.h"   /* fss_node_t, fss_endpoint_t */

#ifdef __cplusplus
extern "C" {
#endif

/* fss_place_construct(node, locator):
 *   dlopen the artefact at `locator`, call fss_create(NULL, node) (transport
 *   arg is unused by all artefacts — verified), register name->{dl,node,fns}.
 *   Returns the node handle, or NULL on any failure.  An already-registered
 *   name is an error (printed; returns NULL). */
fss_node_t     *fss_place_construct(const char *node, const char *locator);

/* fss_place_endpoint(node, ep):
 *   Call the node's fss_get_endpoint.  Returns NULL if the node is not in
 *   the registry or has no such endpoint. */
fss_endpoint_t *fss_place_endpoint (fss_node_t *node, const char *ep);

/* fss_place_resolve(node_slash_ep):
 *   Resolve "node/ep" → endpoint via the registry (split at the FIRST '/').
 *   Returns NULL if the node is not registered or the endpoint does not
 *   exist.  The ep name may itself contain '/' (e.g. "fss/sim"). */
fss_endpoint_t *fss_place_resolve  (const char *node_slash_ep);

/* fss_place_destroy(node):
 *   Call fss_destroy on the node, dlclose its DLL, and unregister it.
 *   No-op if node is not in the registry. */
void            fss_place_destroy  (fss_node_t *node);

#ifdef __cplusplus
}
#endif
#endif
