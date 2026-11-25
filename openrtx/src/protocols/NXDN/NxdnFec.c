/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "protocols/NXDN/NxdnFec.h"

/*
 * Voice frame geometry.  NXDN and DMR carry the same AMBE+2 frame; only the
 * packing into the burst differs, so the dibit schedule that turns the 72
 * on-air bits into mbelib's ambe_fr[4][24] planes is the DMR one.  It used to
 * be exported by protocols/DMR/DmrDefs.h; when the DMR package took over that
 * directory the schedule came here, which is now its only user.
 */

/* 72 on-air bits of a voice frame (36 dibits, MSB first) -> the planes. */
static const int8_t ambe_rw[36] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                                    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2,
                                    0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2 };

static const int8_t ambe_rx[36] = { 23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5,
                                    17, 4,  16, 3, 15, 2, 14, 1, 13, 0, 12, 10,
                                    11, 9,  10, 8, 9,  7, 8,  6, 7,  5, 6,  4 };

static const int8_t ambe_ry[36] = { 0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3,
                                    1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
                                    1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3 };

static const int8_t ambe_rz[36] = { 5,  3,  4,  2,  3,  1, 2,  0, 1,  13, 0,  12,
                                    22, 11, 21, 10, 20, 9, 19, 8, 18, 7,  17, 6,
                                    16, 5,  15, 4,  14, 3, 13, 2, 12, 1,  11, 0 };

/*
 * ambe_fr[row][col] -> transmission-order bit index.  Matches mbelib's
 * mbe_eccAmbe3600Data_common: A = row0[23..0], B = row1[22..0],
 * C = row2[10..0] followed by row3[13..0].
 */
