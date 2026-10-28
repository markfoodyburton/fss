/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * meta.c — implementation of the carrier-agnostic C meta resolver (fss/c/meta.h).
 *
 * A minimal reader for the closed CBOR profile tools/fss_compile.py emits.
 * The meta is a definite-length array of definite-length maps; each map has
 * tstr keys ("qname", "wire", "byte_offset", "byte_size", "layout", ...) and
 * values that are tstr / uint / bool / (for submeta) a nested array.  We only
 * need to locate the map whose "qname" matches and read its "byte_offset" /
 * "byte_size" uints — but we must skip every value type to walk maps reliably.
 */
#include "fss/c/meta.h"
#include "fss/c/meta_hash.h"   /* fss_fnv1a64 + fss__cbor_head (for flattening) */

#include <stdlib.h>
#include <string.h>

/* ---- bounded CBOR cursor ------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int            ok;     /* cleared on any malformed/out-of-bounds read */
} cur_t;

static uint8_t cur_byte(cur_t *c) {
    if (c->p >= c->end) { c->ok = 0; return 0; }
    return *c->p++;
}

/* Read a CBOR head: major type + argument (definite forms only). */
static void read_head(cur_t *c, int *major, uint64_t *arg) {
    uint8_t ib = cur_byte(c);
    if (!c->ok) return;
    *major = (ib >> 5) & 0x07;
    uint8_t ai = ib & 0x1f;
    if (ai < 24) {
        *arg = ai;
    } else if (ai == 24) {
        *arg = cur_byte(c);
    } else if (ai == 25) {
        uint64_t hi = cur_byte(c), lo = cur_byte(c);
        *arg = (hi << 8) | lo;
    } else if (ai == 26) {
        uint64_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | cur_byte(c);
        *arg = v;
    } else if (ai == 27) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | cur_byte(c);
        *arg = v;
    } else {
        c->ok = 0;   /* indefinite / reserved — not in the profile */
    }
}

/* Skip one complete data item (any profile type). */
static void skip_item(cur_t *c) {
    int major; uint64_t arg;
    read_head(c, &major, &arg);
    if (!c->ok) return;
    switch (major) {
        case 0: case 1:            /* uint / negint — value in arg */
        case 7:                    /* simple/bool/float-by-ai — no payload here */
            return;
        case 2: case 3:            /* bstr / tstr — arg bytes follow */
            if ((uint64_t)(c->end - c->p) < arg) { c->ok = 0; return; }
            c->p += arg;
            return;
        case 4:                    /* array of arg items */
            for (uint64_t i = 0; i < arg && c->ok; ++i) skip_item(c);
            return;
        case 5:                    /* map of arg pairs */
            for (uint64_t i = 0; i < arg && c->ok; ++i) { skip_item(c); skip_item(c); }
            return;
        default:
            c->ok = 0;
            return;
    }
}

/* If the cursor is at a tstr, compare it to s (no allocation) and advance
 * past it; sets *match.  If not a tstr, fail. */
static void match_tstr(cur_t *c, const char *s, int *match) {
    int major; uint64_t arg;
    const uint8_t *save = c->p;
    read_head(c, &major, &arg);
    if (!c->ok || major != 3) { c->ok = 0; (void)save; return; }
    if ((uint64_t)(c->end - c->p) < arg) { c->ok = 0; return; }
    size_t n = strlen(s);
    *match = (arg == (uint64_t)n) && (memcmp(c->p, s, n) == 0);
    c->p += arg;
}

/* Read a uint item into out (must be major 0). */
static void read_uint(cur_t *c, uint64_t *out) {
    int major; uint64_t arg;
    read_head(c, &major, &arg);
    if (!c->ok || major != 0) { c->ok = 0; return; }
    *out = arg;
}

/* Copy a tstr value into dst[cap] (NUL-terminated, truncated if longer).  The
 * cursor must be at a tstr; advances past it. */
