/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss_zenoh.h — network / cross-host FSS transport plugin (v0.1).
 *
 * A loadable transport PLUGIN: exports the well-known factory symbols
 * `fss_transport_create` / `fss_transport_destroy` (fss/transport_abi.h), which
 * a host resolves after dlopening this module.  Like the SHM transport, zenoh
 * bridges peers over a
 * zenoh session, carrying only atom BYTES across the boundary.  Where SHM is
 * the same-machine fast path, zenoh is the network / cross-host path (see
 * docs/design/zenoh-transport.md).
 *
 * Two carrier-flow shapes, selected by channel(..., round_trip, ...):
 *   - REQUEST/REPLY (round_trip != 0): a bound send() is synchronous and two-way — it
 *     z_get()s the peer's queryable, blocks for the reply, and copies the
 *     (possibly mutated) bytes + status back over the caller's carrier (the
 *     return-path contract in transport_abi.h).  For MUTABLE carriers (a txn
 *     whose Status/read-data is written back).
 *   - PUB/SUB (round_trip == 0, one-way): send() is a fire-and-forget z_put(); the receive side
 *     is a subscriber.  For IMMUTABLE / fan-out carriers (can::frame, a
 *     sim::event broadcast) — no write-back.
 *
 * A request/reply channel has one target.  Re-attaching a different target to
 * one local transport state is rejected.  The runtime topology validator also
 * rejects cross-process request/reply fan-out; use a one-way channel for fan-out.
 *
 * `config` is a ';'-separated option string:
 *   "key=<prefix>"   — this node's key-expression prefix (zenoh-key-compatible
 *                      <node>/<endpoint>, per docs/design/binding-and-phases.md).
 *   "mode=<peer|client>" — zenoh session mode (default: peer).
 *   "pubsub"         — historical compatibility token; channel() flow selection
 *                      by round_trip takes precedence.
 * e.g. "key=fss/spi;mode=peer".
 *
 * vtable (fss/transport_abi.h):
 *   channel(name, round_trip, owner, hints, n) — find the key-defined channel.
 *   attach(channel, local, INITIATOR) — installs z_get (round-trip) or z_put
 *     (one-way) as local->send.
 *   attach(channel, local, TARGET) — declares the queryable or subscriber.
 *   send_to(channel, c) — one-shot request/reply or publish.
 *
 * Artefact construction and endpoint lookup are local placement work.  The
 * exported sink helper remains for direct-host compatibility.
 *
 * Optional remote readiness: fss_transport_capabilities() includes
 * FSS_TRANSPORT_CAP_REMOTE_READY when fss_zenoh_set_edge() and
 * fss_zenoh_wait_ready() are available. A caller tests that bit before dlsym.
 * Configure both peers before the target attach. Start the target/listener
 * first; it installs its data receiver, then declares a liveliness token. The
 * initiator waits with a finite timeout. Liveliness finds the target, but a
 * bounded challenge proves the data path: z_get/reply for request/reply, or a
 * z_put observed by the target subscriber plus an acknowledgement query for
 * pub/sub. No fixed discovery-settle sleep is part of this contract.
 *
 * Wire framing (both peers agree on this; the carrier byte format is the only
 * cross-ABI contract — see fss/carrier.h):
 *   request/publish = [data magic/version][id:u64 LE][meta_len:u32 LE][flat meta]
 *                     [payload_len:u32 LE][serialized payload]
 *   reply           = [status:i32 LE][payload_len:u32 LE][serialized payload]
 * `fss:local-ptr` data is serialized into the payload; the request carries the
 * flattened id and flattened metadata, never process-local pointer values.
 */
#ifndef FSS_ZENOH_H
#define FSS_ZENOH_H

#include "fss/transport_abi.h"   /* fss_transport_create / fss_transport_destroy */
#include "fss/endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the local endpoint that received carriers are delivered to, and
 * declare the zenoh queryable (request/reply) or subscriber (pub/sub) on this
 * node's key prefix.  The handler reconstructs a flat fss_carrier_t pointing
 * into the received payload, calls sink->receive, and (request/reply) replies
 * with the mutated bytes + status.  `sink` must outlive the transport. */
FSS_EXPORT void fss_zenoh_set_sink(fss_transport_t *transport, fss_endpoint_t *sink);

/* Configure one launch edge for the optional remote-ready barrier before the
 * target attaches. Liveliness discovers the exact target token, then
 * wait_ready proves the real data path with bounded retries to timeout_ms:
 * z_get probe/reply for request/reply, or z_put probe followed by target ack
 * for pub/sub. Endpoints are opaque full endpoint names; they are hashed before
 * they enter a Zenoh key. */
FSS_EXPORT int fss_zenoh_set_edge(fss_transport_t *transport,
                                  const char *local_endpoint,
                                  const char *peer_endpoint, int role);
FSS_EXPORT int fss_zenoh_wait_ready(fss_transport_t *transport,
                                    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
