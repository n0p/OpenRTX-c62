/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrFec.c
 * @brief DMR channel coding, see DmrFec.h.
 */

#include <string.h>

#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrFec.h"

/*
 * Syndrome of the Hamming(15,11,3) word to the bit it implicates: the 15
 * correctable patterns plus "no error" (0xFF) and "not a single error" (0xFE).
 */
static const uint8_t hamming15113Bit[16] = {
    [0] = 0xFF, [1] = 11, [2] = 12, [3] = 8,  [4] = 13, [5] = 5,
    [6] = 9,    [7] = 3,  [8] = 14, [9] = 0,  [10] = 6, [11] = 1,
    [12] = 10,  [13] = 7, [14] = 4, [15] = 2,
};

/** Correct one bit of a Hamming(15,11,3) word: DMR "second" variant. */
static bool hamming_15113_fix(uint8_t d[15])
{
    unsigned c0 = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    unsigned c1 = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    unsigned c2 = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    unsigned c3 = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
    unsigned syndrome = (c0 != d[11]) ? 0x01u : 0u;

    syndrome |= (c1 != d[12]) ? 0x02u : 0u;
    syndrome |= (c2 != d[13]) ? 0x04u : 0u;
    syndrome |= (c3 != d[14]) ? 0x08u : 0u;

    uint8_t bit = hamming15113Bit[syndrome];
    if (bit > 14)
        return false;

    d[bit] ^= 1;
    return true;
}

/*
 * Syndrome of the Hamming(13,9,3) word to the bit it implicates, same encoding
 * as the (15,11) table above.
 */
static const uint8_t hamming1393Bit[16] = {
    [0] = 0xFF, [1] = 9,  [2] = 10, [3] = 6,  [4] = 11, [5] = 3,
    [6] = 7,    [7] = 1,  [8] = 12, [9] = 0,  [10] = 4, [11] = 0xFE,
    [12] = 8,   [13] = 5, [14] = 2, [15] = 0,
};

/** Correct one bit of a Hamming(13,9,3) word: BPTC columns. */
static bool hamming_1393_fix(uint8_t d[13])
{
    unsigned c0 = d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6];
    unsigned c1 = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7];
    unsigned c2 = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    unsigned c3 = d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[8];
    unsigned syndrome = (c0 != d[9]) ? 0x01u : 0u;

    syndrome |= (c1 != d[10]) ? 0x02u : 0u;
    syndrome |= (c2 != d[11]) ? 0x04u : 0u;
    syndrome |= (c3 != d[12]) ? 0x08u : 0u;

    uint8_t bit = hamming1393Bit[syndrome];
    if (bit > 12)
        return false;

    d[bit] ^= 1;
    return true;
}

/*
 * CACH syndrome to the codeword position it implicates, or 0xFF when the error
 * is not a single one.  Columns of the parity-check matrix for {AT, TC, LS1,
 * LS0, H0, H1, H2}.
 */
static const uint8_t cachSyndromePos[8] = { [0] = 0xFF, [1] = 4, [2] = 5,
                                            [3] = 2,    [4] = 6, [5] = 0,
                                            [6] = 3,    [7] = 1 };

int dmr_fec_cach_decode(uint8_t cach[24])
{
    uint8_t at = cach[dmr_cach_tact_pos[0]];
    uint8_t tc = cach[dmr_cach_tact_pos[1]];
    uint8_t ls1 = cach[dmr_cach_tact_pos[2]];
    uint8_t ls0 = cach[dmr_cach_tact_pos[3]];
    uint8_t h0 = cach[dmr_cach_tact_pos[4]];
    uint8_t h1 = cach[dmr_cach_tact_pos[5]];
    uint8_t h2 = cach[dmr_cach_tact_pos[6]];
    int syndrome = (at ^ tc ^ ls1 ^ h0) | ((tc ^ ls1 ^ ls0 ^ h1) << 1)
                 | ((at ^ tc ^ ls0 ^ h2) << 2);

    if (syndrome == 0)
        return 0;

    if (cachSyndromePos[syndrome] == 0xFF)
        return -1;

    cach[dmr_cach_tact_pos[cachSyndromePos[syndrome]]] ^= 1;
    return 1;
}

void dmr_fec_cach_encode(uint8_t cach[24])
{
    uint8_t at = cach[dmr_cach_tact_pos[0]];
    uint8_t tc = cach[dmr_cach_tact_pos[1]];
    uint8_t ls1 = cach[dmr_cach_tact_pos[2]];
    uint8_t ls0 = cach[dmr_cach_tact_pos[3]];

    cach[dmr_cach_tact_pos[4]] = at ^ tc ^ ls1;
    cach[dmr_cach_tact_pos[5]] = tc ^ ls1 ^ ls0;
    cach[dmr_cach_tact_pos[6]] = at ^ tc ^ ls0;
}

