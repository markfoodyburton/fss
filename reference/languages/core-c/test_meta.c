/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * test_meta.c — unit test for the carrier-agnostic meta resolver (fss/c/meta.h).
 *
 * Uses the build-time-generated FSS_TLM_META (the canonical CBOR meta of the
 * TLM carrier) as a real, non-trivial input and checks that resolving the
 * bus:: fields by qname yields the expected offsets from the composition
 * (known_carriers/tlm/transaction.jsonnet).
 *
 * Path B (resolve-from-meta) is the assertion; the expected offsets are
 * hard-coded locally to match the composition (no longer cross-checked against
 * a now-deleted hand-written header).
 */
#include "fss/c/meta.h"
#include "fss/c/meta_hash.h"
#include "fss/known_carriers/tlm_id.h"   /* generated: FSS_TLM_ID / FSS_TLM_META */

#include <stdio.h>
#include <string.h>

/* Expected offsets from known_carriers/tlm/transaction.jsonnet — the COMPLETE
 * (live) carrier: real tlm_generic_payload offsets, core::TypeId (vptr) at 0. */
enum {
    EXP_TYPEID    =  0,
    EXP_LOCATOR   =  8,
    EXP_OPERATION = 16,
    EXP_DATA      = 24,
    EXP_LENGTH    = 32,
    EXP_STATUS    = 36,
    EXP_DMI       = 40,
    EXP_BE        = 48,
    EXP_BE_LEN    = 56,
    EXP_STREAMING = 60,
    EXP_GPOPT     = 64,
};

static int check(const char *qname, uint32_t want_off, int *fails) {
    uint32_t off = 0xffffffffu, size = 0;
    int ok = fss_meta_find(FSS_TLM_META, FSS_TLM_META_LEN, qname, &off, &size);
    if (!ok || off != want_off) {
        printf("  FAIL %-24s resolved=%d off=%u (want %u)\n", qname, ok, off, want_off);
        (*fails)++;
        return 0;
    }
    printf("  ok   %-24s off=%u size=%u\n", qname, off, size);
    return 1;
}

