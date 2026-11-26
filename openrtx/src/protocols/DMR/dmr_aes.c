/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The keystreams follow RadioFoundry's voice_aes.c (MIT), whose DMR IV
 * expansion and ordering are adapted from dmrgo, Copyright 2023-2026 Jacob
 * McSwain (MIT, aes/LICENSE.dmrgo); DES-OFB as dsd-neo uses it (dsd_mbe.c,
 * GPL-3.0-or-later). AES: tiny-AES-c (Unlicense, aes/UNLICENSE.tinyaes),
 * AES-256 in aes/tinyaes.c, AES-128 in aes/tinyaes128.c. DES: dsd-neo's
 * (ISC, aes/des.c).
 */

#include <stdio.h>
#include <string.h>
#include "dmr_aes.h"
#include "aes/tinyaes.h"
#include "aes/des.h"

/* aes/tinyaes128.c */
void tinyaes128_ofb(const uint8_t key[16], const uint8_t iv[16],
                    unsigned discard, uint8_t *out, unsigned n);

/*
 * The keyring. Written by the UI thread (PC mode), read by the rtx thread
 * when a header comes: a sequence count lets the reader take a copy that no
 * write cut in two (as dmr_ep.c).
 */
static struct {
    volatile uint32_t seq; /* odd while being written */
    struct dmrAesKeyStore e[DMR_AES_KEYS];
} ring;

static volatile uint32_t changes;

uint32_t dmrAes_changes(void)
{
    return changes;
}

static void writeBegin(void)
{
    ring.seq++;
    __asm volatile("" ::: "memory");
}

static void writeEnd(void)
{
    __asm volatile("" ::: "memory");
    ring.seq++;
}

void dmrAes_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- != 0u)
        *v++ = 0u;
}

/* An entry's key length: 0 in the store means AES-256 (before DES and
 * AES-128 came, the byte was padding) */
static uint8_t lenOf(const struct dmrAesKeyStore *e)
{
    return (e->len == DMR_KEY_DES || e->len == DMR_KEY_AES128)
           ? e->len : DMR_KEY_AES256;
}

static int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool dmrAes_set(unsigned i, const char *text)
{
    if (i >= DMR_AES_KEYS || text == NULL)
        return false;
    if (text[0] == '\0') {
        if (ring.e[i].used) {
            writeBegin();
            memset(&ring.e[i], 0, sizeof(ring.e[i]));
            writeEnd();
            changes++;
        }
        return true;
    }

    /* The key ID, 1..65535, then ':' */
    unsigned id = 0, digits = 0;
    const char *p = text;
    while (*p == ' ')
        p++;
    for (; *p >= '0' && *p <= '9'; p++, digits++)
        id = id * 10u + (unsigned)(*p - '0');
    while (*p == ' ')
        p++;
    if (digits == 0 || digits > 5 || id == 0u || id > 65535u || *p != ':')
        return false;
    p++;
    while (*p == ' ')
        p++;

    if (strcmp(p, "cargada") == 0) {
        if (!ring.e[i].used)
            return false;
        if (ring.e[i].id != id) {
            writeBegin();
            ring.e[i].id = (uint16_t)id;
            writeEnd();
            changes++;
        }
        return true;
    }

    /* 16 hex digits DES, 32 AES-128, 64 AES-256 */
    uint8_t key[DMR_AES_KEY_BYTES];
    memset(key, 0, sizeof(key));
    unsigned n = 0;
    for (; *p; p++) {
        if (*p == ' ' || *p == ':' || *p == '-')
            continue;
        const int d = hexDigit(*p);
        if (d < 0 || n >= 2u * DMR_AES_KEY_BYTES) {
            dmrAes_wipe(key, sizeof(key));
            return false;
        }
        if ((n & 1u) == 0)
            key[n / 2] = (uint8_t)(d << 4);
        else
            key[n / 2] |= (uint8_t)d;
        n++;
    }
    if (n != 2u * DMR_KEY_DES && n != 2u * DMR_KEY_AES128
        && n != 2u * DMR_KEY_AES256) {
        dmrAes_wipe(key, sizeof(key));
        return false;
    }
    writeBegin();
    ring.e[i].used = 1u;
    ring.e[i].len = (uint8_t)(n / 2u);
    ring.e[i].id = (uint16_t)id;
    memcpy(ring.e[i].key, key, sizeof(key));
    writeEnd();
    dmrAes_wipe(key, sizeof(key));
    changes++;
    return true;
}

