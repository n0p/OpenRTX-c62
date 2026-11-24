/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "dmr_ep.h"

/*
 * The keyring (dmr_ep.h). Written by the UI thread (PC mode), read by the
 * rtx thread when a PI header comes: a sequence count lets the reader take
 * a copy that no write cut in two, as protocols/TETRA/TetraKey.c does.
 */
static struct {
    volatile uint32_t seq; /* odd while being written */
    struct dmrEpKeyStore e[DMR_EP_KEYS];
} ring;

static volatile uint32_t changes; /* by hand, not the restore at power on */

uint32_t dmrEp_changes(void)
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

/*
 * Erase key material. A plain memset() just before a return is a dead
 * store on an automatic object and the compiler is free to delete it, so
 * the key would stay in the stack frame however carefully the code wipes
 * it. Writing through a volatile pointer cannot be optimised away.
 */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- != 0u) {
        *v++ = 0u;
    }
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

bool dmrEp_set(unsigned i, const char *text)
{
    if (i >= DMR_EP_KEYS || text == NULL)
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

    /* The key ID, 0..255, then ':' */
    unsigned id = 0, digits = 0;
    const char *p = text;
    while (*p == ' ')
        p++;
    for (; *p >= '0' && *p <= '9'; p++, digits++)
        id = id * 10u + (unsigned)(*p - '0');
    while (*p == ' ')
        p++;
    if (digits == 0 || digits > 3 || id > 255u || *p != ':')
        return false;
    p++;
    while (*p == ' ')
        p++;

    /* "cargada": the key as it is, under this ID */
    if (strcmp(p, "cargada") == 0) {
        if (!ring.e[i].used)
            return false;
        if (ring.e[i].id != id) {
            writeBegin();
            ring.e[i].id = (uint8_t)id;
            writeEnd();
            changes++;
        }
        return true;
    }

    uint8_t key[DMR_EP_KEY_BYTES];
    unsigned n = 0;
    for (; *p; p++) {
        if (*p == ' ' || *p == ':' || *p == '-')
            continue;
        const int d = hexDigit(*p);
        if (d < 0 || n >= 2u * DMR_EP_KEY_BYTES) {
            wipe(key, sizeof(key)); /* half parsed key off the stack */
            return false;
        }
        if ((n & 1u) == 0)
            key[n / 2] = (uint8_t)(d << 4);
        else
            key[n / 2] |= (uint8_t)d;
        n++;
    }
    if (n != 2u * DMR_EP_KEY_BYTES) {
        wipe(key, sizeof(key)); /* half parsed key off the stack */
        return false;
    }
    writeBegin();
    ring.e[i].used = 1u;
    ring.e[i].id = (uint8_t)id;
    memcpy(ring.e[i].key, key, sizeof(key));
    ring.e[i].pad = 0u;
    writeEnd();
    wipe(key, sizeof(key));
    changes++;
    return true;
}

int dmrEp_get(unsigned i, char *out, size_t n)
{
    if (i >= DMR_EP_KEYS || !ring.e[i].used)
        return snprintf(out, n, "%s", "");
    return snprintf(out, n, "%u:cargada", (unsigned)ring.e[i].id);
}

void dmrEp_save(struct dmrEpKeyStore out[DMR_EP_KEYS])
{
    memcpy(out, ring.e, sizeof(ring.e));
}

bool dmrEp_restore(const struct dmrEpKeyStore in[DMR_EP_KEYS])
{
    for (unsigned i = 0; i < DMR_EP_KEYS; i++)
        if (in[i].used > 1u)
            return false;
    writeBegin();
    memcpy(ring.e, in, sizeof(ring.e));
    for (unsigned i = 0; i < DMR_EP_KEYS; i++)
        if (!ring.e[i].used)
            memset(&ring.e[i], 0, sizeof(ring.e[i]));
    writeEnd();
    return true;
}

bool dmrEp_find(uint8_t id, uint8_t key[DMR_EP_KEY_BYTES])
{
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t s = ring.seq;
        if (s & 1u)
            continue;
        __asm volatile("" ::: "memory");
        bool found = false;
        for (unsigned i = 0; i < DMR_EP_KEYS && !found; i++) {
            if (ring.e[i].used && ring.e[i].id == id) {
                memcpy(key, ring.e[i].key, DMR_EP_KEY_BYTES);
                found = true;
            }
        }
        __asm volatile("" ::: "memory");
        if (ring.seq == s)
            return found;
    }
    return false;
}