static void read_tstr(cur_t *c, char *dst, size_t cap) {
    int major; uint64_t arg;
    read_head(c, &major, &arg);
    if (!c->ok || major != 3) { c->ok = 0; return; }
    if ((uint64_t)(c->end - c->p) < arg) { c->ok = 0; return; }
    size_t n = (arg < cap - 1) ? (size_t)arg : cap - 1;
    memcpy(dst, c->p, n);
    dst[n] = '\0';
    c->p += arg;
}

/* ---- fss_meta_find ------------------------------------------------------ */

int fss_meta_find(const void *meta, uint64_t meta_size, const char *qname,
                  uint32_t *byte_offset, uint32_t *byte_size)
{
    if (!meta || !qname) return 0;
    cur_t c = { (const uint8_t *)meta, (const uint8_t *)meta + meta_size, 1 };

    int major; uint64_t n_entries;
    read_head(&c, &major, &n_entries);
    if (!c.ok || major != 4) return 0;          /* top level must be an array */

    for (uint64_t e = 0; e < n_entries && c.ok; ++e) {
        int emajor; uint64_t n_pairs;
        read_head(&c, &emajor, &n_pairs);
        if (!c.ok || emajor != 5) return 0;     /* each entry is a map */

        int      found_qname = 0;
        int      have_off = 0, have_size = 0;
        uint64_t off = 0, size = 0;

        for (uint64_t k = 0; k < n_pairs && c.ok; ++k) {
            /* key is a tstr; dispatch on which one */
            int major2; uint64_t klen;
            const uint8_t *kp = c.p;
            read_head(&c, &major2, &klen);
            if (!c.ok || major2 != 3) return 0;
            if ((uint64_t)(c.end - c.p) < klen) return 0;
            const char *key = (const char *)c.p;
            c.p += klen;

            if (klen == 5 && memcmp(key, "qname", 5) == 0) {
                int m = 0;
                match_tstr(&c, qname, &m);
                found_qname = m;
            } else if (klen == 11 && memcmp(key, "byte_offset", 11) == 0) {
                read_uint(&c, &off);  have_off = c.ok;
            } else if (klen == 9 && memcmp(key, "byte_size", 9) == 0) {
                read_uint(&c, &size); have_size = c.ok;
            } else {
                skip_item(&c);        /* wire / version / layout / attrs / submeta */
            }
            (void)kp;
        }

        if (c.ok && found_qname) {
            /* sub-meta entries have no byte_offset/byte_size — not a data field */
            if (!have_off || !have_size) return 0;
            if (off > UINT32_MAX || size > UINT32_MAX) return 0;
            if (byte_offset) *byte_offset = (uint32_t)off;
            if (byte_size)   *byte_size   = (uint32_t)size;
            return 1;
        }
    }
    return 0;
}

/* ---- (id, qname) cache -------------------------------------------------- */

#define FSS_META_CACHE_SLOTS 64

typedef struct {
    int      used;
    uint64_t id;
    char     *qname;
    size_t   qname_len;
    uint32_t off;
    uint32_t size;
} cache_slot;

struct fss_meta_cache {
    cache_slot slots[FSS_META_CACHE_SLOTS];
    unsigned   next;   /* round-robin eviction cursor */
};

fss_meta_cache_t *fss_meta_cache_create(void) {
    return (fss_meta_cache_t *)calloc(1, sizeof(struct fss_meta_cache));
}

void fss_meta_cache_destroy(fss_meta_cache_t *cache) {
    if (!cache) return;
    for (unsigned i = 0; i < FSS_META_CACHE_SLOTS; ++i)
        free(cache->slots[i].qname);
    free(cache);
}

