/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "protocols/NXDN/NxdnFec.h"
#include "protocols/DMR/DmrDefs.h"

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
 * Hard-decision Viterbi over the depunctured block.  The trellis is tiny (36
 * steps, 16 states) and terminates in state 0 through the four tail bits, so a
 * full-traceback table costs 36 bytes and nothing else.
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
                /* coded[] holds -1 for an erasure, which costs nothing */
                if (coded[2 * t] >= 0)
                    m += (coded[2 * t] == g1) ? 1 : -1;
                if (coded[2 * t + 1] >= 0)
                    m += (coded[2 * t + 1] == g2) ? 1 : -1;

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

bool nxdnFecSacch(const uint8_t bits[NXDN_SACCH_BITS], uint8_t out[26])
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
        deint[(t % 5) * 12 + (t / 5)] = (int8_t)(bits[t] & 1u);

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
        coded[i] = -1;

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
        planes[dmr_rw[i]][dmr_rx[i]] = (int8_t)((dibits[i] >> 1) & 1u);
        planes[dmr_ry[i]][dmr_rz[i]] = (int8_t)(dibits[i] & 1u);
    }

    memset(frame, 0, NXDN_AMBE_BYTES);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            int t = dmr_ambe_layout[r][c];

            if ((t >= 0) && (planes[r][c] != 0))
                frame[t >> 3] |= (uint8_t)(0x80u >> (t & 7));
        }
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