/*
 * Motorola Basic Privacy. Each key 1..255 stands for a fixed 16-bit value
 * (table from DSD-FME, include/bp.h, ISC licence: Copyright (C) 2010 DSD
 * Author, permission to use, copy, modify and distribute with this notice).
 */
/*
 * Checked against dsd-fme's include/bp.h on 2026-11-14: identical, all
 * 256 entries. Note before touching it: 240 of them follow a linear law
 * (the value for key i is the XOR of the values for its set bits) and
 * the sixteen from 0xD0 to 0xDF do not. That is not a transcription
 * slip of ours -- the reference breaks it in exactly the same sixteen
 * places. "Correcting" them to the linear values would make keys 208
 * to 223 fail against real Motorola radios, which is the only case the
 * table is for.
 */
static const uint16_t k_bp[256] = {
    0x0000, 0x1F00, 0xE300, 0xFC00, 0x2503, 0x3A03, 0xC603, 0xD903,
    0x4A05, 0x5505, 0xA905, 0xB605, 0x6F06, 0x7006, 0x8C06, 0x9306,
    0x2618, 0x3918, 0xC518, 0xDA18, 0x031B, 0x1C1B, 0xE01B, 0xFF1B,
    0x6C1D, 0x731D, 0x8F1D, 0x901D, 0x491E, 0x561E, 0xAA1E, 0xB51E,
    0x4B28, 0x5428, 0xA828, 0xB728, 0x6E2B, 0x712B, 0x8D2B, 0x922B,
    0x012D, 0x1E2D, 0xE22D, 0xFD2D, 0x242E, 0x3B2E, 0xC72E, 0xD82E,
    0x6D30, 0x7230, 0x8E30, 0x9130, 0x4833, 0x5733, 0xAB33, 0xB433,
    0x2735, 0x3835, 0xC435, 0xDB35, 0x0236, 0x1D36, 0xE136, 0xFE36,
    0x2B49, 0x3449, 0xC849, 0xD749, 0x0E4A, 0x114A, 0xED4A, 0xF24A,
    0x614C, 0x7E4C, 0x824C, 0x9D4C, 0x444F, 0x5B4F, 0xA74F, 0xB84F,
    0x0D51, 0x1251, 0xEE51, 0xF151, 0x2852, 0x3752, 0xCB52, 0xD452,
    0x4754, 0x5854, 0xA454, 0xBB54, 0x6257, 0x7D57, 0x8157, 0x9E57,
    0x6061, 0x7F61, 0x8361, 0x9C61, 0x4562, 0x5A62, 0xA662, 0xB962,
    0x2A64, 0x3564, 0xC964, 0xD664, 0x0F67, 0x1067, 0xEC67, 0xF367,
    0x4679, 0x5979, 0xA579, 0xBA79, 0x637A, 0x7C7A, 0x807A, 0x9F7A,
    0x0C7C, 0x137C, 0xEF7C, 0xF07C, 0x297F, 0x367F, 0xCA7F, 0xD57F,
    0x4D89, 0x5289, 0xAE89, 0xB189, 0x688A, 0x778A, 0x8B8A, 0x948A,
    0x078C, 0x188C, 0xE48C, 0xFB8C, 0x228F, 0x3D8F, 0xC18F, 0xDE8F,
    0x6B91, 0x7491, 0x8891, 0x9791, 0x4E92, 0x5192, 0xAD92, 0xB292,
    0x2194, 0x3E94, 0xC294, 0xDD94, 0x0497, 0x1B97, 0xE797, 0xF897,
    0x06A1, 0x19A1, 0xE5A1, 0xFAA1, 0x23A2, 0x3CA2, 0xC0A2, 0xDFA2,
    0x4CA4, 0x53A4, 0xAFA4, 0xB0A4, 0x69A7, 0x76A7, 0x8AA7, 0x95A7,
    0x20B9, 0x3FB9, 0xC3B9, 0xDCB9, 0x05BA, 0x1ABA, 0xE6BA, 0xF9BA,
    0x6ABC, 0x75BC, 0x89BC, 0x96BC, 0x4FBF, 0x50BF, 0xACBF, 0xB3BF,
    0x66C0, 0x79C0, 0x85C0, 0x9AC0, 0x43C3, 0x5CC3, 0xA0C3, 0xBFC3,
    0x2CC5, 0x33C5, 0xCFC5, 0xD0C5, 0x09C6, 0x16C6, 0xEAC6, 0xF5C6,
    0x84D0, 0x85DF, 0x8AD3, 0x8BDC, 0xB6D5, 0xB7DA, 0xB8D6, 0xB9D9,
    0xD0DA, 0xD1D5, 0xDED9, 0xDFD6, 0xE2DF, 0xE3D0, 0xECDC, 0xEDD3,
    0x2DE8, 0x32E8, 0xCEE8, 0xD1E8, 0x08EB, 0x17EB, 0xEBEB, 0xF4EB,
    0x67ED, 0x78ED, 0x84ED, 0x9BED, 0x42EE, 0x5DEE, 0xA1EE, 0xBEEE,
    0x0BF0, 0x14F0, 0xE8F0, 0xF7F0, 0x2EF3, 0x31F3, 0xCDF3, 0xD2F3,
    0x41F5, 0x5EF5, 0xA2F5, 0xBDF5, 0x64F6, 0x7BF6, 0x87F6, 0x98F6,
};