static const int8_t ambe_layout[4][24] = {
    { 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12,
      11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0 },
    { 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35,
      34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, -1 },
    { 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 71, 70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60,
      59, 58, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
};

void nxdnFecDescramble(uint8_t dibits[182], bool inverted)
{
    for (int i = 0; i < 182; i++) {
        unsigned mask = ((nxdnScramble[i >> 3] >> (i & 7)) & 1u);

        if ((mask != 0u) != inverted)
            dibits[i] ^= 0x02;
    }
}

bool nxdnFecLich(const uint8_t dibits[NXDN_LICH_DIBITS], struct nxdnLich *lich)
{
    uint8_t raw = 0;
    int parity = 0;

    for (int i = 0; i < NXDN_LICH_DIBITS; i++) {
        /* The data bit is the sign bit of the dibit (4.5.3 Figure 4.5-7) */
        int bit = (dibits[i] >> 1) & 1u;
        raw = (uint8_t)((raw << 1) | (uint8_t)bit);
        if (i < 4)
            parity ^= bit;
    }

    lich->parityOk = (((raw & 1u) != 0) == (parity != 0));
    nxdnLichParse((uint8_t)(raw >> 1), lich);

    return lich->parityOk;
}

/* ------------------------------------------------------------------ */
/* SACCH                                                              */
/* ------------------------------------------------------------------ */

/**
 * CRC-6 of a SACCH block, X^6 + X^5 + X^2 + X + 1 over the 26 information
 * bits, compared with the 6 bits that follow them.  The register starts all
 * ones, which is what the reference decoders use and what a transmitter has to
 * match; the polynomial terms are folded into the shift above (X^5, X^2 and
 * X of the generator on top of the X^6 that falls out of the register).
 */
static bool sacch_crc_ok(const uint8_t bits[32])
{
    uint8_t s[6] = { 1, 1, 1, 1, 1, 1 };

    for (int i = 0; i < 26; i++) {
        uint8_t a = (uint8_t)((bits[i] & 1u) ^ s[0]);

        s[0] = (uint8_t)(a ^ s[1]);
        s[1] = s[2];
        s[2] = s[3];
        s[3] = (uint8_t)(a ^ s[4]);
        s[4] = (uint8_t)(a ^ s[5]);
        s[5] = a;
    }

    uint32_t crc = 0;
    for (int i = 0; i < 6; i++)
        crc = (crc << 1) | (s[i] & 1u);

    uint32_t received = 0;
    for (int i = 26; i < 32; i++)
        received = (received << 1) | (bits[i] & 1u);

    return crc == received;
}

/*
 * The punctured rate 1/2 K=5 convolutional code of the SACCH
 * (G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4).  Puncturing keeps every G1
 * bit and the G2 bit of the first two and the last two of every six pairs, so
 * 36 input bits leave 60 of the 72 coded bits and twelve positions are
 * erasures on the way back (4.5.2.1).
 *
 * Puncturing matrix:
 *      1 1 1 1 1 1
 *      1 1 0 1 1 0
 */
#define SACCH_STEPS 36
#define SACCH_STATES 16

static inline void conv_encode_step(int state, int input, int *g1, int *g2)
{
    /* state holds u[-1]..u[-4] in bits 0..3, bit 0 the most recent */
    int b0 = state & 1;
    int b1 = (state >> 1) & 1;
    int b2 = (state >> 2) & 1;
    int b3 = (state >> 3) & 1;

    *g1 = input ^ b2 ^ b3;        /* 1 + D^3 + D^4 */
    *g2 = input ^ b0 ^ b1 ^ b3;   /* 1 + D + D^2 + D^4 */
}

/**
 * Viterbi over the depunctured block, on signed soft values.
 *
 * coded[] is one entry per coded bit: positive means "this bit is a 1, and by
 * this much", negative means "a 0, by this much", and zero is an erasure (a
 * punctured position) and costs nothing.  A hard decision is the special case
 * +-1, so the hard path through this decoder is bit for bit what it always
 * was.  The branch metric adds the soft value when the transition's coded bit
 * matches the observed sign and subtracts it when it does not, which is the
 * maximum-likelihood metric of a memoryless channel: the decoder now weighs a
 * bit read at the edge of a level as much as it deserves and not the same as
 * one read cleanly.
 *
 * The trellis is tiny (36 steps, 16 states) and terminates in state 0 through
 * the four tail bits, so a full-traceback table costs 36 bytes and nothing
 * else.
 */
static void sacch_viterbi(const int8_t coded[72], uint8_t out[SACCH_STEPS])
{
    int32_t metric[SACCH_STATES];
    int32_t next[SACCH_STATES];
    uint8_t from[SACCH_STEPS][SACCH_STATES];

    for (int s = 0; s < SACCH_STATES; s++)
        metric[s] = (s == 0) ? 0 : -0x3FFF;

    memset(from, 0, sizeof(from));

    for (int t = 0; t < SACCH_STEPS; t++) {
        for (int s = 0; s < SACCH_STATES; s++)
            next[s] = -0x3FFF;

        for (int s = 0; s < SACCH_STATES; s++) {
            if (metric[s] <= -0x3FFF)
                continue;

            for (int u = 0; u < 2; u++) {
                int g1, g2;
                conv_encode_step(s, u, &g1, &g2);

                int32_t m = metric[s];

                m += (g1 != 0) ? coded[2 * t] : -coded[2 * t];
                m += (g2 != 0) ? coded[2 * t + 1] : -coded[2 * t + 1];

                int ns = ((s << 1) | u) & (SACCH_STATES - 1);
                if (m > next[ns]) {
                    next[ns] = m;
                    from[t][ns] = (uint8_t)s;
                }
            }
        }

        memcpy(metric, next, sizeof(metric));
    }

    /* The four tail bits are zero, so the survivor ends in state 0 */
    int s = 0;
    for (int t = SACCH_STEPS - 1; t >= 0; t--) {
        out[t] = (uint8_t)(s & 1);
        s = from[t][s];
    }
}

/**
 * Decode one block from signed soft values: de-interleave, un-puncture, run
 * the Viterbi and check the CRC-6.  shared by the hard and soft entry points.
 */
static bool sacch_decode(const int8_t soft[NXDN_SACCH_BITS], uint8_t out[26])
{
    int8_t deint[60];
    int8_t coded[72];
    uint8_t decoded[SACCH_STEPS];

    /*
     * De-interleave: the encoder splits the 60 bits into five rows of twelve
     * (D1..D5) and transmits them column by column, so the n-th transmitted
     * bit is column n/5 of row n%5.
     */
    for (int t = 0; t < NXDN_SACCH_BITS; t++)
        deint[(t % 5) * 12 + (t / 5)] = soft[t];

    /*
     * Un-puncture.  Every G1 bit of the 72 survives, and so does the G2 bit of
     * pairs 0, 1, 3 and 4 of each group of six, which leaves the ten bits of a
     * group in this order:
     *
     *   +0 g1_0  +1 g2_0  +2 g1_1  +3 g2_1  +4 g1_2
     *   +5 g1_3  +6 g2_3  +7 g1_4  +8 g2_4  +9 g1_5
     *
     * and the two erased G2 bits (pairs 2 and 5) are erasures in the decoder.
     */
    static const int8_t group_pair[10] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5 };
    static const int8_t group_is_g2[10] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0 };

    for (int i = 0; i < 72; i++)
        coded[i] = 0;

    for (int p = 0; p < 6; p++) {
        for (int b = 0; b < 10; b++) {
            int pair = p * 6 + group_pair[b];

            coded[2 * pair + group_is_g2[b]] = deint[p * 10 + b];
        }
    }

    sacch_viterbi(coded, decoded);

    if (!sacch_crc_ok(decoded))
        return false;

    for (int i = 0; i < 26; i++)
        out[i] = decoded[i];

    return true;
}