int fss_meta_resolve(fss_meta_cache_t *cache, uint64_t id,
                     const void *meta, uint64_t meta_size, const char *qname,
                     uint32_t *byte_offset, uint32_t *byte_size)
{
    if (!qname) return 0;
    size_t qn = strlen(qname);

    if (cache) {
        for (unsigned i = 0; i < FSS_META_CACHE_SLOTS; ++i) {
            cache_slot *s = &cache->slots[i];
            if (s->used && s->id == id && s->qname_len == qn &&
                memcmp(s->qname, qname, qn) == 0) {
                if (byte_offset) *byte_offset = s->off;
                if (byte_size)   *byte_size   = s->size;
                return 1;
            }
        }
    }

    uint32_t off = 0, size = 0;
    if (!fss_meta_find(meta, meta_size, qname, &off, &size)) return 0;

    if (cache && qn < SIZE_MAX) {
        cache_slot *s = &cache->slots[cache->next];
        char *name = (char *)malloc(qn + 1);
        if (!name) goto done;
        memcpy(name, qname, qn + 1);
        cache->next = (cache->next + 1) % FSS_META_CACHE_SLOTS;
        free(s->qname);
        s->used = 1; s->id = id; s->qname = name; s->qname_len = qn;
        s->off = off; s->size = size;
    }

done:
    if (byte_offset) *byte_offset = off;
    if (byte_size)   *byte_size   = size;
    return 1;
}

/* ---- field iteration ---------------------------------------------------- */

/* Parse one meta entry (a CBOR map) at the cursor into `out`, advancing past
 * it.  Reads qname / byte_offset / byte_size / layout and attrs.length_at;
 * skips everything else.  Sets c->ok = 0 on malformed input. */
static void parse_entry(cur_t *c, fss_meta_field_t *out) {
    int emajor; uint64_t n_pairs;
    read_head(c, &emajor, &n_pairs);
    if (!c->ok || emajor != 5) { c->ok = 0; return; }

    memset(out, 0, sizeof(*out));
    int have_off = 0, have_size = 0;

    for (uint64_t k = 0; k < n_pairs && c->ok; ++k) {
        int kmajor; uint64_t klen;
        read_head(c, &kmajor, &klen);
        if (!c->ok || kmajor != 3) { c->ok = 0; return; }
        if ((uint64_t)(c->end - c->p) < klen) { c->ok = 0; return; }
        const char *key = (const char *)c->p;
        c->p += klen;

        if (klen == 5 && memcmp(key, "qname", 5) == 0) {
            read_tstr(c, out->qname, sizeof(out->qname));
        } else if (klen == 11 && memcmp(key, "byte_offset", 11) == 0) {
            uint64_t v = 0; read_uint(c, &v);
            if (v > UINT32_MAX) c->ok = 0;
            else out->byte_offset = (uint32_t)v;
            have_off = c->ok;
        } else if (klen == 9 && memcmp(key, "byte_size", 9) == 0) {
            uint64_t v = 0; read_uint(c, &v);
            if (v > UINT32_MAX) c->ok = 0;
            else out->byte_size = (uint32_t)v;
            have_size = c->ok;
        } else if (klen == 6 && memcmp(key, "layout", 6) == 0) {
            read_tstr(c, out->layout, sizeof(out->layout));
        } else if (klen == 5 && memcmp(key, "attrs", 5) == 0) {
            /* attrs is a map; pull out length_at, skip the rest. */
            int amajor; uint64_t a_pairs;
            read_head(c, &amajor, &a_pairs);
            if (!c->ok || amajor != 5) { c->ok = 0; return; }
            for (uint64_t a = 0; a < a_pairs && c->ok; ++a) {
                int akm; uint64_t aklen;
                read_head(c, &akm, &aklen);
                if (!c->ok || akm != 3) { c->ok = 0; return; }
                if ((uint64_t)(c->end - c->p) < aklen) { c->ok = 0; return; }
                const char *akey = (const char *)c->p;
                c->p += aklen;
                if (aklen == 9 && memcmp(akey, "length_at", 9) == 0)
                    read_tstr(c, out->length_at, sizeof(out->length_at));
                else
                    skip_item(c);
            }
        } else {
            skip_item(c);   /* wire / version / encoding / submeta */
        }
    }

    /* A sub-meta slot (layout "sub-meta") or any entry lacking offset/size is
     * not a data field. */
    if (strcmp(out->layout, "sub-meta") == 0 || !have_off || !have_size)
        out->is_submeta = 1;
}

