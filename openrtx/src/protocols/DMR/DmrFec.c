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

/** Correct one bit of a Hamming(15,11,3) word: DMR "second" variant. */
static bool hamming_15113_fix(uint8_t d[15])
{
    uint8_t c0 = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    uint8_t c1 = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    uint8_t c2 = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    uint8_t c3 = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
    unsigned syndrome = 0;

    syndrome |= (c0 != d[11]) ? 0x01u : 0u;
    syndrome |= (c1 != d[12]) ? 0x02u : 0u;
    syndrome |= (c2 != d[13]) ? 0x04u : 0u;
    syndrome |= (c3 != d[14]) ? 0x08u : 0u;

    switch (syndrome) {
        case 0x01u: d[11] ^= 1; return true;
        case 0x02u: d[12] ^= 1; return true;
        case 0x04u: d[13] ^= 1; return true;
        case 0x08u: d[14] ^= 1; return true;
        case 0x09u: d[0] ^= 1; return true;
        case 0x0Bu: d[1] ^= 1; return true;
        case 0x0Fu: d[2] ^= 1; return true;
        case 0x07u: d[3] ^= 1; return true;
        case 0x0Eu: d[4] ^= 1; return true;
        case 0x05u: d[5] ^= 1; return true;
        case 0x0Au: d[6] ^= 1; return true;
        case 0x0Du: d[7] ^= 1; return true;
        case 0x03u: d[8] ^= 1; return true;
        case 0x06u: d[9] ^= 1; return true;
        case 0x0Cu: d[10] ^= 1; return true;
        default: return false;
    }
}

/** Correct one bit of a Hamming(13,9,3) word: BPTC columns. */
static bool hamming_1393_fix(uint8_t d[13])
{
    uint8_t c0 = d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6];
    uint8_t c1 = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7];
    uint8_t c2 = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    uint8_t c3 = d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[8];
    unsigned syndrome = 0;

    syndrome |= (c0 != d[9]) ? 0x01u : 0u;
    syndrome |= (c1 != d[10]) ? 0x02u : 0u;
    syndrome |= (c2 != d[11]) ? 0x04u : 0u;
    syndrome |= (c3 != d[12]) ? 0x08u : 0u;

    switch (syndrome) {
        case 0x01u: d[9] ^= 1; return true;
        case 0x02u: d[10] ^= 1; return true;
        case 0x04u: d[11] ^= 1; return true;
        case 0x08u: d[12] ^= 1; return true;
        case 0x0Fu: d[0] ^= 1; return true;
        case 0x07u: d[1] ^= 1; return true;
        case 0x0Eu: d[2] ^= 1; return true;
        case 0x05u: d[3] ^= 1; return true;
        case 0x0Au: d[4] ^= 1; return true;
        case 0x0Du: d[5] ^= 1; return true;
        case 0x03u: d[6] ^= 1; return true;
        case 0x06u: d[7] ^= 1; return true;
        case 0x0Cu: d[8] ^= 1; return true;
        default: return false;
    }
}

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
    int pos;

    if (syndrome == 0)
        return 0;

    /*
     * Columns of the parity-check matrix for the seven codeword positions
     * {AT, TC, LS1, LS0, H0, H1, H2}.
     */
    switch (syndrome) {
        case 0x5: pos = 0; break;
        case 0x7: pos = 1; break;
        case 0x3: pos = 2; break;
        case 0x6: pos = 3; break;
        case 0x1: pos = 4; break;
        case 0x2: pos = 5; break;
        case 0x4: pos = 6; break;
        default: return -1;
    }

    cach[dmr_cach_tact_pos[pos]] ^= 1;
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