bool nxdnFecSacch(const uint8_t bits[NXDN_SACCH_BITS], uint8_t out[26])
{
    int8_t soft[NXDN_SACCH_BITS];

    for (int i = 0; i < NXDN_SACCH_BITS; i++)
        soft[i] = (bits[i] & 1u) ? 1 : -1;

    return sacch_decode(soft, out);
}

bool nxdnFecSacchSoft(const int8_t soft[NXDN_SACCH_BITS], uint8_t out[26])
{
    return sacch_decode(soft, out);
}

/* ------------------------------------------------------------------ */
/* Voice frame mapping                                                */
/* ------------------------------------------------------------------ */

void nxdnFecVchToAmbe(const uint8_t dibits[NXDN_VCH_DIBITS],
                      uint8_t frame[NXDN_AMBE_BYTES])
{
    int8_t planes[4][24];

    memset(planes, 0, sizeof(planes));

    for (int i = 0; i < 36; i++) {
        /* Sign bit of the dibit first, then its magnitude bit: the same
         * schedule the DMR voice frame uses. */
        planes[ambe_rw[i]][ambe_rx[i]] = (int8_t)((dibits[i] >> 1) & 1u);
        planes[ambe_ry[i]][ambe_rz[i]] = (int8_t)(dibits[i] & 1u);
    }

    memset(frame, 0, NXDN_AMBE_BYTES);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            int t = ambe_layout[r][c];

            if ((t >= 0) && (planes[r][c] != 0))
                frame[t >> 3] |= (uint8_t)(0x80u >> (t & 7));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Transmitting halves                                                */
/* ------------------------------------------------------------------ */

void nxdnFecScramble(uint8_t dibits[182], bool inverted)
{
    /* The sign bit XOR is its own inverse */
    nxdnFecDescramble(dibits, inverted);
}

void nxdnFecLichEncode(uint8_t control, uint8_t dibits[NXDN_LICH_DIBITS])
{
    /*
     * Figure 4.5-7: eight coding bits, the seven control bits MSB first and
     * an even parity bit over the four MSBs; each goes out as one dibit whose
     * sign bit is the data bit.  Every LICH symbol is an outer one, so the
     * magnitude bit stays set.
     */
    uint8_t bit[8];

    for (int i = 0; i < 7; i++)
        bit[i] = (uint8_t)((control >> (6 - i)) & 1u);

    bit[7] = (uint8_t)(bit[0] ^ bit[1] ^ bit[2] ^ bit[3]);

    for (int i = 0; i < NXDN_LICH_DIBITS; i++)
        dibits[i] = (uint8_t)((bit[i] << 1) | 1u);
}

void nxdnFecSacchEncode(const uint8_t info[26], uint8_t bits[NXDN_SACCH_BITS])
{
    uint8_t in[36];
    uint8_t pair[36][2];   /* the 72 coded bits, G1 then G2 per step */
    uint8_t coded[60];
    int state = 0;

    /*
     * The CRC-6 is the register of sacch_crc_ok() read out after the 26
     * information bits: the decoder compares exactly that with the six bits
     * that follow them, so the transmitter appends the register contents.
     */
    uint8_t s[6] = { 1, 1, 1, 1, 1, 1 };

    for (int i = 0; i < 26; i++) {
        uint8_t a = (uint8_t)((info[i] & 1u) ^ s[0]);

        s[0] = (uint8_t)(a ^ s[1]);
        s[1] = s[2];
        s[2] = s[3];
        s[3] = (uint8_t)(a ^ s[4]);
        s[4] = (uint8_t)(a ^ s[5]);
        s[5] = a;
    }

    memcpy(in, info, 26);
    memcpy(&in[26], s, 6);

    /* Four zero tail bits close the trellis in state 0 */
    memset(&in[32], 0, 4);

    for (int t = 0; t < SACCH_STEPS; t++) {
        int g1, g2;

        conv_encode_step(state, in[t] & 1u, &g1, &g2);
        pair[t][0] = (uint8_t)g1;
        pair[t][1] = (uint8_t)g2;
        state = ((state << 1) | (in[t] & 1u)) & (SACCH_STATES - 1);
    }

    /*
     * Puncture to 60 bits in the inverse order of the decoder's
     * un-puncturing: per group of six pairs, ten bits leave as
     * g1_0 g2_0 g1_1 g2_1 g1_2 g1_3 g2_3 g1_4 g2_4 g1_5.
     */
    static const int8_t group_pair[10] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5 };
    static const int8_t group_is_g2[10] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0 };

    for (int p = 0; p < 6; p++)
        for (int b = 0; b < 10; b++)
            coded[p * 10 + b] = pair[p * 6 + group_pair[b]][group_is_g2[b]];

    /*
     * Interleave: the decoder writes transmitted bit t into
     * deint[(t % 5) * 12 + t / 5], so the encoder sends the n-th bit of its
     * 60-bit block at t = (n % 12) * 5 + n / 12.
     */
    for (int n = 0; n < NXDN_SACCH_BITS; n++)
        bits[(n % 12) * 5 + n / 12] = coded[n];
}

