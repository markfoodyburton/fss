/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * wire.c — implementation of the generic carrier serializer (fss/c/wire.h).
 *
 * Driven by the carrier meta (fss/c/meta.h field iteration): walk the fields,
 * carry the fixed data region verbatim, and inline each fss:local-ptr field's
 * pointed-to bytes.  See wire.h for the wire format.
 */
#include "fss/c/wire.h"
#include "fss/c/meta.h"

#include <limits.h>
#include <string.h>

/* Read a little-endian uint of `size` bytes (1/2/4/8) from p. */
static uint64_t read_le(const uint8_t *p, uint32_t size) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < size && i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int is_local_ptr(const fss_meta_field_t *f) {
    return strcmp(f->layout, "fss:local-ptr") == 0
        || strcmp(f->layout, "fss:local-ptr-nul") == 0;
}

/* Resolve the in-region byte length of a local-ptr field by reading its
 * companion length field (attrs.length_at) from the data region.  A -nul
 * pointer with no length_at falls back to strlen()+1 of the pointed-to bytes.
 * Stores the u32 wire length in out and returns 1, or returns 0 on error. */
static int ptr_length(const void *meta, uint64_t meta_size,
                      const fss_meta_field_t *f,
                      const uint8_t *region, size_t region_size,
                      uint32_t *out) {
    if (f->length_at[0]) {
        uint32_t loff = 0, lsize = 0;
        uint64_t len;
        if (!fss_meta_find(meta, meta_size, f->length_at, &loff, &lsize)) return 0;
        if (lsize > 8 || loff > region_size || lsize > region_size - loff) return 0;
        len = read_le(region + loff, lsize);
        if (len > UINT32_MAX) return 0;
        *out = (uint32_t)len;
        return 1;
    }
    return 0;   /* length_at is required for inlining in this profile */
}

/* Return the capacity supplied by the caller for reply write-back.  NUL
 * strings without length_at use their existing terminating NUL as the bound,
 * as they do during serialization. */
static int ptr_capacity(const void *meta, uint64_t meta_size,
                        const fss_meta_field_t *f,
                        const uint8_t *region, size_t region_size,
                        const uint8_t *ptr, uint32_t *out) {
    if (!ptr) { *out = 0; return 1; }
    if (ptr_length(meta, meta_size, f, region, region_size, out)) return 1;
    if (strcmp(f->layout, "fss:local-ptr-nul") != 0) return 0;
    size_t len = strlen((const char *)ptr);
    if (len == SIZE_MAX || len + 1 > UINT32_MAX) return 0;
    *out = (uint32_t)(len + 1);
    return 1;
}

int fss_carrier_has_pointers(const void *meta, uint64_t meta_size) {
    int n = fss_meta_field_count(meta, meta_size);
    for (int i = 0; i < n; ++i) {
        fss_meta_field_t f;
        if (!fss_meta_field_at(meta, meta_size, i, &f)) return 0;
        if (is_local_ptr(&f)) return 1;
    }
    return 0;
}

/* Append `n` bytes to out at *off, bounded by cap.  Returns 0 on overflow. */
static int put(uint8_t *out, size_t *off, size_t cap, const void *src, size_t n) {
    if (*off > cap || n > cap - *off) return 0;
    if (n) memcpy(out + *off, src, n);
    *off += n;
    return 1;
}

static int put_u32(uint8_t *out, size_t *off, size_t cap, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return put(out, off, cap, b, 4);
}

