/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DStarFec.c
 * @brief D-STAR radio header coding, see DStarFec.h and JARL STD-7 Ap1/Ap2.
 *
 * The chain, in the order the standard defines it:
 *
 *   41 bytes -> 328 bits (LSB first) -> +2 zero flush bits = 330
 *     -> rate 1/2, K=3 convolutional code (g1 = 7 = b^d1^d2, g0 = 5 = b^d2)
 *     -> 660 coded bits written into a 24-column matrix, read column-major
 *     -> XOR with the PN sequence S(x) = x^7 + x^4 + 1, all-ones seed
 *
 * The interleaver is the 24x28 matrix of STD-7 Ap2.2: coded bit i goes to
 * on-air position k(i), with k += 24 per bit and the two wrap corrections that
 * skip the 12 padding positions of the ragged last column (660 = 24 * 27 + 12).
 * Both the encoder and the decoder derive it, so there is no 1.3 kB table.
 */

#include "protocols/DSTAR/DStarFec.h"
#include "core/psram_state.h"

#include <string.h>

const uint8_t dstarEndPattern[DSTAR_END_BYTES] = {0x55, 0x55, 0x55, 0x55, 0xC8, 0x7A};

/* PN bits 0..23 packed LSB first: 0x0E, 0xF2, 0xC9 read MSB first. */
const uint8_t dstarSlowScrambler[3] = {0x70, 0x4F, 0x93};

/* The null voice frame of an idle D-STAR transmitter (MMDVMHost/MBELIB order),
 * bit for bit what an Icom radio sends when the operator is silent. */
const uint8_t dstarNullVoice[DSTAR_VOICE_BYTES] = {0x9E, 0x8D, 0x32, 0x88, 0x26,
                                                   0x1A, 0x3F, 0x61, 0xE8};

/* ------------------------------------------------------------------ CRC --- */

uint16_t dstarCrc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

/* --------------------------------------------------------------- header --- */

static void putField(uint8_t *dst, const char *src, size_t width)
{
    for (size_t i = 0; i < width; i++) {
        char c = src[i];
        if (c == '\0') {
            /* Pad the rest with spaces */
            for (; i < width; i++)
                dst[i] = ' ';
            return;
        }
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        dst[i] = (uint8_t)c;
    }
}