void nxdnFecVchFromAmbe(const uint8_t frame[NXDN_AMBE_BYTES],
                        uint8_t dibits[NXDN_VCH_DIBITS])
{
    int8_t planes[4][24];

    memset(planes, 0, sizeof(planes));

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            int t = ambe_layout[r][c];

            if (t >= 0)
                planes[r][c] = (int8_t)((frame[t >> 3] >> (7 - (t & 7))) & 1u);
        }
    }

    for (int i = 0; i < NXDN_VCH_DIBITS; i++) {
        const uint8_t sign = (uint8_t)planes[ambe_rw[i]][ambe_rx[i]];
        const uint8_t mag = (uint8_t)planes[ambe_ry[i]][ambe_rz[i]];

        dibits[i] = (uint8_t)((sign << 1) | mag);
    }
}

/* ------------------------------------------------------------------ */
/* SACCH layer 3                                                      */
/* ------------------------------------------------------------------ */

static uint32_t bits_to_u32(const uint8_t *bits, int first, int count)
{
    uint32_t v = 0;

    for (int i = 0; i < count; i++)
        v = (v << 1) | (bits[first + i] & 1u);

    return v;
}

void nxdnFecSacchParse(const uint8_t bits[72], struct nxdnSacchMessage *msg)
{
    memset(msg, 0, sizeof(*msg));
    memcpy(msg->bits, bits, 72);

    for (int o = 0; o < 9; o++)
        msg->octets[o] = (uint8_t)bits_to_u32(bits, o * 8, 8);

    msg->type = msg->octets[0];
    msg->ccOption = msg->octets[1];

    switch (msg->type) {
    case NXDN_MSG_VCALL:
    case NXDN_MSG_TX_REL_EX:
    case NXDN_MSG_TX_REL:
    case NXDN_MSG_DISC:
        msg->callType = (uint8_t)bits_to_u32(bits, 16, 3);
        msg->source = (uint16_t)bits_to_u32(bits, 24, 16);
        msg->dest = (uint16_t)bits_to_u32(bits, 40, 16);
        msg->valid = true;
        break;
    default:
        break;
    }
}

const char *nxdnFecMsgName(uint8_t type)
{
    switch (type) {
    case NXDN_MSG_VCALL:
        return "VCALL";
    case NXDN_MSG_VCALL_IV:
        return "VCALL_IV";
    case NXDN_MSG_TX_REL_EX:
        return "TX_REL_EX";
    case NXDN_MSG_TX_REL:
        return "TX_REL";
    case NXDN_MSG_DISC:
        return "DISC";
    case NXDN_MSG_ALIAS:
        return "ALIAS";
    default:
        break;
    }

    return "MSG";
}
