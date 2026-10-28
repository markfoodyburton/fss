/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * test_live_gp_serialize.cpp — prove a LIVE tlm_generic_payload serialises over
 * the wire.
 *
 * This is the crux of "an SC model can initiate over the wire": the initiator
 * hands the transport a live GP (data_ptr = &gp) tagged with the LIVE meta
 * (FSS_TLM_META — bus:: fields at their REAL offsets inside the object).
 * A transport can't call TLM accessors, but with that meta it can WALK the live
 * object: read each field at its real offset and inline bus::Data's payload.
 *
 * Here we drive the exact transport path on a real GP:
 *   build gp → fss_carrier_serialize(live meta) → fss_carrier_deserialize →
 *   read the rebuilt region by the live meta's offsets → assert the GP's fields
 *   and its payload survived, with bus::Data repointed into the receiver's
 *   buffer (not the sender's).  No address-space sharing, no accessors.
 */
#include <tlm>

#include "fss/carrier.h"
#include "fss/c/wire.h"
#include "fss/c/meta.h"
#include "fss/known_carriers/tlm_id.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int fails = 0;
static uint32_t field_u32(const uint8_t *region, const char *qname) {
    uint32_t off = 0, sz = 0;
    fss_meta_find(FSS_TLM_META, FSS_TLM_META_LEN, qname, &off, &sz);
    uint32_t v = 0; std::memcpy(&v, region + off, sizeof(v)); return v;
}
static uint64_t field_u64(const uint8_t *region, const char *qname) {
    uint32_t off = 0, sz = 0;
    fss_meta_find(FSS_TLM_META, FSS_TLM_META_LEN, qname, &off, &sz);
    uint64_t v = 0; std::memcpy(&v, region + off, sizeof(v)); return v;
}

int sc_main(int, char **) {
    /* ---- build a real, live tlm_generic_payload ---- */
    unsigned char payload[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    tlm::tlm_generic_payload gp;
    gp.set_command(tlm::TLM_WRITE_COMMAND);   /* 1 */
    gp.set_address(0x1000);
    gp.set_data_ptr(payload);
    gp.set_data_length(sizeof(payload));
    gp.set_streaming_width(sizeof(payload));
    gp.set_byte_enable_ptr(nullptr);
    gp.set_byte_enable_length(0);
    gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    fss_carrier_t c;
    std::memset(&c, 0, sizeof(c));
    c.data_ptr  = &gp;                                  /* the LIVE object */
    c.data_size = sizeof(tlm::tlm_generic_payload);
    c.meta_ptr  = const_cast<uint8_t *>(FSS_TLM_META);
    c.meta_size = FSS_TLM_META_LEN;

    if (!fss_carrier_has_pointers(FSS_TLM_META, FSS_TLM_META_LEN)) {
        std::printf("  FAIL live meta reports no pointer fields\n"); fails++;
    }

    /* ---- serialise the live GP (what a wire transport does on send) ---- */
    uint8_t wire[1024];
    long wn = fss_carrier_serialize(FSS_TLM_META, FSS_TLM_META_LEN,
                                    &c, wire, sizeof(wire));
    if (wn < 0) { std::printf("  FAIL serialize -> %ld\n", wn); fails++; }
    else        std::printf("  ok   serialised live GP -> %ld wire bytes\n", wn);

    /* ---- deserialise into fresh receiver buffers (other address space) ---- */
    uint8_t region[256], scratch[256];
    std::memset(region, 0, sizeof(region));
    std::memset(scratch, 0xCC, sizeof(scratch));
    long rn = fss_carrier_deserialize(FSS_TLM_META, FSS_TLM_META_LEN,
                                      wire, (size_t)wn,
                                      region, sizeof(region), scratch, sizeof(scratch));
    if (rn < 0) { std::printf("  FAIL deserialize -> %ld\n", rn); fails++; }

    /* ---- read the rebuilt region by the live meta's offsets ---- */
    uint32_t cmd  = field_u32(region, "bus::Operation");
    uint64_t addr = field_u64(region, "bus::Locator");
    uint32_t len  = field_u32(region, "bus::Length");
    if (cmd  != 1)       { std::printf("  FAIL command %u (want 1)\n", cmd);  fails++; }
    if (addr != 0x1000)  { std::printf("  FAIL address 0x%llx (want 0x1000)\n", (unsigned long long)addr); fails++; }
    if (len  != 8)       { std::printf("  FAIL length %u (want 8)\n", len);   fails++; }
    if (!fails) std::printf("  ok   command=%u address=0x%llx length=%u survived\n",
                            cmd, (unsigned long long)addr, len);

    /* bus::Data must be repointed into the receiver's scratch (NOT the original
     * payload), and the bytes must match. */
    uint32_t doff = 0, dsz = 0;
    fss_meta_find(FSS_TLM_META, FSS_TLM_META_LEN, "bus::Data", &doff, &dsz);
    uint8_t *rebuilt = nullptr;
    std::memcpy(&rebuilt, region + doff, sizeof(rebuilt));
    if (!rebuilt)                          { std::printf("  FAIL bus::Data NULL\n"); fails++; }
    else if (rebuilt == payload)           { std::printf("  FAIL bus::Data still points at the sender's payload\n"); fails++; }
    else if (std::memcmp(rebuilt, payload, sizeof(payload)) != 0)
                                           { std::printf("  FAIL payload bytes differ after round-trip\n"); fails++; }
    else        std::printf("  ok   payload inlined + repointed into receiver buffer\n");

    if (fails == 0) {
        std::printf("[OK] live GP serialises over the wire (payload inlined)\n");
        return 0;
    }
    std::printf("[FAIL] live GP serialise: %d failure(s)\n", fails);
    return 1;
}
