/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * inproc.cpp — in-process FSS transport, built as a loadable PLUGIN.
 *
 * Exports the well-known transport factory symbols fss_transport_create /
 * fss_transport_destroy (fss/transport_abi.h).  After R1, artefact construction
 * moved to the placement facility (fss/runtime/placement.h) and 1:1 in-process
 * edges are wired DIRECTLY by the agent/host (local->send = peer->receive) with
 * no transport on the path — the identity fast path.
 *
 * R4 reintroduces a ROUTING in-process transport for the MULTIPOINT case (a
 * channel with more than one target), which the direct-wire path cannot express.
 * A channel here follows the arity state machine (docs/design/channel-abi-rework.md
 * "In-process implementation states"):
 *   - identity (1 initiator + 1 target): the initiator's send is wired straight
 *     to the target's receive — the same zero-cost identity the agent does for a
 *     1:1 edge, just reached through attach().  The transport steps off the path.
 *   - broadcast-shim (>= 2 one-way targets): on the second target attach the
 *     identity binding is torn down and the transport reinserts itself as a
 *     fan-out shim; every send iterates the targets.
 *   - refuse: a ROUND-TRIP channel with a second target is an error (D4) — a
 *     round-trip write-back must return unambiguously to one originator, and
 *     routing by content would be a model, not a transport.  send_to on a
 *     multi-target channel is likewise refused for a round-trip channel.
 *
 * One channel per transport (v0.1), so the channel handle IS the transport
 * state, exactly as shm/zenoh do it.  The existing 576-cell matrix does not use
 * this path (its in-proc edges are the agent's direct-wire); only the multipoint
 * test (fss_inproc_bus) exercises it.
 */
#include "inproc.h"

#include "fss/transport_abi.h"   /* fss_role, fss_channel_t, FSS_SEND_* */
#include "fss/endpoint.h"
#include "fss/carrier.h"

#include <cstdio>

namespace {

constexpr int INPROC_MAX_TARGETS = 16;

struct inproc_state {
    fss_transport_t transport{};
    bool            has_channel = false;
    bool            round_trip  = false;
    fss_endpoint_t *initiator   = nullptr;                 /* the one sender */
    fss_endpoint_t *targets[INPROC_MAX_TARGETS] = {};      /* receivers */
    int             n_targets   = 0;
};

/* Fan-out shim (>= 2 one-way targets): deliver the carrier to every target in
 * turn.  Installed as initiator->send with send_ctx = the channel state.  This
 * path is one-way only (the round-trip >1-target case is refused at attach), so
 * there is no write-back to ferry and the per-target status is aggregated:
 * OK unless a target reports a receiver error. */
int inproc_fanout(void *send_ctx, fss_carrier_t *carrier)
{
    auto *s = static_cast<inproc_state *>(send_ctx);
    int agg = FSS_SEND_OK;
    for (int i = 0; i < s->n_targets; i++) {
        fss_endpoint_t *t = s->targets[i];
        int rc = (t && t->receive) ? t->receive(t->self, carrier) : FSS_SEND_NO_RECEIVER;
        if (rc != FSS_SEND_OK) agg = rc;   /* remember the last non-OK */
    }
    return agg;
}

/* Recompute the initiator's outbound wiring for the channel's current arity.
 * Called after any attach that could change it.  No initiator yet -> nothing to
 * wire (a target that arrives first is simply recorded).  1 target -> identity
 * (send = peer->receive, transport off the path).  >= 2 targets -> fan-out shim. */
void inproc_rewire(inproc_state *s)
{
    if (!s->initiator) return;
    if (s->n_targets == 1) {
        s->initiator->send     = s->targets[0]->receive;
        s->initiator->send_ctx = s->targets[0]->self;
    } else if (s->n_targets >= 2) {
        s->initiator->send     = inproc_fanout;
        s->initiator->send_ctx = s;
    } else {
        s->initiator->send     = nullptr;   /* no target yet: unbound */
        s->initiator->send_ctx = nullptr;
    }
}

fss_channel_t *inproc_channel_vt(void *self, const char *name, int round_trip,
                                 int owner, const uint64_t *carrier_hints, size_t n)
{
    auto *s = static_cast<inproc_state *>(self);
    (void)name; (void)owner; (void)carrier_hints; (void)n;   /* one channel/transport; hints: unused */
    s->has_channel = true;
    s->round_trip  = (round_trip != 0);
    return reinterpret_cast<fss_channel_t *>(s);
}

int inproc_attach_vt(void *self, fss_channel_t *ch, fss_endpoint_t *local, fss_role role)
{
    auto *s = static_cast<inproc_state *>(self);
    (void)ch;   /* one channel per transport, ch == s */
    if (!s->has_channel || !local) return FSS_SEND_UNBOUND;

    if (role == FSS_ROLE_TARGET) {
        /* D4: a round-trip channel admits at most one target. */
        if (s->round_trip && s->n_targets >= 1) {
            fprintf(stderr, "fss_inproc: round-trip channel refuses a 2nd target (D4)\n");
            return FSS_SEND_UNBOUND;
        }
        if (s->n_targets >= INPROC_MAX_TARGETS) return FSS_SEND_UNBOUND;
        s->targets[s->n_targets++] = local;
        inproc_rewire(s);
        return FSS_SEND_OK;
    }

    /* INITIATOR: record the single sender and wire it for the current arity. */
    s->initiator = local;
    inproc_rewire(s);
    return FSS_SEND_OK;
}

int inproc_send_to_vt(void *self, fss_channel_t *ch, fss_carrier_t *carrier)
{
    auto *s = static_cast<inproc_state *>(self);
    (void)ch;
    if (!s->has_channel || !s->n_targets) return FSS_SEND_UNBOUND;
    if (s->round_trip && s->n_targets > 1) return FSS_SEND_UNBOUND;   /* D4 */
    if (s->n_targets == 1) {
        fss_endpoint_t *t = s->targets[0];
        return (t && t->receive) ? t->receive(t->self, carrier) : FSS_SEND_NO_RECEIVER;
    }
    return inproc_fanout(s, carrier);
}

} // namespace

extern "C" FSS_EXPORT const char *
fss_transport_name(void)
{
    return "inproc";
}

extern "C" FSS_EXPORT uint32_t
fss_transport_abi_version(void)
{
    return FSS_TRANSPORT_ABI_VERSION;
}

extern "C" FSS_EXPORT uint64_t
fss_transport_capabilities(void)
{
    return FSS_TRANSPORT_CAP_CHANNEL_ABI;
}

extern "C" FSS_EXPORT fss_transport_t *
fss_transport_create(const char * /*config*/)
{
    auto *s = new inproc_state;
    s->transport.self    = s;
    s->transport.channel = inproc_channel_vt;
    s->transport.attach  = inproc_attach_vt;
    s->transport.send_to = inproc_send_to_vt;
    return &s->transport;
}

extern "C" FSS_EXPORT void
fss_transport_destroy(fss_transport_t *t)
{
    if (!t) return;
    auto *s = static_cast<inproc_state *>(t->self);
    delete s;
}