int fss_meta_field_count(const void *meta, uint64_t meta_size) {
    if (!meta) return 0;
    cur_t c = { (const uint8_t *)meta, (const uint8_t *)meta + meta_size, 1 };
    int major; uint64_t n;
    read_head(&c, &major, &n);
    if (!c.ok || major != 4) return 0;
    return (int)n;
}

int fss_meta_field_at(const void *meta, uint64_t meta_size, int idx,
                      fss_meta_field_t *out) {
    if (!meta || !out || idx < 0) return 0;
    cur_t c = { (const uint8_t *)meta, (const uint8_t *)meta + meta_size, 1 };
    int major; uint64_t n;
    read_head(&c, &major, &n);
    if (!c.ok || major != 4 || (uint64_t)idx >= n) return 0;

    fss_meta_field_t scratch;
    for (int e = 0; e <= idx && c.ok; ++e)
        parse_entry(&c, (e == idx) ? out : &scratch);
    return c.ok ? 1 : 0;
}

/* ---- flattening -------------------------------------------------------- */

/* The compiler emits deterministic CBOR.  Flattening copies selected raw
 * entries, so accept only that form: definite, minimally encoded items and
 * strictly ordered map keys. */
static int head_is_canonical(uint8_t ai, uint64_t arg) {
    return (ai < 24 && arg < 24) ||
           (ai == 24 && arg >= 24) ||
           (ai == 25 && arg > UINT8_MAX) ||
           (ai == 26 && arg > UINT16_MAX) ||
           (ai == 27 && arg > UINT32_MAX);
}

static int valid_utf8(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t b = p[i++];
        unsigned more;
        if (b < 0x80) continue;
        if (b >= 0xc2 && b <= 0xdf) more = 1;
        else if (b >= 0xe0 && b <= 0xef) more = 2;
        else if (b >= 0xf0 && b <= 0xf4) more = 3;
        else return 0;
        if (more > n - i) return 0;
        /* Reject overlong forms, UTF-16 surrogates, and code points > U+10FFFF. */
        if ((b == 0xe0 && p[i] < 0xa0) || (b == 0xed && p[i] >= 0xa0) ||
            (b == 0xf0 && p[i] < 0x90) || (b == 0xf4 && p[i] >= 0x90)) return 0;
        for (unsigned j = 0; j < more; ++j) if ((p[i + j] & 0xc0) != 0x80) return 0;
        i += more;
    }
    return 1;
}

static void validate_item(cur_t *c) {
    int major; uint64_t arg;
    const uint8_t *head = c->p;
    uint8_t ai;
    if (c->p >= c->end) { c->ok = 0; return; }
    ai = *c->p & 0x1f;
    read_head(c, &major, &arg);
    if (!c->ok || !head_is_canonical(ai, arg)) { c->ok = 0; return; }
    if (major == 0 || major == 1) return;
    if (major == 2 || major == 3) {
        if ((uint64_t)(c->end - c->p) < arg) c->ok = 0;
        else {
            if (major == 3 && !valid_utf8(c->p, (size_t)arg)) c->ok = 0;
            c->p += arg;
        }
        return;
    }
    if (major == 4) {
        for (uint64_t i = 0; i < arg && c->ok; ++i) validate_item(c);
        return;
    }
    if (major == 5) {
        const uint8_t *previous = NULL;
        size_t previous_len = 0;
        for (uint64_t i = 0; i < arg && c->ok; ++i) {
            const uint8_t *key = c->p;
            validate_item(c);
            size_t key_len = (size_t)(c->p - key);
            if (!c->ok || (previous &&
                (memcmp(key, previous, key_len < previous_len ? key_len : previous_len) < 0 ||
                 (memcmp(key, previous, key_len < previous_len ? key_len : previous_len) == 0 &&
                  key_len <= previous_len)))) {
                c->ok = 0; return;
            }
            previous = key; previous_len = key_len;
            validate_item(c);
        }
        return;
    }
    /* The compiler's profile uses only false, true, and null simple values. */
    if (major != 7 || !(ai == 20 || ai == 21 || ai == 22)) c->ok = 0;
}

