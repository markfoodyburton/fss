/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/c/wire.h — generic carrier serializer for cross-address-space transports.
 *
 * An in-process carrier's data region may contain `fss:local-ptr` fields: 8
 * bytes holding a pointer to bytes OUTSIDE the region (e.g. bus::Data → the TLM
 * payload), with the pointed-to length given by a companion field named in the
 * meta's `attrs.length_at` (docs/spec/02-encodings.md §"In-process pointer
 * inlining").  Such a carrier cannot cross a process/host boundary as-is — the
 * pointer is meaningless to the peer.
 *
 * This pair walks the carrier's meta (via fss/c/meta.h field iteration) and:
 *   - serialize:   inlines every local-ptr field's pointed-to bytes, producing
 *                  a flat, self-contained wire buffer.
 *   - deserialize: rebuilds the data region and repoints each local-ptr field
 *                  at the inlined bytes (laid into a caller-provided scratch
 *                  arena), so the receiver gets a valid in-process carrier.
 *
 * Protocol-INDEPENDENT: one piece of code, driven entirely by the meta — the
 * same design as the meta resolver.  A transport calls these instead of a flat
 * memcpy when a carrier has pointer fields; carriers with none serialize as a
 * verbatim region copy (the fast path is unchanged).
 *
 * Wire format (little-endian lengths):
 *   [u32 region_size][region bytes ...]
 *   then, for each fss:local-ptr field IN META ORDER:
 *     [u32 inlined_len][inlined bytes ...]
 * The region's own pointer bytes are carried verbatim but ignored on decode
 * (overwritten with valid local pointers); the length fields (bus::Length etc.)
 * live in the region and survive the round-trip unchanged.
 */
#ifndef FSS_C_WIRE_H
#define FSS_C_WIRE_H

#include <stdint.h>
#include <stddef.h>

#include "fss/carrier.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize `carrier` (data region + any pointed-to local-ptr bytes) into
 * `out` (capacity out_cap).  Returns the byte count written, or -1 on error
 * (malformed meta, buffer too small, or a local-ptr field with a nonzero
 * length but a NULL pointer). */
long fss_carrier_serialize(const void *meta, uint64_t meta_size,
                           const fss_carrier_t *carrier,
                           uint8_t *out, size_t out_cap);

/* Deserialize a wire buffer (from fss_carrier_serialize) back into an
 * in-process data region.  `region` receives the flat region bytes; each
 * local-ptr field in it is repointed into `scratch` (a caller-provided arena
 * holding the inlined bytes).  Returns the region size, or -1 on error
 * (malformed wire, region/scratch too small).  Both `region` and `scratch`
 * must outlive any use of the resulting carrier. */
long fss_carrier_deserialize(const void *meta, uint64_t meta_size,
                             const uint8_t *wire, size_t wire_len,
                             uint8_t *region, size_t region_cap,
                             uint8_t *scratch, size_t scratch_cap);

/* True (1) if `meta` has at least one fss:local-ptr / -nul field — i.e. the
 * carrier needs serialization to cross a boundary.  0 if it is self-contained
 * (a verbatim region copy suffices) or the meta is malformed. */
int fss_carrier_has_pointers(const void *meta, uint64_t meta_size);

/* Apply a reply wire buffer (the peer's serialization of the carrier AFTER it
 * mutated it — e.g. a TLM b_transport writing Status + read data) back onto the
 * caller's ORIGINAL in-process `carrier`, in place: scalar/region fields are
 * copied over carrier->data_ptr, and each inlined pointer field's bytes are
 * copied over the caller's existing pointed-to buffer (bounded by the original
 * length).  This is the write-back half of the return-path contract for
 * pointer-carriers — the read data lands in the caller's payload buffer, not a
 * fresh one.  Returns 0 on success, -1 on error.  A carrier with no pointer
 * fields reduces to copying the region bytes back. */
int fss_carrier_apply_reply(const void *meta, uint64_t meta_size,
                            fss_carrier_t *carrier,
                            const uint8_t *wire, size_t wire_len);

#ifdef __cplusplus
}
#endif
#endif /* FSS_C_WIRE_H */
