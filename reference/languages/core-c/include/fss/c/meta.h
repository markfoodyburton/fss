/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/c/meta.h — carrier-agnostic meta resolver for the C language.
 *
 * Path B of the consumption model (docs/spec/05-abi-conventions.md §"Consuming
 * a carrier's fields"): a receiver that does NOT recognise carrier->id walks
 * the carrier's CBOR meta once to resolve a field's byte_offset/byte_size by
 * qname, caching the result keyed by (id, qname) so later lookups are O(1).
 *
 * This is protocol-INDEPENDENT — one piece of code per language, not per
 * carrier.  It is the generic resolver of docs/design/open-issues.md §10; the
 * SystemC receiver is its first consumer.
 *
 * It decodes only the closed CBOR profile that tools/fss_compile.py emits:
 * definite-length arrays and maps, text strings (major 3), unsigned ints
 * (major 0), and booleans (major 7).  It is read-only and bounded by the
 * meta size; malformed input fails the lookup rather than reading past the
 * buffer.  Sub-meta entries (no byte_offset/byte_size) are skipped.
 */
#ifndef FSS_C_META_H
#define FSS_C_META_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve `qname` in the CBOR `meta` (meta_size bytes).  On success writes
 * byte_offset/byte_size and returns 1; returns 0 if the qname is absent, the
 * entry is a sub-meta (no data bytes), or the meta is malformed. */
int fss_meta_find(const void *meta, uint64_t meta_size, const char *qname,
                  uint32_t *byte_offset, uint32_t *byte_size);

/* A small (id, qname) → (offset, size) cache over fss_meta_find, so repeated
 * lookups for the same carrier are O(1).  Caller supplies the cache; it holds
 * a fixed number of entries (older entries evicted).  Not thread-safe; one
 * cache per consuming thread/endpoint. */
typedef struct fss_meta_cache fss_meta_cache_t;

fss_meta_cache_t *fss_meta_cache_create(void);
void              fss_meta_cache_destroy(fss_meta_cache_t *cache);

/* As fss_meta_find, but consults/updates the cache under key (id, qname). */
int fss_meta_resolve(fss_meta_cache_t *cache, uint64_t id,
                     const void *meta, uint64_t meta_size, const char *qname,
                     uint32_t *byte_offset, uint32_t *byte_size);

/* ---- field iteration (for a generic carrier serializer) ----------------- *
 *
 * fss_meta_find answers "where is field X"; a wire serializer instead needs to
 * walk EVERY field and learn which ones are pointers (layout fss:local-ptr /
 * -nul) and where each pointer's length lives (attrs.length_at).  These two
 * calls expose exactly that — the layout + length_at the resolver otherwise
 * skips — without disturbing the by-qname API above.
 */

/* One data-region field of a carrier meta, as needed to serialize it.
 * `layout` / `length_at` are empty strings when the entry has none.  A sub-meta
 * entry (no data bytes) is reported with is_submeta=1 and zero offset/size. */
typedef struct {
    char     qname[64];
    uint32_t byte_offset;
    uint32_t byte_size;     /* in-region width (8 for a local-ptr: the pointer) */
    char     layout[24];    /* "", "fss:local-ptr", "fss:local-ptr-nul", "sub-meta" */
    char     length_at[64]; /* attrs.length_at qname, or "" */
    int      is_submeta;
} fss_meta_field_t;

/* Number of top-level entries in `meta` (data fields + sub-meta slots), or 0 if
 * malformed.  Pair with fss_meta_field_at to iterate. */
int fss_meta_field_count(const void *meta, uint64_t meta_size);

/* Fill `out` with the entry at index `idx` (0-based, declaration order).
 * Returns 1 on success, 0 if idx is out of range or the meta is malformed. */
int fss_meta_field_at(const void *meta, uint64_t meta_size, int idx,
                      fss_meta_field_t *out);

/* ---- flattening (the transport's "construct the flat carrier" step) ------- *
 *
 * A live carrier carries non-serialisable atoms (core::TypeId — the vtable
 * marker).  A wire transport must DROP them before transmitting, and the
 * resulting carrier is a DISTINCT carrier with its OWN id.  fss_meta_flatten
 * produces that flat meta — every entry whose `serializable` is false is
 * removed — and its id.  The flat meta retains `in_hash:false` entries and
 * nested sub-meta exactly as transmitted.  Its id is instead FNV-1a-64 over
 * the compiler's id projection: `in_hash:false` entries are removed, and only
 * the nested `submeta` value is removed from each retained sub-meta marker.
 * This lets the transport stamp the wire frame with the flat id instead of the live id.  Without this a
 * receiver would see the live id, take the zero-copy fast path, and cast
 * deserialised flat bytes as a live object.  Writes the flat meta CBOR to
 * out[out_cap]; returns its length, or -1 (malformed / buffer too small).
 * *flat_id (if non-NULL) receives the flat carrier id.
 *
 * Input must be compiler-style canonical CBOR.  Non-canonical or malformed
 * input fails rather than being copied into a carrier id. */
long fss_meta_flatten(const void *meta, uint64_t meta_size,
                      uint8_t *out, size_t out_cap, uint64_t *flat_id);

#ifdef __cplusplus
}
#endif
#endif /* FSS_C_META_H */