uint32_t dmr_fec_golay_encode(uint8_t info)
{
    uint16_t check = 0;
    uint16_t parity;

    for (size_t i = 0; i < 8; i++) {
        if ((info & (1u << i)) != 0)
            check ^= dmr_golay_basis[i];
    }

    /* The 12 significant bits of the basis vectors, permuted as the
     * extended Golay code of the slot type requires. */
    parity = (uint16_t)((((check >> 6) & 0x3) << 10)
                        | (((check >> 2) & 0xF) << 6) | ((check & 0x3) << 4)
                        | ((check >> 12) & 0xF));

    return ((uint32_t)info << 12) | (uint32_t)(parity & 0x0FFF);
}

int dmr_fec_golay_decode(uint32_t codeword, uint8_t *info)
{
    int best = 21;
    uint8_t bestInfo = 0;

    /*
     * Nearest codeword over the 256 possible information words: the code has
     * only 8 information bits, so an exhaustive search is cheaper than a
     * syndrome table (and needs 1 KiB less of .rodata than one would).
     */
    for (unsigned v = 0; v < 256; v++) {
        uint32_t distance = codeword ^ dmr_fec_golay_encode((uint8_t)v);
        int weight = __builtin_popcount(distance & 0xFFFFFu);

        if (weight < best) {
            best = weight;
            bestInfo = (uint8_t)v;
            if (weight == 0)
                break;
        }
    }

    if (info != NULL)
        *info = bestInfo;
    return best;
}

bool dmr_fec_bptc_decode(const uint8_t in[196], uint8_t data[96])
{
    uint8_t g[196];
    bool fixing;
    int passes = 0;

    /* De-interleave: g[a] = raw[(a * 181) mod 196]. */
    for (int a = 0; a < 196; a++)
        g[a] = in[(a * 181) % 196] & 1u;

    do {
        fixing = false;

        for (int c = 0; c <= 14; c++) {
            uint8_t col[13];
            int p = c + 1;

            for (int a = 0; a < 13; a++, p += 15)
                col[a] = g[p];
            if (hamming_1393_fix(col)) {
                p = c + 1;
                for (int a = 0; a < 13; a++, p += 15)
                    g[p] = col[a];
                fixing = true;
            }
        }

        for (int r = 0; r <= 8; r++) {
            if (hamming_15113_fix(&g[r * 15 + 1]))
                fixing = true;
        }

        passes++;
    } while (fixing && (passes < 5));

    /*
     * Information bits: row 0 columns 4..11, then rows 1..8 columns 1..11.
     * Bit 0 of the block is R(3) and is never payload.
     */
    int pos = 0;
    for (int a = 4; a <= 11; a++)
        data[pos++] = g[a];
    for (int r = 1; r <= 8; r++) {
        for (int a = 1; a <= 11; a++)
            data[pos++] = g[r * 15 + a];
    }

    return !fixing;
}

uint16_t dmr_fec_crc16(const uint8_t *bits, int nbits)
{
    const uint32_t poly = (1u << 12) + (1u << 5) + 1u;
    uint32_t crc = 0;

    for (int i = 0; i < nbits; i++) {
        crc = ((crc << 1) | (bits[i] & 1u)) & 0x1FFFFu;
        if ((crc & 0x10000u) != 0)
            crc = (crc & 0xFFFFu) ^ poly;
    }

    return (uint16_t)((crc ^ 0xFFFFu) & 0xFFFFu);
}

uint16_t dmr_fec_crc16_append(const uint8_t *bits, int nbits)
{
    const uint32_t poly = (1u << 12) + (1u << 5) + 1u;
    uint32_t crc = 0;

    /*
     * The 16 bits to append are the ones that make the block leave a zero
     * residue in dmr_fec_crc16().  Feeding the message and then sixteen zeros
     * leaves the remainder of P(x)*x^16; appending b(x) adds b, so the block
     * closes when b is the complement of that remainder (subtraction and
     * addition are the same XOR in GF(2)).
     */
    for (int i = 0; i < nbits + 16; i++) {
        crc = (crc << 1) & 0x1FFFFu;
        if (i < nbits)
            crc |= (uint32_t)(bits[i] & 1u);
        if ((crc & 0x10000u) != 0)
            crc = (crc & 0xFFFFu) ^ poly;
    }

    return (uint16_t)(crc ^ 0xFFFFu);
}

void dmr_fec_voice_deinterleave(const uint8_t payload[72], uint8_t frame[9])
{
    int8_t planes[4][24];

    /*
     * Every valid (row, column) of the mbelib planes is written by exactly one
     * of the 72 dibit bits, so an all-zero start is enough.
     */
    memset(planes, 0, sizeof(planes));

    for (int i = 0; i < 36; i++) {
        /* MSB of the dibit goes through rW/rX, LSB through rY/rZ. */
        planes[dmr_rw[i]][dmr_rx[i]] = (int8_t)payload[2 * i];
        planes[dmr_ry[i]][dmr_rz[i]] = (int8_t)payload[2 * i + 1];
    }

    memset(frame, 0, DMR_AMBE_BYTES);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            int t = dmr_ambe_layout[r][c];
            if ((t >= 0) && (planes[r][c] != 0))
                frame[t >> 3] |= (uint8_t)(0x80u >> (t & 7));
        }
    }
}

