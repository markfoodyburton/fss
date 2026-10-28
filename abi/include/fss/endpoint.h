/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/endpoint.h — one of the two main FSS objects.
 *
 *   endpoint    a carrier sink (inbound `receive`) plus, once bound, a
 *               carrier source (outbound `send`).  Owned by an artefact
 *               (fss/artefact_abi.h); each artefact exposes one or more.
 *               Wrapped per language.
 *   transport   the other main object (fss/transport_abi.h): resolves
 *               endpoints and binds channels between them.
 *
 * fss_node_t is an opaque handle to an artefact instance; endpoints are
 * resolved on it by name.
 *
 * Inbound and outbound are symmetric and installed by opposite owners:
 *   - `receive` (inbound) is installed by the ARTEFACT — what happens to a
 *     carrier arriving here.
 *   - `send`/`send_ctx` (outbound) are installed by the TRANSPORT at bind —
 *     how a carrier leaves here.  NULL `send` means unbound.
 * Both use the same signature (fss_carrier_fn) and the same return contract,
 * so `send` returns exactly what the peer's `receive` returned (the transport
 * ferries the status back).  In-process 1:1 bind is therefore the literal
 * identity `local->send = peer->receive; local->send_ctx = peer->self` — no
 * shim, no per-send transport lookup.
 *
 * A model NEVER reaches into another endpoint: it delivers a carrier by
 * invoking the transport-installed `send`, i.e. `ep->send(ep->send_ctx, c)`.
 * The outbound channel is private transport state behind `send_ctx`.  (A
 * language binding may wrap that one-liner in a convenience; the ABI itself
 * is just the struct below — no helper.)
 */
#ifndef FSS_ENDPOINT_H
#define FSS_ENDPOINT_H

#include "fss/carrier.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void fss_node_t;

/* Delivery callback — both inbound (`receive`) and outbound (`send`).
 * Returns a transport status (FSS_SEND_* in fss/transport_abi.h):
 * 0 = OK, non-zero = reason.  A `send` returns the peer receive's result. */
typedef int (*fss_carrier_fn)(void *self, fss_carrier_t *carrier);

typedef struct fss_endpoint fss_endpoint_t;
struct fss_endpoint {
    void           *self;       /* inbound context (artefact-defined)        */
    fss_carrier_fn  receive;    /* inbound delivery; installed by artefact   */
    fss_carrier_fn  send;       /* outbound delivery; installed by bind; NULL = unbound */
    void           *send_ctx;   /* outbound context; transport-private       */
};

FSS_ABI_STATIC_ASSERT(sizeof(fss_carrier_fn) == 8,
                      "FSS ABI unsupported layout: requires 64-bit function pointers");
FSS_ABI_STATIC_ASSERT(sizeof(fss_endpoint_t) == 32,
                      "FSS ABI unsupported layout: fss_endpoint_t must be 32 bytes");
FSS_ABI_STATIC_ASSERT(offsetof(fss_endpoint_t, self) == 0,
                      "FSS ABI unsupported layout: fss_endpoint_t.self offset must be 0");
FSS_ABI_STATIC_ASSERT(offsetof(fss_endpoint_t, receive) == 8,
                      "FSS ABI unsupported layout: fss_endpoint_t.receive offset must be 8");
FSS_ABI_STATIC_ASSERT(offsetof(fss_endpoint_t, send) == 16,
                      "FSS ABI unsupported layout: fss_endpoint_t.send offset must be 16");
FSS_ABI_STATIC_ASSERT(offsetof(fss_endpoint_t, send_ctx) == 24,
                      "FSS ABI unsupported layout: fss_endpoint_t.send_ctx offset must be 24");

#ifdef __cplusplus
}
#endif
#endif
