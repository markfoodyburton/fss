/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/c/cfg_json.h — minimal JSON-scalar reader for fss/config ParamValue.
 *
 * The config::param_request ParamValue (40 bytes) carries a flat JSON object of
 * unsigned-integer values, e.g.  {"frames":100000,"period_us":2}  (open-issue #9:
 * typed ParamValue shapes are deferred; a flat JSON-int blob is the v0.1 form
 * agreed for the CAN platform).  This reader extracts one integer value by key.
 *
 * Deliberately tiny and dependency-free (like meta_hash.h): each binding/host
 * carries its OWN copy.  It is NOT a general JSON parser — it handles a flat
 * object of "key": <decimal-uint> pairs, which is all the config blob ever is.
 */
#ifndef FSS_C_CFG_JSON_H
#define FSS_C_CFG_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Find "key" in the flat JSON object `json` and parse the decimal unsigned
 * integer that follows its colon into *out.  Returns 1 on success, 0 if the key
 * is absent or no integer follows.  Matching is on the quoted key token so that
 * e.g. "period_us" does not match a substring of another key. */
static inline int fss_cfg_json_get_u64(const char *json, const char *key,
                                       uint64_t *out)
{
    if (!json || !key || !out) return 0;
    size_t klen = strlen(key);
    for (const char *p = json; (p = strchr(p, '"')) != NULL; p++) {
        /* p points at an opening quote of some token. */
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 1 + klen + 1;   /* just past the closing quote */
            while (*q == ' ' || *q == '\t') q++;
            if (*q != ':') { p = q; continue; }
            q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q < '0' || *q > '9') return 0;
            uint64_t v = 0;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (uint64_t)(*q - '0'); q++; }
            *out = v;
            return 1;
        }
        /* Not our key: skip to its closing quote so we don't re-scan inside it. */
        const char *close = strchr(p + 1, '"');
        if (!close) break;
        p = close;
    }
    return 0;
}

#endif /* FSS_C_CFG_JSON_H */
