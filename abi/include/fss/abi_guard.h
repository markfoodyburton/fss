/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Compile-time ABI guard support for the normative FSS C headers. */
#ifndef FSS_ABI_GUARD_H
#define FSS_ABI_GUARD_H

#if defined(__cplusplus) && __cplusplus >= 201103L
#  define FSS_ABI_STATIC_ASSERT(condition, message) static_assert(condition, message)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define FSS_ABI_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#else
/* C99 and C++98 have no native static assertion.  A negative array bound is
 * ill-formed, so this preserves each layout check at translation time. */
#  define FSS_ABI_STATIC_ASSERT_JOIN_(left, right) left ## right
#  define FSS_ABI_STATIC_ASSERT_JOIN(left, right) \
    FSS_ABI_STATIC_ASSERT_JOIN_(left, right)
#  define FSS_ABI_STATIC_ASSERT(condition, message) \
    typedef char FSS_ABI_STATIC_ASSERT_JOIN(fss_abi_static_assertion_, __LINE__) \
        [(condition) ? 1 : -1]
#endif

#endif
