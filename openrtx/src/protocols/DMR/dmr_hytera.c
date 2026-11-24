/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The schemes follow dsd-neo (GPL-3.0-or-later): crypt-etc.c
 * (hytera_bp_apply_frame49), crypt-rc4.c (rc4_block_output,
 * hytera_enhanced_rc4_setup) and dmr_pi.c (hytera_lfsr,
 * dmr_hytera_checksum). See dmr_hytera.h.
 */

#include <stdio.h>
#include <string.h>
#include "dmr_hytera.h"

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- != 0u)
        *v++ = 0u;
}

/* ---- Basic: the key, written by the UI thread, read by the vocoder ---- */

static struct {
    volatile uint32_t seq; /* odd while being written */
    struct dmrHyBpStore k;
} hbp;
static volatile uint32_t hbpChanges;

uint32_t dmrHyBp_changes(void)
{
    return hbpChanges;
}

static void hbpWrite(const struct dmrHyBpStore *v)
{
    hbp.seq++;
    __asm volatile("" ::: "memory");
    hbp.k = *v;
    __asm volatile("" ::: "memory");
    hbp.seq++;
}

static bool hbpRead(struct dmrHyBpStore *out)
{
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t s = hbp.seq;
        if (s & 1u)
            continue;
        __asm volatile("" ::: "memory");
        *out = hbp.k;
        __asm volatile("" ::: "memory");
        if (hbp.seq == s)
            return out->len != 0u;
    }
    return false;
}

void dmrHyBp_save(struct dmrHyBpStore *out)
{
    *out = hbp.k;
}

bool dmrHyBp_restore(const struct dmrHyBpStore *in)
{
    if (in->len != 0u && in->len != 5u && in->len != 16u && in->len != 32u)
        return false;
    hbpWrite(in);
    return true;
}

bool dmrHyBp_present(void)
{
    return hbp.k.len != 0u;
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

bool dmrHyBp_set(const char *text)
{
    if (text == NULL)
        return false;
    struct dmrHyBpStore v;
    memset(&v, 0, sizeof(v));
    if (text[0] == '\0') {
        if (hbp.k.len != 0u) {
            hbpWrite(&v);
            hbpChanges++;
        }
        return true;
    }
    if (strcmp(text, "cargada") == 0)
        return hbp.k.len != 0u;
    unsigned n = 0;
    for (const char *p = text; *p; p++) {
        if (*p == ' ' || *p == ':' || *p == '-')
            continue;
        const int d = hexDigit(*p);
        if (d < 0 || n >= 64u) {
            wipe(&v, sizeof(v));
            return false;
        }
        if ((n & 1u) == 0)
            v.key[n / 2] = (uint8_t)(d << 4);
        else
            v.key[n / 2] |= (uint8_t)d;
        n++;
    }
    if (n != 10u && n != 32u && n != 64u) {
        wipe(&v, sizeof(v));
        return false;
    }
    v.len = (uint8_t)(n / 2u);
    hbpWrite(&v);
    wipe(&v, sizeof(v));
    hbpChanges++;
    return true;
}

int dmrHyBp_get(char *out, size_t n)
{
    return snprintf(out, n, "%s", hbp.k.len != 0u ? "cargada" : "");
}

/* hytera_bp_apply_frame49(): the key's bits round and round, 882 of them */
void dmrHyBp_frame(char ambe_d[49], unsigned f)
{
    static const uint64_t silence = 0xF801A99F8CE080ull;
    bool isSilence = true;
    for (int i = 0; i < 49; i++)
        if ((char)((silence >> (55 - i)) & 1u) != (ambe_d[i] & 1)) {
            isSilence = false;
            break;
        }
    struct dmrHyBpStore k;
    if (isSilence || !hbpRead(&k)) {
        wipe(&k, sizeof(k));
        return;
    }
    const unsigned bits = 8u * k.len; /* 40, 128 or 256 */
    unsigned pos = ((f % 18u) * 49u) % bits;
    for (int i = 0; i < 49; i++) {
        ambe_d[i] ^= (char)((k.key[pos / 8u] >> (7u - pos % 8u)) & 1u);
        if (++pos == bits)
            pos = 0;
    }
    wipe(&k, sizeof(k));
}

/* ---- Enhanced ------------------------------------------------------------ */

/* rc4_block_output() with no drop, then hytera_enhanced_rc4_setup()'s
 * key ^ MI over it: the superframe's 135 bytes */
static void hyEpStream(const uint8_t key[5], uint64_t mi, uint8_t ks[135])
{
    uint8_t S[256];
    for (int i = 0; i < 256; i++)
        S[i] = (uint8_t)i;
    unsigned j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % 5]) & 0xFFu;
        const uint8_t t = S[i];
        S[i] = S[j];
        S[j] = t;
    }
    unsigned a = 0, b = 0;
    uint8_t m[5];
    for (int i = 0; i < 5; i++)
        m[i] = (uint8_t)(mi >> (32 - 8 * i));
    for (int i = 0; i < 135; i++) {
        a = (a + 1u) & 0xFFu;
        b = (b + S[a]) & 0xFFu;
        const uint8_t t = S[a];
        S[a] = S[b];
        S[b] = t;
        ks[i] = (uint8_t)(S[(S[a] + S[b]) & 0xFFu] ^ key[i % 5] ^ m[i % 5]);
    }
    wipe(S, sizeof(S));
    wipe(m, sizeof(m));
}

void dmrHyEp_bits(const uint8_t key[5], uint64_t mi, unsigned f,
                  uint8_t out[7])
{
    uint8_t ks[135];
    hyEpStream(key, mi, ks);
    memset(out, 0, 7);
    const unsigned base = (f % 18u) * 49u;
    for (unsigned i = 0; i < 49; i++) {
        const unsigned p = base + i;
        if ((ks[p / 8u] >> (7u - p % 8u)) & 1u)
            out[i / 8u] |= (uint8_t)(0x80u >> (i % 8u));
    }
    wipe(ks, sizeof(ks));
}

/* hytera_lfsr() with hytera_enhanced_alg_refresh()'s taps */
uint64_t dmrHyEp_nextMi(uint64_t mi)
{
    static const uint8_t taps[5] = { 0x12, 0x24, 0x48, 0x22, 0x14 };
    uint8_t m[5];
    for (int i = 0; i < 5; i++)
        m[i] = (uint8_t)(mi >> (32 - 8 * i));
    uint64_t out = 0;
    for (int i = 0; i < 5; i++) {
        const uint8_t bit = (uint8_t)((m[i] >> 7) & 1u);
        m[i] = (uint8_t)(m[i] << 1);
        if (bit)
            m[i] ^= taps[i];
        m[i] |= bit;
        out = (out << 8) | m[i];
    }
    return out;
}

bool dmrHyEp_piOk(const uint8_t pi[10])
{
    uint8_t sum = 0;
    for (int i = 0; i < 9; i++)
        sum = (uint8_t)(sum + pi[i]);
    return (uint8_t)(0u - sum) == pi[9];
}