long fss_carrier_serialize(const void *meta, uint64_t meta_size,
                           const fss_carrier_t *carrier,
                           uint8_t *out, size_t out_cap) {
    if (!meta || !carrier || !out) return -1;
    const uint8_t *region = (const uint8_t *)carrier->data_ptr;
    if (carrier->data_size > UINT32_MAX) return -1;
    uint32_t region_size = (uint32_t)carrier->data_size;
    if (!region && region_size) return -1;

    size_t off = 0;
    if (!put_u32(out, &off, out_cap, region_size)) return -1;
    if (!put(out, &off, out_cap, region, region_size)) return -1;

    /* Inline each local-ptr field's pointed-to bytes, in meta order. */
    int n = fss_meta_field_count(meta, meta_size);
    for (int i = 0; i < n; ++i) {
        fss_meta_field_t f;
        if (!fss_meta_field_at(meta, meta_size, i, &f)) return -1;
        if (!is_local_ptr(&f)) continue;
        if (f.byte_offset > region_size || f.byte_size > region_size - f.byte_offset ||
            f.byte_size < sizeof(void *)) return -1;

        const uint8_t *pp = NULL;
        memcpy(&pp, region + f.byte_offset, sizeof(pp));   /* the pointer value */

        /* A NULL local-ptr carries nothing — inline length 0 (e.g. config's
         * ParamName when the request names no parameter).  Otherwise the length is
         * the companion length_at field, or — for a -nul string with no length_at —
         * strlen()+1 of the pointed-to bytes. */
        uint32_t len;
        if (!pp) {
            len = 0;
        } else {
            if (!ptr_length(meta, meta_size, &f, region, region_size, &len)) {
                if (strcmp(f.layout, "fss:local-ptr-nul") != 0) return -1;
                size_t nul_len = strlen((const char *)pp);
                if (nul_len == SIZE_MAX || nul_len + 1 > UINT32_MAX) return -1;
                len = (uint32_t)(nul_len + 1);
            }
        }

        if (!put_u32(out, &off, out_cap, len)) return -1;
        if (len > 0 && !put(out, &off, out_cap, pp, len)) return -1;
    }
    return (long)off;
}

/* Read `n` bytes from wire at *off, bounded by len.  Returns 0 on overrun. */
static int get(const uint8_t *wire, size_t *off, size_t len, void *dst, size_t n) {
    if (*off > len || n > len - *off) return 0;
    if (n) memcpy(dst, wire + *off, n);
    *off += n;
    return 1;
}