static size_t cbor_head_len(uint64_t value) {
    return value < 24 ? 1 : value < 0x100ULL ? 2 : value < 0x10000ULL ? 3 :
           value < 0x100000000ULL ? 5 : 9;
}

static int is_key(const uint8_t *key, uint64_t len, const char *name) {
    return len == strlen(name) && memcmp(key, name, (size_t)len) == 0;
}

/* Advance past an entry and report the properties used by the two projections. */
static void inspect_entry(cur_t *c, int *non_ser, int *not_in_hash,
                          int *is_submeta) {
    int emajor; uint64_t n_pairs;
    read_head(c, &emajor, &n_pairs);
    if (!c->ok || emajor != 5) { c->ok = 0; return; }

    *non_ser = *not_in_hash = *is_submeta = 0;
    for (uint64_t k = 0; k < n_pairs && c->ok; ++k) {
        int kmajor; uint64_t klen;
        read_head(c, &kmajor, &klen);
        if (!c->ok || kmajor != 3) { c->ok = 0; return; }
        if ((uint64_t)(c->end - c->p) < klen) { c->ok = 0; return; }
        const char *key = (const char *)c->p;
        c->p += klen;
        if (is_key((const uint8_t *)key, klen, "serializable") ||
            is_key((const uint8_t *)key, klen, "in_hash")) {
            /* value is a bool: 0xf4 false / 0xf5 true */
            if (c->p >= c->end) { c->ok = 0; return; }
            if (*c->p == 0xf4) {
                if (is_key((const uint8_t *)key, klen, "serializable")) *non_ser = 1;
                else *not_in_hash = 1;
            }
            else if (*c->p != 0xf5) { c->ok = 0; return; }
            c->p += 1;
        } else if (is_key((const uint8_t *)key, klen, "layout")) {
            int match = 0;
            match_tstr(c, "sub-meta", &match);
            if (!c->ok) return;
            *is_submeta = match;
        } else {
            skip_item(c);   /* skip the value (any profile type) */
        }
    }
}

/* Copy an entry for the id meta.  A retained sub-meta keeps its marker but
 * omits precisely its `submeta` value. */
static int emit_id_entry(const uint8_t *entry, const uint8_t *end,
                         int is_submeta, uint8_t *out, size_t cap, size_t *off) {
    cur_t c = { entry, end, 1 };
    int major; uint64_t pairs;
    read_head(&c, &major, &pairs);
    if (!c.ok || major != 5) return 0;
    uint64_t kept = pairs;
    if (is_submeta) {
        for (uint64_t i = 0; i < pairs && c.ok; ++i) {
            int km; uint64_t kl;
            read_head(&c, &km, &kl);
            if (!c.ok || km != 3 || (uint64_t)(c.end - c.p) < kl) return 0;
            int strip = is_key(c.p, kl, "submeta");
            c.p += kl;
            skip_item(&c);
            if (!c.ok) return 0;
            if (strip) --kept;
        }
    }
    size_t hlen = cbor_head_len(kept);
    if (*off > cap || hlen > cap - *off) return 0;
    *off += fss__cbor_head(out + *off, 5, kept);
    c.p = entry;
    read_head(&c, &major, &pairs);
    for (uint64_t i = 0; i < pairs && c.ok; ++i) {
        const uint8_t *start = c.p;
        int km; uint64_t kl;
        read_head(&c, &km, &kl);
        if (!c.ok || km != 3 || (uint64_t)(c.end - c.p) < kl) return 0;
        int strip = is_submeta && is_key(c.p, kl, "submeta");
        c.p += kl;
        skip_item(&c);
        if (!c.ok) return 0;
        size_t len = (size_t)(c.p - start);
        if (!strip) {
            if (*off > cap || len > cap - *off) return 0;
            memcpy(out + *off, start, len); *off += len;
        }
    }
    return c.ok;
}

