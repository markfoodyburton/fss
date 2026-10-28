/*
 * This file is part of fss
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Test the private wire decoder directly.  Including the implementation keeps
 * this focused test independent of a Zenoh session or network peer. */
#include "zenoh.c"

int main(void)
{
    uint8_t frame[64];
    uint8_t region[MAX_DATA], scratch[MAX_DATA], meta[MAX_META];
    fss_carrier_t carrier;
    size_t off = 0;
    uint8_t payload = 0x5a;

    if (!fput(frame, &off, sizeof(frame), data_magic, sizeof(data_magic)) ||
        !fput64(frame, &off, sizeof(frame), UINT64_C(1)) ||
        !fput32(frame, &off, sizeof(frame), 0) ||
        !fput32(frame, &off, sizeof(frame), 1) ||
        !fput(frame, &off, sizeof(frame), &payload, sizeof(payload)))
        return 1;

    memset(&carrier, 0, sizeof(carrier));
    if (!decode_request(frame, off, &carrier, region, sizeof(region), scratch,
                        sizeof(scratch), meta, sizeof(meta)))
        return 1;

    /* The extra byte must not be accepted as part of this frame. */
    frame[off] = 0;
    memset(&carrier, 0, sizeof(carrier));
    return decode_request(frame, off + 1, &carrier, region, sizeof(region),
                          scratch, sizeof(scratch), meta, sizeof(meta)) ? 1 : 0;
}