static int get_u32(const uint8_t *wire, size_t *off, size_t len, uint32_t *v) {
    uint8_t b[4];
    if (!get(wire, off, len, b, 4)) return 0;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

long fss_carrier_deserialize(const void *meta, uint64_t meta_size,
                             const uint8_t *wire, size_t wire_len,
                             uint8_t *region, size_t region_cap,
                             uint8_t *scratch, size_t scratch_cap) {
    if (!meta || !wire || !region) return -1;

    size_t woff = 0;
    uint32_t region_size = 0;
    if (!get_u32(wire, &woff, wire_len, &region_size)) return -1;
    if (region_size > region_cap) return -1;
    if (!get(wire, &woff, wire_len, region, region_size)) return -1;

    /* Lay each inlined local-ptr field into scratch and repoint the region. */
    size_t soff = 0;
    int n = fss_meta_field_count(meta, meta_size);
    for (int i = 0; i < n; ++i) {
        fss_meta_field_t f;
        if (!fss_meta_field_at(meta, meta_size, i, &f)) return -1;
        if (!is_local_ptr(&f)) continue;
        if (f.byte_offset > region_size || f.byte_size > region_size - f.byte_offset ||
            f.byte_size < sizeof(void *)) return -1;

        uint32_t len = 0;
        if (!get_u32(wire, &woff, wire_len, &len)) return -1;

        uint8_t *dst = NULL;
        if (len > 0) {
            if (!scratch || soff > scratch_cap || len > scratch_cap - soff) return -1;
            dst = scratch + soff;
            if (!get(wire, &woff, wire_len, dst, len)) return -1;
            soff += len;
        }
        /* Write the (local) pointer into the region's pointer slot. */
        memcpy(region + f.byte_offset, &dst, sizeof(dst));
    }
    return (long)region_size;
}

int fss_carrier_apply_reply(const void *meta, uint64_t meta_size,
                            fss_carrier_t *carrier,
                            const uint8_t *wire, size_t wire_len) {
    if (!meta || !carrier || !wire) return -1;
    uint8_t *region = (uint8_t *)carrier->data_ptr;
    if (!region || carrier->data_size > UINT32_MAX) return -1;
    uint32_t region_size = (uint32_t)carrier->data_size;

    size_t woff = 0;
    uint32_t wire_region = 0;
    if (!get_u32(wire, &woff, wire_len, &wire_region)) return -1;
    if (wire_region != region_size) return -1;   /* shape mismatch */
    if (woff > wire_len || wire_region > wire_len - woff) return -1;

    /* Copy scalar/region fields back, but PRESERVE the caller's pointer values
     * (the region's pointer slots must keep pointing at the caller's buffers).
     * Snapshot the pointer slots, overlay the region, restore them, then copy
     * each field's replied bytes into the caller's existing buffer. */
    int n = fss_meta_field_count(meta, meta_size);
    const uint8_t *wregion = wire + woff;

    /* Save the caller's original pointers and capacities before overlaying the
     * region.  The peer controls the replacement length_at values, so they
     * must never be used as capacities for caller-owned buffers. */
    uint8_t *saved_ptr[64];
    uint32_t saved_off[64];
    uint32_t saved_cap[64];
    int      nsaved = 0;
    for (int i = 0; i < n; ++i) {
        fss_meta_field_t f;
        if (!fss_meta_field_at(meta, meta_size, i, &f)) return -1;
        if (!is_local_ptr(&f)) continue;
        if (f.byte_offset > region_size || f.byte_size > region_size - f.byte_offset ||
            f.byte_size < sizeof(void *)) return -1;
        if (nsaved >= 64) return -1;
        uint8_t *pp = NULL;
        memcpy(&pp, region + f.byte_offset, sizeof(pp));
        uint32_t cap = 0;
        if (!ptr_capacity(meta, meta_size, &f, region, region_size, pp, &cap))
            return -1;
        saved_ptr[nsaved] = pp;
        saved_off[nsaved] = f.byte_offset;
        saved_cap[nsaved] = cap;
        nsaved++;
    }

    /* Validate all inlined reply data against the original caller-owned
     * buffers before the region overlay.  This also leaves the carrier intact
     * on malformed or oversized replies. */
    size_t check_off = woff + region_size;
    int si = 0;
    for (int i = 0; i < n; ++i) {
        fss_meta_field_t f;
        if (!fss_meta_field_at(meta, meta_size, i, &f)) return -1;
        if (!is_local_ptr(&f)) continue;
        uint32_t len = 0;
        if (!get_u32(wire, &check_off, wire_len, &len)) return -1;
        if (check_off > wire_len || len > wire_len - check_off) return -1;
        if ((len != 0 && !saved_ptr[si]) || len > saved_cap[si]) return -1;
        check_off += len;
        si++;
    }

    /* Overlay region scalars (Status, read-length, etc.). */
    woff += region_size;
    memcpy(region, wregion, region_size);

    /* Restore the caller's pointers and copy each field's inlined bytes back
     * into the caller's existing buffer (bounded by the original length). */
    si = 0;
    for (int i = 0; i < n; ++i) {
        fss_meta_field_t f;
        if (!fss_meta_field_at(meta, meta_size, i, &f)) return -1;
        if (!is_local_ptr(&f)) continue;

        uint8_t *orig = saved_ptr[si];
        memcpy(region + saved_off[si], &orig, sizeof(orig));   /* restore pointer */
        si++;

        uint32_t len = 0;
        if (!get_u32(wire, &woff, wire_len, &len)) return -1;
        if (woff > wire_len || len > wire_len - woff) return -1;
        if (len > 0 && orig) {
            memcpy(orig, wire + woff, len);
        }
        woff += len;
    }
    return 0;
}