long fss_meta_flatten(const void *meta, uint64_t meta_size,
                      uint8_t *out, size_t out_cap, uint64_t *flat_id) {
    if (!meta || !out) return -1;
    if (meta_size > SIZE_MAX) return -1;
    cur_t valid = { (const uint8_t *)meta, (const uint8_t *)meta + meta_size, 1 };
    validate_item(&valid);
    if (!valid.ok || valid.p != valid.end) return -1;
    cur_t c = { (const uint8_t *)meta, (const uint8_t *)meta + meta_size, 1 };
    int major; uint64_t n;
    read_head(&c, &major, &n);
    if (!c.ok || major != 4) return -1;

    /* Collect the byte span of every entry that is NOT serializable:false. */
    if (n > SIZE_MAX / sizeof(const uint8_t *)) return -1;
    const uint8_t **kept_start = (const uint8_t **)malloc((size_t)n * sizeof(*kept_start));
    size_t *kept_len = (size_t *)malloc((size_t)n * sizeof(*kept_len));
    int *kept_submeta = (int *)malloc((size_t)n * sizeof(*kept_submeta));
    int *kept_in_hash = (int *)malloc((size_t)n * sizeof(*kept_in_hash));
    if ((n && (!kept_start || !kept_len || !kept_submeta || !kept_in_hash))) {
        free(kept_start); free(kept_len); free(kept_submeta); free(kept_in_hash); return -1;
    }
    size_t kept = 0;
    for (uint64_t e = 0; e < n && c.ok; ++e) {
        const uint8_t *estart = c.p;
        int drop, not_in_hash, is_submeta;
        inspect_entry(&c, &drop, &not_in_hash, &is_submeta);
        if (!c.ok) { free(kept_start); free(kept_len); free(kept_submeta); free(kept_in_hash); return -1; }
        if (!drop) {
            kept_start[kept] = estart;
            kept_len[kept]   = (size_t)(c.p - estart);
            kept_submeta[kept] = is_submeta;
            kept_in_hash[kept] = not_in_hash;
            kept++;
        }
    }

    /* Emit: new array header for `kept`, then the kept entries verbatim. */
    size_t hlen = cbor_head_len((uint64_t)kept);
    if (hlen > out_cap) goto fail;
    size_t off = fss__cbor_head(out, 4, (uint64_t)kept);
    for (size_t i = 0; i < kept; ++i) {
        if (off > out_cap || kept_len[i] > out_cap - off) goto fail;
        memcpy(out + off, kept_start[i], kept_len[i]);
        off += kept_len[i];
    }
    uint8_t *id_meta = (uint8_t *)malloc((size_t)meta_size);
    if (!id_meta && meta_size) goto fail;
    size_t id_count = 0, id_off = 0;
    for (size_t i = 0; i < kept; ++i) if (!kept_in_hash[i]) id_count++;
    if (cbor_head_len((uint64_t)id_count) > (size_t)meta_size) { free(id_meta); goto fail; }
    id_off = fss__cbor_head(id_meta, 4, (uint64_t)id_count);
    for (size_t i = 0; i < kept; ++i)
        if (!kept_in_hash[i] && !emit_id_entry(kept_start[i], kept_start[i] + kept_len[i],
                                                kept_submeta[i], id_meta, (size_t)meta_size, &id_off)) {
            free(id_meta); goto fail;
        }
    if (flat_id) *flat_id = fss_fnv1a64(id_meta, id_off);
    free(id_meta);
    free(kept_start); free(kept_len); free(kept_submeta); free(kept_in_hash);
    return (long)off;
fail:
    free(kept_start); free(kept_len); free(kept_submeta); free(kept_in_hash);
    return -1;
}