uint64_t dmrBp_keystream(uint8_t key)
{
    const uint64_t k = k_bp[key];
    return ((k & 0xFF0Fu) << 32) | (k << 16) | k;
}

bool dmrEp_entry(unsigned i, uint8_t *id, uint8_t key[DMR_EP_KEY_BYTES])
{
    if (i >= DMR_EP_KEYS)
        return false;
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t s = ring.seq;
        if (s & 1u)
            continue;
        __asm volatile("" ::: "memory");
        const bool used = ring.e[i].used != 0;
        *id = ring.e[i].id;
        memcpy(key, ring.e[i].key, DMR_EP_KEY_BYTES);
        __asm volatile("" ::: "memory");
        if (ring.seq == s)
            return used;
    }
    return false;
}

/* Anytone Basic Privacy (dmr_ep.h) */
static const uint8_t ambeSilence49[7] = {
    0xF8, 0x01, 0xA9, 0x9F, 0x8C, 0xE0, 0x80
};

static uint16_t bpAnyWord(uint16_t key)
{
    return (uint16_t)((~key & 0xF0F0u) | ((((key >> 8) + 8u) & 0x0Fu) << 8)
                      | ((key + 8u) & 0x0Fu));
}

/* Left in clear: the silence frame, or bits 24..43 all zero */
static bool bpAnyClear(const char bits[49])
{
    bool silence = true, zeroed = true;
    for (int i = 0; i < 49; i++) {
        if ((bits[i] & 1) != ((ambeSilence49[i >> 3] >> (7 - (i & 7))) & 1))
            silence = false;
        if (i >= 24 && i < 44 && (bits[i] & 1))
            zeroed = false;
    }
    return silence || zeroed;
}

void dmrBpAny_frame(char ambe_d[49], uint16_t key, unsigned f)
{
    if (bpAnyClear(ambe_d))
        return;
    const uint16_t k = bpAnyWord(key);
    unsigned pos = (f % 18u) * 49u;
    for (int i = 0; i < 49; i++, pos++)
        ambe_d[i] ^= (char)((k >> (15u - (pos & 15u))) & 1u);
}