int dmrAes_get(unsigned i, char *out, size_t n)
{
    if (i >= DMR_AES_KEYS || !ring.e[i].used)
        return snprintf(out, n, "%s", "");
    return snprintf(out, n, "%u:cargada", (unsigned)ring.e[i].id);
}

void dmrAes_save(struct dmrAesKeyStore out[DMR_AES_KEYS])
{
    memcpy(out, ring.e, sizeof(ring.e));
}

bool dmrAes_restore(const struct dmrAesKeyStore in[DMR_AES_KEYS])
{
    for (unsigned i = 0; i < DMR_AES_KEYS; i++)
        if (in[i].used > 1u)
            return false;
    writeBegin();
    memcpy(ring.e, in, sizeof(ring.e));
    for (unsigned i = 0; i < DMR_AES_KEYS; i++)
        if (!ring.e[i].used)
            memset(&ring.e[i], 0, sizeof(ring.e[i]));
    writeEnd();
    return true;
}

/* A copy of entry i, or the one with that ID (i < 0), no write cut in two */
static bool take(int i, uint16_t id, uint16_t *idOut,
                 uint8_t key[DMR_AES_KEY_BYTES], uint8_t *lenOut)
{
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t s = ring.seq;
        if (s & 1u)
            continue;
        __asm volatile("" ::: "memory");
        bool found = false;
        for (unsigned k = 0; k < DMR_AES_KEYS && !found; k++) {
            const bool match = (i >= 0) ? (k == (unsigned)i)
                                        : (ring.e[k].id == id);
            if (ring.e[k].used && match) {
                if (key != NULL)
                    memcpy(key, ring.e[k].key, DMR_AES_KEY_BYTES);
                if (idOut != NULL)
                    *idOut = ring.e[k].id;
                if (lenOut != NULL)
                    *lenOut = lenOf(&ring.e[k]);
                found = true;
            }
        }
        __asm volatile("" ::: "memory");
        if (ring.seq == s)
            return found;
    }
    return false;
}

bool dmrAes_find(uint16_t id, uint8_t key[DMR_AES_KEY_BYTES], uint8_t *len)
{
    return id != 0u && take(-1, id, NULL, key, len);
}

bool dmrAes_entry(unsigned i, uint16_t *id, uint8_t key[DMR_AES_KEY_BYTES],
                  uint8_t *len)
{
    return i < DMR_AES_KEYS && take((int)i, 0, id, key, len);
}

uint8_t dmrAes_lenOf(unsigned i)
{
    uint8_t len = 0;
    if (i >= DMR_AES_KEYS || !take((int)i, 0, NULL, NULL, &len))
        return 0;
    return len;
}

uint8_t dmrAes_dmrAlg(uint8_t len)
{
    return len == DMR_KEY_DES ? 0x22u
         : (len == DMR_KEY_AES128 ? 0x24u : 0x25u);
}

uint8_t dmrAes_dmrLen(uint8_t alg)
{
    switch (alg & 0xDFu) {
        case 0x02u: return DMR_KEY_DES;
        case 0x04u: return DMR_KEY_AES128;
        case 0x05u: return DMR_KEY_AES256;
        default:    return 0;
    }
}

uint8_t dmrAes_p25Alg(uint8_t len)
{
    return len == DMR_KEY_DES ? 0x81u
         : (len == DMR_KEY_AES128 ? 0x89u : 0x84u);
}

uint8_t dmrAes_p25Len(uint8_t alg)
{
    switch (alg) {
        case 0x81u: return DMR_KEY_DES;
        case 0x89u: return DMR_KEY_AES128;
        case 0x84u: return DMR_KEY_AES256;
        default:    return 0;
    }
}

/* ---- OFB: DES, AES-128 or AES-256 by the key's length ------------------- */

/* n bytes of keystream from the IV (8 bytes for DES, 16 for AES), its
 * first block dropped, as all these voice formats do */