static void getField(const uint8_t *src, char *dst, size_t width)
{
    size_t n = width;
    /* Trim trailing spaces, keep the rest printable */
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\0'))
        n--;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = src[i];
        dst[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    dst[n] = '\0';
}

void dstarHeaderPack(const struct dstarHeader *hdr, uint8_t out[DSTAR_HEADER_BYTES])
{
    memset(out, 0, DSTAR_HEADER_BYTES);
    out[DSTAR_HDR_FLAGS + 0] = hdr->flags[0];
    out[DSTAR_HDR_FLAGS + 1] = hdr->flags[1];
    out[DSTAR_HDR_FLAGS + 2] = hdr->flags[2];
    putField(out + DSTAR_HDR_RPT2, hdr->rpt2, DSTAR_CALL_LEN);
    putField(out + DSTAR_HDR_RPT1, hdr->rpt1, DSTAR_CALL_LEN);
    putField(out + DSTAR_HDR_UR, hdr->ur, DSTAR_CALL_LEN);
    putField(out + DSTAR_HDR_MY, hdr->my, DSTAR_CALL_LEN);
    putField(out + DSTAR_HDR_SFX, hdr->sfx, DSTAR_SUFFIX_LEN);

    uint16_t crc = dstarCrc16(out, DSTAR_HDR_CRC);
    out[DSTAR_HDR_CRC] = (uint8_t)(crc & 0xFF);
    out[DSTAR_HDR_CRC + 1] = (uint8_t)(crc >> 8);
}

void dstarHeaderUnpack(const uint8_t in[DSTAR_HEADER_BYTES], struct dstarHeader *hdr)
{
    hdr->flags[0] = in[DSTAR_HDR_FLAGS + 0];
    hdr->flags[1] = in[DSTAR_HDR_FLAGS + 1];
    hdr->flags[2] = in[DSTAR_HDR_FLAGS + 2];
    getField(in + DSTAR_HDR_RPT2, hdr->rpt2, DSTAR_CALL_LEN);
    getField(in + DSTAR_HDR_RPT1, hdr->rpt1, DSTAR_CALL_LEN);
    getField(in + DSTAR_HDR_UR, hdr->ur, DSTAR_CALL_LEN);
    getField(in + DSTAR_HDR_MY, hdr->my, DSTAR_CALL_LEN);
    getField(in + DSTAR_HDR_SFX, hdr->sfx, DSTAR_SUFFIX_LEN);
    hdr->crcOk = dstarHeaderCheck(in);
    hdr->viterbi = 0;
}

bool dstarHeaderCheck(const uint8_t in[DSTAR_HEADER_BYTES])
{
    uint16_t crc = dstarCrc16(in, DSTAR_HDR_CRC);
    uint16_t got = (uint16_t)(in[DSTAR_HDR_CRC] | ((uint16_t)in[DSTAR_HDR_CRC + 1] << 8));
    return crc == got;
}

/* ---------------------------------------------------------- scrambler --- */

void dstarPnBits(uint8_t *out, size_t len)
{
    uint8_t s = 0x7F;

    for (size_t i = 0; i < len; i++) {
        uint8_t fb = (uint8_t)(((s >> 3) ^ (s >> 6)) & 1);
        out[i] = fb;
        s = (uint8_t)(((s << 1) | fb) & 0x7F);
    }
}

/* -------------------------------------------------------------- coder --- */

/* On-air position of coded bit i (STD-7 Ap2.2 matrix, column-major). */
static inline uint16_t interleaveSlot(uint16_t i)
{
    (void)i;
    return 0; /* unused: the loop below carries k */
}

/**
 * @brief Map a coded bit index to its on-air position, filling onAir[].
 *
 * Kept as a helper so the encoder and the decoder cannot drift apart: both
 * walk the same sequence k += 24 with the two wrap corrections of STD-7.
 */
static void interleaveMap(uint16_t map[DSTAR_CODED_BITS])
{
    uint16_t k = 0;

    for (uint16_t i = 0; i < DSTAR_CODED_BITS; i++) {
        map[i] = k;
        k = (uint16_t)(k + 24);
        if (k >= 672)
            k = (uint16_t)(k - 671);
        else if (k >= 660)
            k = (uint16_t)(k - 647);
    }
}

void dstarHeaderEncode(const uint8_t hdr[DSTAR_HEADER_BYTES],
                       uint8_t bits[DSTAR_CODED_BITS])
{
    uint8_t info[DSTAR_HEADER_INFO_BITS + 2];
    uint8_t coded[DSTAR_CODED_BITS];
    uint8_t pn[DSTAR_CODED_BITS];
    uint16_t map[DSTAR_CODED_BITS];

    /* Bytes to bits, LSB first (STD-7 Ap1.4); the two zero flush bits bring
     * the convolutional coder back to state zero (STD-7 Ap2.1). */
    for (size_t i = 0; i < DSTAR_HEADER_INFO_BITS; i++)
        info[i] = (uint8_t)((hdr[i >> 3] >> (i & 7)) & 1u);
    info[DSTAR_HEADER_INFO_BITS + 0] = 0;
    info[DSTAR_HEADER_INFO_BITS + 1] = 0;

    uint8_t d1 = 0, d2 = 0;
    for (size_t i = 0; i < DSTAR_HEADER_INFO_BITS + 2; i++) {
        uint8_t b = info[i];
        coded[2 * i] = (uint8_t)(b ^ d1 ^ d2); /* g1 = 111 */
        coded[2 * i + 1] = (uint8_t)(b ^ d2);  /* g0 = 101 */
        d2 = d1;
        d1 = b;
    }

    interleaveMap(map);
    dstarPnBits(pn, DSTAR_CODED_BITS);

    /* Interleave then scramble: bit i of the coded block sits at on-air
     * position map[i], and the PN is applied in transmission order. */
    for (uint16_t i = 0; i < DSTAR_CODED_BITS; i++)
        bits[map[i]] = (uint8_t)(coded[i] ^ pn[map[i]]);
}

/*
 * Path memory of the header Viterbi: one predecessor per trellis step and
 * state, 1 320 B.  It lives in PSRAM because it is the largest thing the
 * D-STAR receive chain keeps, and every entry is written before it is read on
 * each call (each of the four states gets at least one candidate per step).
 */
static uint8_t path[330][4] PSRAM_STATE;

bool dstarHeaderDecode(const int8_t soft[DSTAR_CODED_BITS], struct dstarHeader *hdr)
{
    uint16_t map[DSTAR_CODED_BITS];
    uint8_t pn[DSTAR_CODED_BITS];
    int32_t metric[4];
    int32_t next[4];
    uint8_t byte[DSTAR_HEADER_BYTES];
    uint8_t out[330];

    dstarPnBits(pn, DSTAR_CODED_BITS);
    interleaveMap(map);

    /* Descramble and de-interleave into signal order, keeping the soft value. */
    int16_t sig[DSTAR_CODED_BITS];
    for (uint16_t i = 0; i < DSTAR_CODED_BITS; i++) {
        int16_t v = soft[map[i]];
        sig[i] = pn[map[i]] ? (int16_t)-v : v;
    }

    metric[0] = 0;
    metric[1] = metric[2] = metric[3] = -1000000;

    for (int i = 0; i < 330; i++) {
        int32_t a = sig[2 * i];     /* expected g1 */
        int32_t b = sig[2 * i + 1]; /* expected g0 */
        next[0] = next[1] = next[2] = next[3] = -1000000;

        for (int s = 0; s < 4; s++) {
            int d1 = s & 1;
            int d2 = (s >> 1) & 1;
            for (int bit = 0; bit < 2; bit++) {
                int g1 = bit ^ d1 ^ d2;
                int g0 = bit ^ d2;
                int32_t v = metric[s] + (g1 ? a : -a) + (g0 ? b : -b);
                int ns = bit | (d1 << 1);
                if (v > next[ns]) {
                    next[ns] = v;
                    path[i][ns] = (uint8_t)s;
                }
            }
        }
        for (int s = 0; s < 4; s++)
            metric[s] = next[s];
    }

    /* The flush bits put the coder back in state 0, so trace back from there. */
    int s = 0;
    for (int i = 329; i >= 0; i--) {
        out[i] = (uint8_t)(s & 1);
        s = path[i][s];
    }

    memset(byte, 0, sizeof(byte));
    for (int i = 0; i < DSTAR_HEADER_INFO_BITS; i++)
        byte[i >> 3] = (uint8_t)(byte[i >> 3] | (out[i] << (i & 7)));

    /* Count how many channel bits the decoder had to flip, as a quality hint:
     * re-encode the decided bits and compare with the received ones. */
    int errs = 0;
    {
        uint8_t d1 = 0, d2 = 0;
        for (int i = 0; i < 330; i++) {
            uint8_t b = out[i];
            int g1 = b ^ d1 ^ d2;
            int g0 = b ^ d2;
            int16_t ra = sig[2 * i];
            int16_t rb = sig[2 * i + 1];
            if ((ra > 0) != (g1 != 0))
                errs++;
            if ((rb > 0) != (g0 != 0))
                errs++;
            d2 = d1;
            d1 = b;
        }
    }

    dstarHeaderUnpack(byte, hdr);
    hdr->viterbi = (int8_t)(errs > 127 ? 127 : errs);
    return hdr->crcOk;
}