void dmrBpAny_packed(uint16_t key, unsigned f, uint8_t frame[7])
{
    char bits[49];
    for (int i = 0; i < 49; i++)
        bits[i] = (char)((frame[i >> 3] >> (7 - (i & 7))) & 1u);
    dmrBpAny_frame(bits, key, f);
    for (int i = 0; i < 7; i++)
        frame[i] = 0;
    for (int i = 0; i < 49; i++)
        if (bits[i])
            frame[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
}

/* tyt16_ambe2_codeword_keystream(): bit i of the frame's 72 (on air)
 * against key bit (start - i) mod 16, MSB first */
static void bpTytFrame(uint16_t key, uint8_t *dibits[36], unsigned start)
{
    for (unsigned i = 0; i < 72; i++) {
        const unsigned j = (start + 16u * 5u - i) % 16u;
        if ((key >> (15u - j)) & 1u)
            *dibits[i / 2] ^= (uint8_t)((i & 1u) ? 1u : 2u);
    }
}

void dmrBpTyt_burst(uint16_t key, uint8_t burst[132])
{
    uint8_t *f[36];
    for (unsigned i = 0; i < 36; i++)
        f[i] = &burst[i];
    bpTytFrame(key, f, 15u);
    for (unsigned i = 0; i < 18; i++) {
        f[i] = &burst[36 + i];
        f[18 + i] = &burst[78 + i];
    }
    bpTytFrame(key, f, 7u);
    for (unsigned i = 0; i < 36; i++)
        f[i] = &burst[96 + i];
    bpTytFrame(key, f, 15u);
}

void dmrScr15_bits(uint16_t key, unsigned slot, uint8_t out[7])
{
    uint16_t reg = (uint16_t)(key & 0x7FFFu);
    for (unsigned i = 0; i < 49u * (slot % 16u); i++) {
        const uint16_t fb = (uint16_t)(((reg >> 1) ^ reg) & 1u);
        reg = (uint16_t)((reg >> 1) | (fb << 14));
    }
    for (unsigned i = 0; i < 7; i++)
        out[i] = 0;
    for (unsigned i = 0; i < 49; i++) {
        if (reg & 1u)
            out[i / 8] |= (uint8_t)(0x80u >> (i % 8));
        const uint16_t fb = (uint16_t)(((reg >> 1) ^ reg) & 1u);
        reg = (uint16_t)((reg >> 1) | (fb << 14));
    }
}

uint32_t dmrEp_nextMi(uint32_t mi)
{
    /* C(x) = x^32 + x^4 + x^2 + 1, 32 steps */
    for (int i = 0; i < 32; i++) {
        const uint32_t bit = ((mi >> 31) ^ (mi >> 3) ^ (mi >> 1)) & 1u;
        mi = (mi << 1) | bit;
    }
    return mi;
}

void dmrEp_keystream(const uint8_t key[DMR_EP_KEY_BYTES], uint32_t mi,
                     unsigned skip, uint8_t *out, unsigned n)
{
    uint8_t k[DMR_EP_KEY_BYTES + 4];
    uint8_t s[256];
    unsigned i, j;

    memcpy(k, key, DMR_EP_KEY_BYTES);
    k[5] = (uint8_t)(mi >> 24);
    k[6] = (uint8_t)(mi >> 16);
    k[7] = (uint8_t)(mi >> 8);
    k[8] = (uint8_t)mi;

    for (i = 0; i < 256; i++)
        s[i] = (uint8_t)i;
    for (i = 0, j = 0; i < 256; i++) {
        j = (j + s[i] + k[i % sizeof(k)]) & 0xffu;
        const uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
    const unsigned first = DMR_EP_DROP + skip;
    i = j = 0;
    for (unsigned c = 0; c < first + n; c++) {
        i = (i + 1) & 0xffu;
        j = (j + s[i]) & 0xffu;
        const uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
        if (c >= first)
            out[c - first] = s[(s[i] + s[j]) & 0xffu];
    }
    wipe(k, sizeof(k));
    wipe(s, sizeof(s));
}

void dmrBp_packed(uint8_t key, uint8_t frame[7])
{
    const uint64_t ks = dmrBp_keystream(key);
    for (int i = 0; i < 6; i++)
        frame[i] ^= (uint8_t)(ks >> (40 - 8 * i));
}

void dmrEp_packed(uint8_t frame[7], const uint8_t ks[DMR_EP_FRAME_BYTES])
{
    for (int i = 0; i < 6; i++)
        frame[i] ^= ks[i];
    frame[6] ^= (uint8_t)(ks[6] & 0x80u);
}

void dmrEp_frame(char ambe_d[49], const uint8_t ks[DMR_EP_FRAME_BYTES],
                 int c0_errors)
{
    /* The AMBE+2 silence frame, 49 bits MSB first of 0xF801A99F8CE080 */
    static const uint64_t silence = 0xF801A99F8CE080ull;
    bool isSilence = true, zeroes = true;
    for (int i = 0; i < 49; i++)
        if ((char)((silence >> (55 - i)) & 1u) != ambe_d[i]) {
            isSilence = false;
            break;
        }
    for (int i = 24; i < 44; i++)
        if (ambe_d[i] != 0) {
            zeroes = false;
            break;
        }
    if (isSilence || zeroes || c0_errors >= 3)
        return;
    for (int i = 0; i < 49; i++)
        ambe_d[i] ^= (char)((ks[i / 8] >> (7 - i % 8)) & 1u);
}
