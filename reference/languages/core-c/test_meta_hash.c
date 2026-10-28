/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * test_meta_hash.c — prove the runtime C hash + flatten agree with the compiler.
 *
 * Prints two ids computed at runtime from the generated TLM header:
 *   id   = fss_fnv1a64(FSS_TLM_ID_META)   — the runtime hash of the (live) id-meta;
 *          must equal FSS_TLM_ID, the compiler-baked id.
 *   flat = fss_meta_flatten(FSS_TLM_META) — the FLAT carrier id a transport
 *          produces by dropping the non_serializable core::TypeId atom and
 *          re-hashing; must equal what tools/fss_compile.py computes.
 *
 * test_meta_hash_equality.py recomputes both with fss_compile.py and asserts
 * they match bit-for-bit — the contract that the runtime and build-time
 * canonical-CBOR hashes are identical.
 */
#include "fss/c/meta_hash.h"
#include "fss/c/meta.h"
#include "fss/known_carriers/tlm_id.h"

#include <stdint.h>
#include <stdio.h>

int main(void) {
    printf("id %016llx\n",
           (unsigned long long)fss_fnv1a64(FSS_TLM_ID_META, FSS_TLM_ID_META_LEN));
    printf("compiler_id %016llx\n", (unsigned long long)FSS_TLM_ID);

    uint8_t flat_meta[2048];
    uint64_t flat_id = 0;
    long n = fss_meta_flatten(FSS_TLM_META, FSS_TLM_META_LEN,
                              flat_meta, sizeof(flat_meta), &flat_id);
    printf("flat %016llx len %ld\n", (unsigned long long)flat_id, n);
    return 0;
}