void dmr_fec_voice_interleave(const uint8_t frame[9], uint8_t payload[72])
{
    int8_t planes[4][24];

    memset(planes, 0, sizeof(planes));

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            int t = dmr_ambe_layout[r][c];
            if (t >= 0)
                planes[r][c] = (int8_t)((frame[t >> 3] >> (7 - (t & 7))) & 1u);
        }
    }

    for (int i = 0; i < 36; i++) {
        payload[2 * i] = (uint8_t)planes[dmr_rw[i]][dmr_rx[i]];
        payload[2 * i + 1] = (uint8_t)planes[dmr_ry[i]][dmr_rz[i]];
    }
}

/*
 * ---------------------------------------------------------------------------
 * Transmit side: the inverses of the codes above.
 * ---------------------------------------------------------------------------
 */

/** Parity of a Hamming(15,11,3) word: same equations as the fixer. */
static void hamming_15113_encode(uint8_t d[15])
{
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    d[13] = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    d[14] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
}

/** Parity of a Hamming(13,9,3) word: BPTC columns. */
static void hamming_1393_encode(uint8_t d[13])
{
    d[9] = d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6];
    d[10] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7];
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[8];
}

void dmr_fec_bptc_encode(const uint8_t data[96], uint8_t out[196])
{
    uint8_t g[196];

    memset(g, 0, sizeof(g));

    /* Information bits: row 0 columns 4..11, then rows 1..8 columns 1..11. */
    int pos = 0;
    for (int a = 4; a <= 11; a++)
        g[a] = data[pos++] & 1u;
    for (int r = 1; r <= 8; r++) {
        for (int a = 1; a <= 11; a++)
            g[r * 15 + a] = data[pos++] & 1u;
    }

    for (int r = 0; r <= 8; r++)
        hamming_15113_encode(&g[r * 15 + 1]);

    for (int c = 0; c <= 14; c++) {
        uint8_t col[13];
        int p = c + 1;

        for (int a = 0; a < 13; a++, p += 15)
            col[a] = g[p];
        hamming_1393_encode(col);
        p = c + 1;
        for (int a = 0; a < 13; a++, p += 15)
            g[p] = col[a];
    }

    g[0] = 0; /* R(3), always zero */

    /* Interleave: out[(a * 181) mod 196] = g[a]. */
    for (int a = 0; a < 196; a++)
        out[(a * 181) % 196] = g[a];
}

/*
 * GF(2^8) of TS 102 361-1 annex B: x^8 + x^4 + x^3 + x^2 + 1 (0x11D).  A
 * shift-and-add multiply is enough: one link control is 27 of them, once per
 * data burst.
 */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;

    for (int i = 0; i < 8; i++) {
        if ((b & 1u) != 0)
            p ^= a;
        b >>= 1;
        if ((a & 0x80u) != 0)
            a = (uint8_t)((a << 1) ^ 0x1Du);
        else
            a = (uint8_t)(a << 1);
    }
    return p;
}

void dmr_fec_rs129_encode(uint8_t lc[12], uint8_t dataType)
{
    /*
     * Feedback shift register for g(x) = (x + a)(x + a^2)(x + a^3), whose
     * coefficients x^3..x^0 are {1, 0x0E, 0x38, 0x40}.  The parity leaves the
     * register as parity[2], parity[1], parity[0] -- the order the standard
     * and every implementation put on the air.
     */
    static const uint8_t polyLow[3] = { 0x40, 0x38, 0x0E };
    uint8_t par[3] = { 0, 0, 0 };

    for (int i = 0; i < 9; i++) {
        uint8_t fb = (uint8_t)(lc[i] ^ par[2]);
        uint8_t p2 = (uint8_t)(par[1] ^ gf_mul(polyLow[2], fb));
        uint8_t p1 = (uint8_t)(par[0] ^ gf_mul(polyLow[1], fb));

        par[0] = gf_mul(polyLow[0], fb);
        par[1] = p1;
        par[2] = p2;
    }

    uint8_t mask = 0x00;

    if (dataType == DMR_DT_VOICE_LC_HEADER)
        mask = 0x96;
    else if (dataType == DMR_DT_TERMINATOR_LC)
        mask = 0x99;

    lc[9] = (uint8_t)(par[2] ^ mask);
    lc[10] = (uint8_t)(par[1] ^ mask);
    lc[11] = (uint8_t)(par[0] ^ mask);
}
