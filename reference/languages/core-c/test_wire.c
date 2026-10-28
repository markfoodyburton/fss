/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * test_wire.c — round-trip test for the generic carrier serializer (fss/c/wire.h).
 *
 * Builds a TLM carrier (FSS_TLM_META) whose data region holds a bus::Data
 * fss:local-ptr pointing at a separate payload buffer, serializes it to a flat
 * wire buffer, deserializes into fresh region+scratch buffers, and asserts:
 *   - the payload bytes survived the round-trip,
 *   - the rebuilt data_ptr is a VALID pointer into scratch (not the original),
 *   - bus::Length is unchanged.
 * This is the mechanism that lets a TLM b_transport cross a process/host
 * boundary (the inlined payload), independent of any transport.
 */
#include "fss/c/wire.h"
#include "fss/c/meta.h"
#include "fss/known_carriers/tlm_id.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Offsets from known_carriers/tlm/transaction.jsonnet (the complete/live
 * carrier: real tlm_generic_payload offsets). */
enum { OFF_DATA = 24, OFF_LENGTH = 32 };

int main(void) {
    int fails = 0;

    /* ---- build an in-process TLM carrier with a payload pointer ---------- */
    struct {
        uint8_t before[8];
        uint8_t payload[8];
        uint8_t after[8];
    } guarded_payload = {
        { 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5 },
        { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 },
        { 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A }
    };
    uint8_t *payload = guarded_payload.payload;
    uint8_t region[80];
    memset(region, 0, sizeof(region));

    uint32_t length = sizeof(guarded_payload.payload);
    memcpy(region + OFF_LENGTH, &length, sizeof(length));     /* bus::Length */
    uint8_t *pp = payload;
    memcpy(region + OFF_DATA, &pp, sizeof(pp));                /* bus::Data (local-ptr) */

    fss_carrier_t c;
    memset(&c, 0, sizeof(c));
    c.id        = FSS_TLM_ID;
    c.data_ptr  = region;
    c.data_size = sizeof(region);
    uint8_t wire[256];

    /* Carrier region lengths are encoded as wire u32 values. */
    c.data_size = (uint64_t)UINT32_MAX + 1;
    if (fss_carrier_serialize(FSS_TLM_META, FSS_TLM_META_LEN, &c, wire, sizeof(wire)) >= 0) {
        printf("  FAIL serialize accepted data_size above UINT32_MAX\n"); fails++;
    } else {
        printf("  ok   serialize rejects data_size above UINT32_MAX\n");
    }
    c.data_size = sizeof(region);

    if (!fss_carrier_has_pointers(FSS_TLM_META, FSS_TLM_META_LEN)) {
        printf("  FAIL TLM meta reports no pointer fields\n"); fails++;
    } else {
        printf("  ok   TLM meta has pointer fields\n");
    }

    /* ---- serialize ------------------------------------------------------- */
    long wn = fss_carrier_serialize(FSS_TLM_META, FSS_TLM_META_LEN, &c, wire, sizeof(wire));
    if (wn < 0) {
        printf("  FAIL serialize returned %ld\n", wn); fails++;
    } else {
        /* expect region(64) + u32 len-prefix(4) + region_size u32(4) + payload(8) */
        printf("  ok   serialize wrote %ld bytes\n", wn);
    }

    /* A reply must contain all region bytes before it can overlay the caller. */
    uint8_t short_reply[4] = { (uint8_t)sizeof(region), 0, 0, 0 };
    uint8_t before_region[sizeof(region)];
    memcpy(before_region, region, sizeof(region));
    if (fss_carrier_apply_reply(FSS_TLM_META, FSS_TLM_META_LEN, &c,
                                short_reply, sizeof(short_reply)) >= 0 ||
        memcmp(region, before_region, sizeof(region)) != 0) {
        printf("  FAIL truncated reply was accepted or altered the region\n"); fails++;
    } else {
        printf("  ok   truncated reply rejected before region overlay\n");
    }

    /* A peer can increase bus::Length in its region.  That value describes
     * the reply, not the capacity of this caller's payload buffer. */
    uint8_t oversized_reply[256];
    uint8_t before_payload[sizeof(guarded_payload)];
    memcpy(before_payload, &guarded_payload, sizeof(guarded_payload));
    memcpy(before_region, region, sizeof(region));
    memcpy(oversized_reply, wire, 4 + sizeof(region));
    uint32_t peer_length = 16;
    uint32_t peer_inline_length = 16;
    memcpy(oversized_reply + 4 + OFF_LENGTH, &peer_length, sizeof(peer_length));
    memcpy(oversized_reply + 4 + sizeof(region), &peer_inline_length,
           sizeof(peer_inline_length));
    memset(oversized_reply + 4 + sizeof(region) + 4, 0xCC, peer_inline_length);
    memcpy(oversized_reply + 4 + sizeof(region) + 4 + peer_inline_length,
           wire + 4 + sizeof(region) + 4 + sizeof(guarded_payload.payload),
           (size_t)wn - (4 + sizeof(region) + 4 + sizeof(guarded_payload.payload)));
    size_t oversized_len = (size_t)wn + 8;
    if (fss_carrier_apply_reply(FSS_TLM_META, FSS_TLM_META_LEN, &c,
                                oversized_reply, oversized_len) >= 0 ||
        memcmp(region, before_region, sizeof(region)) != 0 ||
        memcmp(&guarded_payload, before_payload, sizeof(guarded_payload)) != 0) {
        printf("  FAIL oversized reply was accepted or changed caller buffers\n"); fails++;
    } else {
        printf("  ok   oversized reply rejected before caller-buffer write\n");
    }

    /* ---- deserialize into fresh buffers ---------------------------------- */
    uint8_t out_region[80];
    uint8_t scratch[80];
    memset(out_region, 0, sizeof(out_region));
    memset(scratch, 0, sizeof(scratch));
    long rn = fss_carrier_deserialize(FSS_TLM_META, FSS_TLM_META_LEN,
                                      wire, (size_t)wn,
                                      out_region, sizeof(out_region),
                                      scratch, sizeof(scratch));
    if (rn < 0) {
        printf("  FAIL deserialize returned %ld\n", rn); fails++;
    } else {
        printf("  ok   deserialize rebuilt %ld-byte region\n", rn);
    }

    /* ---- checks ---------------------------------------------------------- */
    uint32_t out_len = 0;
    memcpy(&out_len, out_region + OFF_LENGTH, sizeof(out_len));
    if (out_len != length) { printf("  FAIL bus::Length %u (want %u)\n", out_len, length); fails++; }
    else                     printf("  ok   bus::Length survived = %u\n", out_len);

    uint8_t *out_pp = NULL;
    memcpy(&out_pp, out_region + OFF_DATA, sizeof(out_pp));
    if (!out_pp) {
        printf("  FAIL rebuilt bus::Data pointer is NULL\n"); fails++;
    } else if (out_pp == payload) {
        printf("  FAIL rebuilt pointer still points at the ORIGINAL payload (no copy)\n"); fails++;
    } else if (memcmp(out_pp, payload, sizeof(guarded_payload.payload)) != 0) {
        printf("  FAIL payload bytes differ after round-trip\n"); fails++;
    } else {
        printf("  ok   payload bytes survived, repointed into scratch\n");
    }

    if (fails == 0) {
        printf("[OK] c wire serializer: TLM pointer payload round-trips\n");
        return 0;
    }
    printf("[FAIL] c wire serializer: %d failures\n", fails);
    return 1;
}
