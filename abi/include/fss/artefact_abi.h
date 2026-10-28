/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/artefact_abi.h — C entry points an FSS-deployable artefact
 * (.dylib/.so/.dll, any source language) exports.
 *
 * This is THE way a transport instantiates an artefact: an artefact is a
 * dlopen-able binary, and a transport brings it to life by dlopen + symbol
 * lookup + fss_create.  This is universal — the *deployment mode* only changes
 * WHO performs the dlopen, whether a process is forked first, and which
 * transport then connects the endpoints:
 *
 *   - in-process     — the host dlopens the artefact in its own process (no
 *                      fork); carriers move by in-process memory.
 *   - in-machine SHM — the transport forks a child, which dlopens the artefact;
 *                      carriers cross via a shared-memory segment + doorbell.
 *   - remote (e.g. zenoh) — the remote node dlopens the artefact on its host;
 *                      carriers cross the network.
 *
 * In every case the artefact itself is dlopened and created through these
 * symbols.  They are therefore NORMATIVE for any artefact: a model author
 * exports them (usually via a language binding) so that any transport, local
 * or remote, can host the model.
 *
 * An artefact owns its endpoints (fss/endpoint.h); whoever loaded it uses a
 * transport (fss/transport_abi.h) to communicate carriers between endpoints.
 *
 *   fss_create       — instantiate the artefact; transport + name.
 *   fss_destroy      — tear down the artefact.
 *   fss_get_endpoint — resolve a named endpoint on the artefact;
 *                      handle is valid until fss_destroy(node).
 *
 * Lifecycle (start/stop) is NOT an ABI verb — it is a sim::event
 * carrier sent to the artefact's fss/sim standard port (a binding-
 * provided endpoint returned by fss_get_endpoint(node, "fss/sim")).
 * Likewise configuration is a config::param_request carrier to fss/config.
 *
 * Implication for a kernel-bearing model (e.g. SystemC): because the artefact
 * is created fresh by fss_create in (almost) every deployment, the language
 * binding must stand up that model's simulation kernel itself — see the
 * SystemC binding (languages/systemc-tlm/.../artefact.h) for the four cases and
 * the one where it could be elided.
 *
 * One artefact type per .dylib for now (singleton symbols).  A future
 * spec revision may add an optional `fss_artefact_manifest` entry
 * point for multi-type libraries; loaders should prefer it when
 * exported and fall back to `fss_create` otherwise.
 */
#ifndef FSS_ARTEFACT_ABI_H
#define FSS_ARTEFACT_ABI_H

#include "fss/transport_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define FSS_EXPORT __declspec(dllexport)
#else
#  define FSS_EXPORT __attribute__((visibility("default")))
#endif

FSS_EXPORT fss_node_t     *fss_create      (fss_transport_t *transport,
                                            const char *name);
FSS_EXPORT void            fss_destroy     (fss_node_t *handle);
FSS_EXPORT fss_endpoint_t *fss_get_endpoint(fss_node_t *node,
                                            const char *endpoint_name);

#ifdef __cplusplus
}
#endif
#endif