int main(void) {
    int fails = 0;

    /* CBOR field offsets and sizes are wire u32 values, not truncating casts. */
    static const uint8_t oversized_offset[] = {
        0x81, 0xa3, 0x65, 'q','n','a','m','e', 0x61, 'x',
        0x6b, 'b','y','t','e','_','o','f','f','s','e','t',
        0x1b, 0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,
        0x69, 'b','y','t','e','_','s','i','z','e', 0x01
    };
    static const uint8_t oversized_size[] = {
        0x81, 0xa3, 0x65, 'q','n','a','m','e', 0x61, 'x',
        0x6b, 'b','y','t','e','_','o','f','f','s','e','t', 0x00,
        0x69, 'b','y','t','e','_','s','i','z','e',
        0x1b, 0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00
    };
    static const uint8_t malformed_serializable[] = {
        0x81, 0xa1, 0x6c, 's','e','r','i','a','l','i','z','a','b','l','e', 0xf6
    };
    static const uint8_t true_serializable[] = {
        0x81, 0xa1, 0x6c, 's','e','r','i','a','l','i','z','a','b','l','e', 0xf5
    };
    static const uint8_t false_serializable[] = {
        0x81, 0xa1, 0x6c, 's','e','r','i','a','l','i','z','a','b','l','e', 0xf4
    };
    static const uint8_t noncanonical_empty_array[] = { 0x98, 0x00 };
    /* Compiler-parity vectors.  The live form has TypeId, a sub-meta marker
     * with nested content, an in_hash:false field, and a normal field. */
    static const uint8_t parity_live[] = {
        0x84,
        0xa2, 0x65,'q','n','a','m','e', 0x6c,'c','o','r','e',':',':','T','y','p','e','I','d',
              0x6c,'s','e','r','i','a','l','i','z','a','b','l','e', 0xf4,
        0xa3, 0x65,'q','n','a','m','e', 0x70,'c','o','r','e',':',':','E','x','t','e','n','s','i','o','n','s',
              0x66,'l','a','y','o','u','t', 0x68,'s','u','b','-','m','e','t','a',
              0x67,'s','u','b','m','e','t','a', 0x80,
        0xa2, 0x65,'q','n','a','m','e', 0x61,'x', 0x67,'i','n','_','h','a','s','h', 0xf4,
        0xa1, 0x65,'q','n','a','m','e', 0x61,'y'
    };
    static const uint8_t parity_flat[] = {
        0x83,
        0xa3, 0x65,'q','n','a','m','e', 0x70,'c','o','r','e',':',':','E','x','t','e','n','s','i','o','n','s',
              0x66,'l','a','y','o','u','t', 0x68,'s','u','b','-','m','e','t','a',
              0x67,'s','u','b','m','e','t','a', 0x80,
        0xa2, 0x65,'q','n','a','m','e', 0x61,'x', 0x67,'i','n','_','h','a','s','h', 0xf4,
        0xa1, 0x65,'q','n','a','m','e', 0x61,'y'
    };
    static const uint8_t parity_id_meta[] = {
        0x82,
        0xa2, 0x65,'q','n','a','m','e', 0x70,'c','o','r','e',':',':','E','x','t','e','n','s','i','o','n','s',
              0x66,'l','a','y','o','u','t', 0x68,'s','u','b','-','m','e','t','a',
        0xa1, 0x65,'q','n','a','m','e', 0x61,'y'
    };
    /* Same serialised shape with TypeId absent. */
    const uint8_t *parity_no_typeid = parity_flat;
    size_t parity_no_typeid_len = sizeof(parity_flat);
    /* Same marker but non-empty nested meta; it changes bytes, not the id. */
    uint8_t parity_nested[sizeof(parity_flat) + 9];
    uint8_t flat[16];
    if (fss_meta_find(oversized_offset, sizeof(oversized_offset), "x", NULL, NULL) ||
        fss_meta_find(oversized_size, sizeof(oversized_size), "x", NULL, NULL)) {
        printf("  FAIL oversized CBOR field offset/size was accepted\n"); fails++;
    } else {
        printf("  ok   oversized CBOR field offset/size rejected\n");
    }
    if (fss_meta_flatten(malformed_serializable, sizeof(malformed_serializable),
                         flat, sizeof(flat), NULL) >= 0) {
        printf("  FAIL malformed serializable value was accepted\n"); fails++;
    } else {
        printf("  ok   malformed serializable value rejected\n");
    }
    if (fss_meta_flatten(true_serializable, sizeof(true_serializable),
                         flat, sizeof(flat), NULL) != (long)sizeof(true_serializable) ||
        fss_meta_flatten(false_serializable, sizeof(false_serializable),
                         flat, sizeof(flat), NULL) != 1 || flat[0] != 0x80) {
        printf("  FAIL proper serializable booleans were not handled\n"); fails++;
    } else {
        printf("  ok   proper serializable booleans accepted\n");
    }
    if (fss_meta_flatten(noncanonical_empty_array, sizeof(noncanonical_empty_array),
                         flat, sizeof(flat), NULL) >= 0) {
        printf("  FAIL non-canonical CBOR was accepted\n"); fails++;
    } else {
        printf("  ok   non-canonical CBOR rejected\n");
    }
    {
        uint64_t id_live = 0, id_absent = 0, id_nested = 0;
        uint8_t parity_out[512];
        long n_live = fss_meta_flatten(parity_live, sizeof(parity_live), parity_out, sizeof(parity_out), &id_live);
        long n_absent = fss_meta_flatten(parity_no_typeid, parity_no_typeid_len, parity_out, sizeof(parity_out), &id_absent);
        /* Replace submeta: [] with [{qname:"z"}], retaining canonical order. */
        size_t p = 0;
        for (size_t i = 0; i < sizeof(parity_flat); ++i) {
            if (i + 1 < sizeof(parity_flat) && parity_flat[i] == 0x67 && parity_flat[i + 1] == 's' &&
                i + 8 < sizeof(parity_flat) && parity_flat[i + 8] == 0x80) {
                memcpy(parity_nested + p, parity_flat + i, 8); p += 8;
                parity_nested[p++] = 0x81; parity_nested[p++] = 0xa1;
                parity_nested[p++] = 0x65; memcpy(parity_nested + p, "qname", 5); p += 5;
                parity_nested[p++] = 0x61; parity_nested[p++] = 'z';
                i += 8;
            } else parity_nested[p++] = parity_flat[i];
        }
        long n_nested = fss_meta_flatten(parity_nested, p, parity_out, sizeof(parity_out), &id_nested);
        if (n_live != (long)sizeof(parity_flat) ||
            fss_meta_flatten(parity_live, sizeof(parity_live), parity_out, sizeof(parity_out), NULL) != n_live ||
            memcmp(parity_out, parity_flat, sizeof(parity_flat)) != 0 ||
            n_absent != (long)sizeof(parity_flat) || id_live != id_absent ||
            id_live != fss_fnv1a64(parity_id_meta, sizeof(parity_id_meta)) ||
            n_nested <= 0 || id_nested != id_live) {
            printf("  FAIL compiler parity flatten/id vectors\n"); fails++;
        } else {
            printf("  ok   TypeId, nested submeta, and in_hash:false parity vectors\n");
        }
    }

    printf("resolve bus:: fields from FSS_TLM_META by qname:\n");
    check("bus::Locator",          EXP_LOCATOR,   &fails);
    check("bus::Operation",        EXP_OPERATION, &fails);
    check("bus::Length",           EXP_LENGTH,    &fails);
    check("bus::Data",             EXP_DATA,      &fails);
    check("bus::Status",           EXP_STATUS,    &fails);
    check("bus::StreamingWidth",   EXP_STREAMING, &fails);
    check("bus::ByteEnable",       EXP_BE,        &fails);
    check("bus::ByteEnableLength", EXP_BE_LEN,    &fails);
    check("bus::DmiAllowed",       EXP_DMI,       &fails);
    check("tlm::GpOption",         EXP_GPOPT,     &fails);

    /* A sub-meta entry (core::Extensions) must NOT resolve as a data field. */
    uint32_t o, s;
    if (fss_meta_find(FSS_TLM_META, FSS_TLM_META_LEN, "core::Extensions", &o, &s)) {
        printf("  FAIL core::Extensions resolved as a data field (it is a sub-meta)\n");
        fails++;
    } else {
        printf("  ok   core::Extensions correctly not a data field\n");
    }

    /* An absent qname must fail. */
    if (fss_meta_find(FSS_TLM_META, FSS_TLM_META_LEN, "bus::Nonexistent", &o, &s)) {
        printf("  FAIL absent qname resolved\n");
        fails++;
    }

    /* Cache path agrees with direct find. */
    fss_meta_cache_t *cache = fss_meta_cache_create();
    uint32_t c1 = 0, c2 = 0, sz;
    fss_meta_resolve(cache, FSS_TLM_ID, FSS_TLM_META, FSS_TLM_META_LEN, "bus::Status", &c1, &sz);
    fss_meta_resolve(cache, FSS_TLM_ID, FSS_TLM_META, FSS_TLM_META_LEN, "bus::Status", &c2, &sz); /* cached */
    if (c1 != EXP_STATUS || c2 != EXP_STATUS) {
        printf("  FAIL cache resolve off=%u/%u (want %u)\n", c1, c2, EXP_STATUS);
        fails++;
    } else {
        printf("  ok   cache resolve bus::Status off=%u (both lookups)\n", c1);
    }
    fss_meta_cache_destroy(cache);

    /* Long qnames that share the old fixed-cache prefix must stay distinct. */
    {
        uint8_t long_meta[256];
        char long_a[65], long_b[65];
        size_t p = 0;
        memset(long_a, 'q', sizeof(long_a) - 1);
        memcpy(long_b, long_a, sizeof(long_a));
        long_a[63] = 'a'; long_b[63] = 'b';
        long_a[64] = long_b[64] = '\0';
        long_meta[p++] = 0x82;             /* array(2) */
        for (int entry = 0; entry < 2; ++entry) {
            const char *name = entry ? long_b : long_a;
            long_meta[p++] = 0xA3;         /* map(3) */
            long_meta[p++] = 0x65; memcpy(long_meta + p, "qname", 5); p += 5;
            long_meta[p++] = 0x78; long_meta[p++] = 64;
            memcpy(long_meta + p, name, 64); p += 64;
            long_meta[p++] = 0x6B; memcpy(long_meta + p, "byte_offset", 11); p += 11;
            long_meta[p++] = (uint8_t)(entry + 1);
            long_meta[p++] = 0x69; memcpy(long_meta + p, "byte_size", 9); p += 9;
            long_meta[p++] = 1;
        }
        cache = fss_meta_cache_create();
        uint32_t lo_a = 0, lo_b = 0;
        int long_ok = cache &&
            fss_meta_resolve(cache, 0x1234, long_meta, p, long_a, &lo_a, NULL) &&
            fss_meta_resolve(cache, 0x1234, long_meta, p, long_b, &lo_b, NULL) &&
            lo_a == 1 && lo_b == 2;
        fss_meta_cache_destroy(cache);
        if (!long_ok) { printf("  FAIL long qname cache prefix collision\n"); fails++; }
        else          printf("  ok   long qname cache entries match exactly\n");
    }

    /* Field iteration surfaces layout + length_at (what a wire serializer needs).
     * bus::Data must report layout fss:local-ptr with length_at bus::Length. */
    int nf = fss_meta_field_count(FSS_TLM_META, FSS_TLM_META_LEN);
    if (nf <= 0) {
        printf("  FAIL fss_meta_field_count returned %d\n", nf);
        fails++;
    } else {
        int saw_data_ptr = 0, saw_be_ptr = 0, saw_submeta = 0;
        for (int i = 0; i < nf; ++i) {
            fss_meta_field_t f;
            if (!fss_meta_field_at(FSS_TLM_META, FSS_TLM_META_LEN, i, &f)) {
                printf("  FAIL fss_meta_field_at(%d)\n", i); fails++; continue;
            }
            if (strcmp(f.qname, "bus::Data") == 0) {
                saw_data_ptr = (strcmp(f.layout, "fss:local-ptr") == 0 &&
                                strcmp(f.length_at, "bus::Length") == 0 &&
                                f.byte_offset == EXP_DATA);
            } else if (strcmp(f.qname, "bus::ByteEnable") == 0) {
                saw_be_ptr = (strcmp(f.layout, "fss:local-ptr") == 0 &&
                              strcmp(f.length_at, "bus::ByteEnableLength") == 0);
            } else if (strcmp(f.qname, "core::Extensions") == 0) {
                saw_submeta = f.is_submeta;
            }
        }
        if (!saw_data_ptr)  { printf("  FAIL bus::Data not reported as local-ptr@%d len@bus::Length\n", EXP_DATA); fails++; }
        else                  printf("  ok   bus::Data local-ptr, length_at=bus::Length\n");
        if (!saw_be_ptr)    { printf("  FAIL bus::ByteEnable not reported as local-ptr len@bus::ByteEnableLength\n"); fails++; }
        else                  printf("  ok   bus::ByteEnable local-ptr, length_at=bus::ByteEnableLength\n");
        if (!saw_submeta)   { printf("  FAIL core::Extensions not reported as sub-meta\n"); fails++; }
        else                  printf("  ok   core::Extensions reported as sub-meta\n");
    }

    if (fails == 0) {
        printf("[OK] c meta resolver: all bus:: fields resolved at expected offsets\n");
        return 0;
    }
    printf("[FAIL] c meta resolver: %d failures\n", fails);
    return 1;
}