static void ofbStream(const uint8_t *key, uint8_t len, const uint8_t iv[16],
                      uint8_t *out, unsigned n)
{
    if (len == DMR_KEY_DES) {
        c62Des_ofb(key, iv, 1u, out, n);
    } else if (len == DMR_KEY_AES128) {
        tinyaes128_ofb(key, iv, 1u, out, n);
    } else {
        struct AES_ctx ctx;
        uint8_t blk[16];
        memcpy(blk, iv, 16);
        tinyaes_init_ctx(&ctx, key);
        tinyaes_ECB_encrypt(&ctx, blk); /* the first block is dropped */
        for (unsigned at = 0; at < n; at += 16) {
            tinyaes_ECB_encrypt(&ctx, blk);
            const unsigned count = n - at < 16 ? n - at : 16;
            memcpy(out + at, blk, count);
        }
        dmrAes_wipe(blk, sizeof(blk));
        dmrAes_wipe(&ctx, sizeof(ctx));
    }
}

/* ---- P25 keystream (voice_aes.c) ---------------------------------------- */

static uint64_t p25Step(uint64_t mi)
{
    for (unsigned i = 0; i < 64; i++)
        mi = (mi << 1) | (((mi >> 63) ^ (mi >> 61) ^ (mi >> 45) ^ (mi >> 37)
                           ^ (mi >> 26) ^ (mi >> 14)) & 1u);
    return mi;
}

uint64_t dmrAes_p25NextMi(uint64_t mi)
{
    return p25Step(mi);
}

void dmrAes_p25Keystream(const uint8_t key[DMR_AES_KEY_BYTES], uint8_t len,
                         uint64_t mi, uint8_t out[DMR_AES_P25_BYTES])
{
    /* AES: the MI and the MI 64 steps on; DES: the MI alone */
    uint8_t iv[16];
    const uint64_t next = p25Step(mi);
    for (unsigned i = 0; i < 8; i++) {
        iv[i] = (uint8_t)(mi >> (56 - i * 8));
        iv[8 + i] = (uint8_t)(next >> (56 - i * 8));
    }
    ofbStream(key, len, iv, out, DMR_AES_P25_BYTES);
    dmrAes_wipe(iv, sizeof(iv));
}

void dmrAes_p25Frame(const uint8_t ks[DMR_AES_P25_BYTES], unsigned f,
                     uint8_t out[DMR_AES_P25_FRAME_BYTES])
{
    if (f > 17u) {
        memset(out, 0, DMR_AES_P25_FRAME_BYTES);
        return;
    }
    const unsigned offset = 11u + 11u * f + (f >= 8u ? 2u : 0u)
                          + (f >= 17u ? 2u : 0u);
    memcpy(out, ks + offset, DMR_AES_P25_FRAME_BYTES);
}

/* ---- DMR keystream (voice_aes.c) ---------------------------------------- */

uint32_t dmrAes_nextMi(uint32_t mi)
{
    for (unsigned i = 0; i < 32; i++)
        mi = (mi << 1) | (((mi >> 31) ^ (mi >> 21) ^ (mi >> 1) ^ mi) & 1u);
    return mi;
}

/* The MI's 4 bytes then the LFSR run on: 16 bytes for AES; DES takes the
 * first 8 (dsd-neo's dmr_des_expand_iv()) */
static void dmrIv(uint32_t mi, uint8_t iv[16])
{
    memset(iv, 0, 16);
    for (unsigned i = 0; i < 4; i++)
        iv[i] = (uint8_t)(mi >> (24 - i * 8));
    for (unsigned i = 32; i < 128; i++) {
        const uint8_t bit =
            (uint8_t)(((mi >> 31) ^ (mi >> 21) ^ (mi >> 1) ^ mi) & 1u);
        mi = (mi << 1) | bit;
        iv[i / 8] = (uint8_t)((iv[i / 8] << 1) | bit);
    }
}

void dmrAes_keystream(const uint8_t key[DMR_AES_KEY_BYTES], uint8_t len,
                      uint32_t mi, unsigned skip, uint8_t *out, unsigned n)
{
    uint8_t iv[16];
    uint8_t ks[DMR_AES_SF_BYTES];
    dmrIv(mi, iv);
    ofbStream(key, len, iv, ks, sizeof(ks));
    for (unsigned i = 0; i < n; i++)
        out[i] = (skip + i < sizeof(ks)) ? ks[skip + i] : 0u;
    dmrAes_wipe(ks, sizeof(ks));
    dmrAes_wipe(iv, sizeof(iv));
}
