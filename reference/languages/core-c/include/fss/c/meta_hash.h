/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/c/meta_hash.h — runtime carrier-id hashing (FNV-1a-64 over canonical CBOR).
 *
 * The carrier id is hash(canonical-CBOR meta).  tools/fss_compile.py computes it
 * at build time; this header computes it at RUNTIME, which a transport needs when
 * it transforms a meta — drops non-serialisable atoms (e.g. core::TypeId) and
 * inlines fss:local-ptr payloads — and must re-derive the resulting (flat)
 * carrier id (see fss_meta_flatten in fss/c/meta.h, and docs/spec/02-encodings.md
 * §core::TypeId).
 *
 * This is header-only and dependency-free so each binding/transport carries its
 * OWN copy (FSS does not share a runtime lib).  It MUST stay bit-for-bit
 * identical to tools/fss_compile.py: same FNV-1a-64, same canonical CBOR
 * (definite lengths; map keys sorted by encoded-key bytes).  The c_meta_hash
 * equality test pins that agreement.
 */
#ifndef FSS_C_META_HASH_H
#define FSS_C_META_HASH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* FNV-1a-64 — matches fss_compile.fnv1a_64. */
static inline uint64_t fss_fnv1a64(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

/* Write a CBOR head (major type + argument, definite form); return bytes used. */
static inline size_t fss__cbor_head(uint8_t *b, int major, uint64_t v) {
    uint8_t m = (uint8_t)(major << 5);
    if (v < 24)              { b[0] = m | (uint8_t)v; return 1; }
    if (v < 0x100ULL)        { b[0] = m | 24; b[1] = (uint8_t)v; return 2; }
    if (v < 0x10000ULL)      { b[0] = m | 25; b[1] = (uint8_t)(v>>8); b[2] = (uint8_t)v; return 3; }
    if (v < 0x100000000ULL)  { b[0] = m | 26; for (int i=0;i<4;i++) b[1+i] = (uint8_t)(v >> (24-8*i)); return 5; }
    b[0] = m | 27; for (int i=0;i<8;i++) b[1+i] = (uint8_t)(v >> (56-8*i)); return 9;
}

/* The carrier id is FNV-1a-64 over the canonical CBOR id-meta.  The compiler
 * (tools/fss_compile.py) computes it at build time; a transport recomputes it
 * at runtime after transforming a meta — e.g. fss_meta_flatten (fss/c/meta.h)
 * drops the non_serializable core::TypeId atom and calls fss_fnv1a64 on the
 * reduced meta to derive the flat carrier id.  fss_fnv1a64 + fss__cbor_head
 * above are the only pieces that needs (the flatten copies entry spans
 * verbatim, so no full CBOR re-encoder is required). */

#endif /* FSS_C_META_HASH_H */
