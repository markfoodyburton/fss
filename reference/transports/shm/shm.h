/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * shm.h — cross-process shared-memory FSS transport plugin (v0.1).
 *
 * A loadable transport PLUGIN: exports the well-known factory symbols
 * `fss_transport_create` / `fss_transport_destroy` (fss/transport_abi.h),
 * which a host resolves after dlopening this module.  It bridges two processes
 * with a POSIX shared-memory segment and named-semaphore doorbells.  A frame
 * carries an id, flat data bytes, and an index into a shared table of exact
 * flattened metadata.  Metadata is published once per distinct meta byte
 * sequence; an id alone is not a metadata cache key.
 *
 * The reference implementation is SPSC, with one directed channel per
 * transport instance.  It supports self-contained carriers and pointer
 * carriers: the latter are flattened and serialized by the generic C wire
 * helpers.  Each ring has 256 slots; each slot has a 4096-byte data capacity.
 * The metadata table has eight 2048-byte entries.  These are implementation
 * capacities, not transport-ABI limits.  Named POSIX semaphores are used
 * because process-shared sem_init is unavailable on macOS.
 *
 * `config` selects role + channel + mode:
 *   "create:<channel>"     — creator, ONE-WAY (fire-and-forget SPSC ring):
 *                            shm_open(O_CREAT|O_EXCL) after a defensive
 *                            shm_unlink, ftruncate, mmap, init sems.
 *   "attach:<channel>"     — attacher, one-way.
 *   "create-rr:<channel>"  — creator, ROUND-TRIP: adds a second (reply) ring +
 *                            its sems, so a bound send() is synchronous and
 *                            two-way — it returns the receiver's status and
 *                            copies the (possibly mutated) carrier bytes back
 *                            over the caller's data region (the return-path
 *                            contract in transport_abi.h).  For mutable carriers
 *                            (a request whose Status/read-data is written back);
 *                            one-way is valid only for carriers with no write-
 *                            back (can::frame, sim::event broadcast).
 *   "attach-rr:<channel>"  — attacher, round-trip.
 * The reader thread starts lazily when a target endpoint is attached.
 * `round_trip` is stored in the fixed shared header and both peers must supply
 * the same value: it validates the shared-memory ABI layout (one ring versus
 * request + reply rings).  It does not infer carrier directionality or
 * semantics, which are unavailable to this transport at channel setup time.
 *
 * vtable (fss/transport_abi.h):
 *   channel(name, round_trip, owner, hints, n) — create or attach the shared
 *     medium.  `owner` creates it; a non-owner retries until it exists.
 *   attach(channel, local, INITIATOR) — installs local->send for the request
 *     ring (and reply ring when round_trip is true).
 *   attach(channel, local, TARGET) — installs the receive sink and starts the
 *     reader thread.
 *   send_to(channel, c) — one-shot delivery using the same framing.
 *
 * Artefact construction and endpoint lookup are placement work.  The exported
 * function below remains a compatibility helper for hosts that need to install
 * a SHM receive sink directly.
 */
#ifndef FSS_SHM_H
#define FSS_SHM_H

#include "fss/transport_abi.h"   /* fss_transport_create / fss_transport_destroy */
#include "fss/endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the local endpoint that received carriers are delivered to.  The
 * reader thread calls sink->receive(sink->self, &carrier) for every slot it
 * pulls off the ring.  `sink` must outlive the transport.  The first successful
 * sink remains installed; a repeat with the same pointer is idempotent and a
 * different pointer is ignored by this void compatibility entry point. Exported for the
 * host to wire the SHM receive side. */
FSS_EXPORT void fss_shm_set_sink(fss_transport_t *transport, fss_endpoint_t *sink);

#ifdef __cplusplus
}
#endif
#endif
