/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/carrier.h — the FSS carrier: data envelope passed between
 * endpoints (fss/endpoint.h) by a transport (fss/transport_abi.h).
 *
 * Strict ABI, header-only.  `data_ptr` and `meta_ptr` carry pointers
 * in-memory and offsets on the wire — same byte layout, two
 * interpretations.
 */
#ifndef FSS_CARRIER_H
#define FSS_CARRIER_H

#include <stdint.h>
#include <stddef.h>

#include "fss/abi_guard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fss_carrier {
    uint64_t  id;
    void     *data_ptr;
    uint64_t  data_size;
    void     *meta_ptr;
    uint64_t  meta_size;
} fss_carrier_t;

/* The normative carrier layout supports only 64-bit C ABI participants. */
FSS_ABI_STATIC_ASSERT(sizeof(void *) == 8,
                      "FSS ABI unsupported layout: requires 64-bit pointers");
FSS_ABI_STATIC_ASSERT(sizeof(fss_carrier_t) == 40,
                      "FSS ABI unsupported layout: fss_carrier_t must be 40 bytes");
FSS_ABI_STATIC_ASSERT(offsetof(fss_carrier_t, id) == 0,
                      "FSS ABI unsupported layout: fss_carrier_t.id offset must be 0");
FSS_ABI_STATIC_ASSERT(offsetof(fss_carrier_t, data_ptr) == 8,
                      "FSS ABI unsupported layout: fss_carrier_t.data_ptr offset must be 8");
FSS_ABI_STATIC_ASSERT(offsetof(fss_carrier_t, data_size) == 16,
                      "FSS ABI unsupported layout: fss_carrier_t.data_size offset must be 16");
FSS_ABI_STATIC_ASSERT(offsetof(fss_carrier_t, meta_ptr) == 24,
                      "FSS ABI unsupported layout: fss_carrier_t.meta_ptr offset must be 24");
FSS_ABI_STATIC_ASSERT(offsetof(fss_carrier_t, meta_size) == 32,
                      "FSS ABI unsupported layout: fss_carrier_t.meta_size offset must be 32");

#ifdef __cplusplus
}
#endif
#endif
