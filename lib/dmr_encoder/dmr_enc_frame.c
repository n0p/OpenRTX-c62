/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 * SPDX-FileCopyrightText: 2013-2016 Max H. Parke KA1RBI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_enc_frame.c
 * @brief AMBE+2 parameter quantiser, rate-1/2 Golay FEC and frame packing.
 *
 * The second half of the encoder: it takes the model parameters of the
 * analysis (dmr_enc_core.c) and produces the 9-byte / 72-bit frame that DMR
 * carries, in the canonical A|B|C order lib/dmr_vocoder decodes.
 *
 *   parameters -> b[0..8] (49 bits) -> 9 bytes, A(24) Golay(24,12) |
 *   B(23) Golay(23,12) x PRNG | C(25) raw
 *
 * Two things come from outside the IMBE analysis and are worth naming:
 *
 *  - **The quantiser is the OP25 one** (mbeenc.cpp).  It is not the reference
 *    of the AMBE+2 standard, it is the encoder OP25 ships, and it is what the
 *    OpenDMR library does — that is what makes the output bit-exact with the
 *    reference we measure against.
 *
 *  - **The predictor state is a decoder.**  After choosing b[] the reference
 *    decodes its own 49 bits with mbelib's parameter decoder
 *    (mbe_decodeAmbe2450Parms) and keeps the reconstructed gain and log
 *    magnitudes for the next frame's prediction.  That is ported here
 *    (dmr_enc_model()), minus everything the encoder never reads: the
 *    amplitudes, the phases, the synthesis state.  It is the same arithmetic
 *    in the same order, with the cosines and the logs as tables
 *    (dmr_enc_trig.c) so that no libm transcendental runs at frame time.
 *
 * The Golay codewords are computed, not looked up: the reference keeps two
 * 4096-entry 32-bit tables (32 KiB of flash in firmware terms) for
 * `ENCODING_TABLE_23127/24128`, which is the same polynomial division this
 * file does in twelve XOR steps.  A unit test checks all 4096 codewords
 * against the tables.
 */

#include <math.h>
#include <string.h>

#include "dmr_enc_int.h"
#include "dmr_enc_ambe_tables.h"
#include "dmr_enc_trig.h"

/* ------------------------------------------------------------------------
 * Parameter history.  Field for field what the encoder reads of mbelib's
 * `mbe_parms`: the amplitudes and the phases are not among them.
 * ------------------------------------------------------------------------ */
typedef struct {
    float gamma;      /**< gain, the reference's gamma */
    int L;            /**< number of harmonics */
    int Vl[57];       /**< voiced/unvoiced per harmonic */
    float log2Ml[57]; /**< log2 spectral magnitudes */
} mbe_parms_t;

static mbe_parms_t cur_mp;
static mbe_parms_t prev_mp;

/* The reference's d_gain_adjust, in the log domain. */
#define DMR_ENC_GAIN_ADJUST 1.0f

/* Q15 of sqrt(2), the reference's `static const float SQRT_2 = sqrtf(2.0)`. */
#define SQRT_2 0x1.6a09e6p+0f

/* ------------------------------------------------------------------------
 * Golay (24,12) / (23,12), rate 1/2, the DMR voice FEC (ETSI TS 102 361-1
 * annex B).  Generator polynomial x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1.
 * ------------------------------------------------------------------------ */
#define GOLAY_POLY 0x0C75U

/** 23-bit Golay codeword of 12 data bits, aligned in bits 22..1 as OP25 packs it. */
static uint32_t golay_code23(uint32_t data)
{
    uint32_t p = (data & 0xFFFU) << 11;
    for (int i = 11; i >= 0; i--) {
        if (p & (1U << (i + 11)))
            p ^= GOLAY_POLY << i;
    }
    return ((data & 0xFFFU) << 11) | (p & 0x7FFU);
}

/** Parity of a 23-bit word, folded by hand: no libgcc __paritysi2 call. */
static uint32_t parity23(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    return (0x6996U >> (v & 0xFU)) & 1U;
}

/** 24-bit Golay codeword: the 23-bit one plus the overall even parity bit. */
static uint32_t golay_code24(uint32_t data)
{
    uint32_t c = golay_code23(data);
    return (c << 1) | parity23(c);
}

/**
 * PRNG mask of the B block: the LCG mbelib and OP25 use
 * (pr[i] = 173 * pr[i-1] + 13849, modulo 2^16, MSB of each state).
 */
static uint32_t prng_mask(uint32_t c0)
{
    uint16_t pr = (uint16_t)(16U * (c0 & 0xFFFU));
    uint32_t mask = 0;

    for (int i = 1; i <= 23; i++) {
        pr = (uint16_t)((173U * pr + 13849U) & 0xFFFFU);
        if ((pr >> 15) & 1U)
            mask |= 1U << (23 - i);
    }
    return mask;
}

/* ------------------------------------------------------------------------
 * 49-bit parameter vector, in the bit order the frame uses.
 * ------------------------------------------------------------------------ */
static void pack_49(uint8_t d[49], const int b[9])
{
    /* Written out explicitly: the interleaving of the low bits of b0..b8 in
     * positions 35..48 is what the AMBE+2 bit allocation dictates, not a
     * packing order one can derive.  Same table as the reference. */
    d[0] = (uint8_t)((b[0] >> 6) & 1);  d[1] = (uint8_t)((b[0] >> 5) & 1);
    d[2] = (uint8_t)((b[0] >> 4) & 1);  d[3] = (uint8_t)((b[0] >> 3) & 1);
    d[4] = (uint8_t)((b[1] >> 4) & 1);  d[5] = (uint8_t)((b[1] >> 3) & 1);
    d[6] = (uint8_t)((b[1] >> 2) & 1);  d[7] = (uint8_t)((b[1] >> 1) & 1);
    d[8] = (uint8_t)((b[2] >> 4) & 1);  d[9] = (uint8_t)((b[2] >> 3) & 1);
    d[10] = (uint8_t)((b[2] >> 2) & 1); d[11] = (uint8_t)((b[2] >> 1) & 1);
    d[12] = (uint8_t)((b[3] >> 8) & 1); d[13] = (uint8_t)((b[3] >> 7) & 1);
    d[14] = (uint8_t)((b[3] >> 6) & 1); d[15] = (uint8_t)((b[3] >> 5) & 1);
    d[16] = (uint8_t)((b[3] >> 4) & 1); d[17] = (uint8_t)((b[3] >> 3) & 1);
    d[18] = (uint8_t)((b[3] >> 2) & 1); d[19] = (uint8_t)((b[3] >> 1) & 1);
    d[20] = (uint8_t)((b[4] >> 6) & 1); d[21] = (uint8_t)((b[4] >> 5) & 1);
    d[22] = (uint8_t)((b[4] >> 4) & 1); d[23] = (uint8_t)((b[4] >> 3) & 1);
    d[24] = (uint8_t)((b[5] >> 4) & 1); d[25] = (uint8_t)((b[5] >> 3) & 1);
    d[26] = (uint8_t)((b[5] >> 2) & 1); d[27] = (uint8_t)((b[5] >> 1) & 1);
    d[28] = (uint8_t)((b[6] >> 3) & 1); d[29] = (uint8_t)((b[6] >> 2) & 1);
    d[30] = (uint8_t)((b[6] >> 1) & 1); d[31] = (uint8_t)((b[7] >> 3) & 1);
    d[32] = (uint8_t)((b[7] >> 2) & 1); d[33] = (uint8_t)((b[7] >> 1) & 1);
    d[34] = (uint8_t)((b[8] >> 2) & 1);
    d[35] = (uint8_t)(b[1] & 1);        d[36] = (uint8_t)(b[2] & 1);
    d[37] = (uint8_t)((b[0] >> 2) & 1); d[38] = (uint8_t)((b[0] >> 1) & 1);
    d[39] = (uint8_t)(b[0] & 1);        d[40] = (uint8_t)(b[3] & 1);
    d[41] = (uint8_t)((b[4] >> 2) & 1); d[42] = (uint8_t)((b[4] >> 1) & 1);
    d[43] = (uint8_t)(b[4] & 1);        d[44] = (uint8_t)(b[5] & 1);
    d[45] = (uint8_t)(b[6] & 1);        d[46] = (uint8_t)(b[7] & 1);
    d[47] = (uint8_t)((b[8] >> 1) & 1); d[48] = (uint8_t)(b[8] & 1);
}

/* b[0] from the pitch: the OP25 encoder's lookup, 898 entries. */
#define b0_lookup dmr_enc_b0_lookup
#define b0_lmax DMR_ENC_B0_LOOKUP_LEN

/**
 * Decode the 49 bits back into model parameters — the reference's
 * `mbe_decodeAmbe2450Parms`, without the amplitudes and the phases.
 */
static void model_decode(const uint8_t d[49], mbe_parms_t *cur, mbe_parms_t *prev)
{
    float Cik[5][18], Tl[57] = {0};
    float Gm[9], Ri[9];
    int Ji[5], intkl[57];
    float flokl[57], deltal[57];
    float Sum42, Sum43, sum, c1, c2, deltaGamma, BigGamma;
    int i, j, k, l, L = 0, m, am, ak, jl;
    int b0, b1, b2, b3, b4, b5, b6, b7, b8;
    float f0;
    int silence = 0;

    /* fundamental frequency index, 7 bits spread over the frame */
    b0 = (d[0] << 6) | (d[1] << 5) | (d[2] << 4) | (d[3] << 3) |
         (d[37] << 2) | (d[38] << 1) | d[39];

    memset(Cik, 0, sizeof(Cik));
    memset(Gm, 0, sizeof(Gm));
    memset(Ri, 0, sizeof(Ri));

    if (b0 >= 120) {
        /* Tone / erasure / silence indices above the quantiser's range: the
         * reference returns without touching the parameters.  The encoder
         * never produces them (AmbeLtable tops out at b0 = 119) but a corrupted
         * frame must not read past the tables. */
        return;
    }

    if (b0 == 124 || b0 == 125) {
        silence = 1;
        f0 = (float)1 / (float)32;
        L = 14;
        cur->L = 14;
        for (l = 1; l <= L; l++)
            cur->Vl[l] = 0;
    }

    if (!silence) {
        f0 = dmr_enc_AmbeW0table[b0];
        L = dmr_enc_AmbeLtable[b0];
        cur->L = L;
    }

    /* V/UV vector */
    b1 = (d[4] << 4) | (d[5] << 3) | (d[6] << 2) | (d[7] << 1) | d[35];
    for (l = 1; l <= L; l++) {
        jl = (int)((float)l * (float)16.0 * f0);
        if (!silence && jl <= 7)
            cur->Vl[l] = dmr_enc_AmbeVuv[b1][jl];
    }

    /* gain: delta from the table plus half of the previous one */
    b2 = (d[8] << 4) | (d[9] << 3) | (d[10] << 2) | (d[11] << 1) | d[36];
    deltaGamma = dmr_enc_AmbeDg[b2];
    cur->gamma = deltaGamma + ((float)0.5 * prev->gamma);

    /* PRBA vectors -> Ri */
    b3 = (d[12] << 8) | (d[13] << 7) | (d[14] << 6) | (d[15] << 5) |
         (d[16] << 4) | (d[17] << 3) | (d[18] << 2) | (d[19] << 1) | d[40];
    Gm[2] = dmr_enc_AmbePRBA24[b3][0];
    Gm[3] = dmr_enc_AmbePRBA24[b3][1];
    Gm[4] = dmr_enc_AmbePRBA24[b3][2];

    b4 = (d[20] << 6) | (d[21] << 5) | (d[22] << 4) | (d[23] << 3) |
         (d[41] << 2) | (d[42] << 1) | d[43];
    Gm[5] = dmr_enc_AmbePRBA58[b4][0];
    Gm[6] = dmr_enc_AmbePRBA58[b4][1];
    Gm[7] = dmr_enc_AmbePRBA58[b4][2];
    Gm[8] = dmr_enc_AmbePRBA58[b4][3];

    for (i = 1; i <= 8; i++) {
        sum = 0;
        for (m = 1; m <= 8; m++) {
            am = (m == 1) ? 1 : 2;
            sum = sum + ((float)am * Gm[m] * dmr_enc_cos_prba_m[m][i]);
        }
        Ri[i] = sum;
    }

    {
        /* 1/(2*sqrt(2)) evaluated in double as the reference does, then
         * rounded to float once, at build time. */
        const float rconst = 0x1.6a09e6p-2f;
        Cik[1][1] = (float)0.5 * (Ri[1] + Ri[2]);
        Cik[1][2] = rconst * (Ri[1] - Ri[2]);
        Cik[2][1] = (float)0.5 * (Ri[3] + Ri[4]);
        Cik[2][2] = rconst * (Ri[3] - Ri[4]);
        Cik[3][1] = (float)0.5 * (Ri[5] + Ri[6]);
        Cik[3][2] = rconst * (Ri[5] - Ri[6]);
        Cik[4][1] = (float)0.5 * (Ri[7] + Ri[8]);
        Cik[4][2] = rconst * (Ri[7] - Ri[8]);
    }

    b5 = (d[24] << 4) | (d[25] << 3) | (d[26] << 2) | (d[27] << 1) | d[44];
    b6 = (d[28] << 3) | (d[29] << 2) | (d[30] << 1) | d[45];
    b7 = (d[31] << 3) | (d[32] << 2) | (d[33] << 1) | d[46];
    b8 = (d[34] << 2) | (d[47] << 1) | d[48];

    Ji[1] = dmr_enc_AmbeLmprbl[L][0];
    Ji[2] = dmr_enc_AmbeLmprbl[L][1];
    Ji[3] = dmr_enc_AmbeLmprbl[L][2];
    Ji[4] = dmr_enc_AmbeLmprbl[L][3];

    for (k = 3; k <= Ji[1]; k++)
        Cik[1][k] = (k > 6) ? 0.0f : dmr_enc_AmbeHOCb5[b5][k - 3];
    for (k = 3; k <= Ji[2]; k++)
        Cik[2][k] = (k > 6) ? 0.0f : dmr_enc_AmbeHOCb6[b6][k - 3];
    for (k = 3; k <= Ji[3]; k++)
        Cik[3][k] = (k > 6) ? 0.0f : dmr_enc_AmbeHOCb7[b7][k - 3];
    for (k = 3; k <= Ji[4]; k++)
        Cik[4][k] = (k > 6) ? 0.0f : dmr_enc_AmbeHOCb8[b8][k - 3];

    /* inverse DCT per block */
    l = 1;
    for (i = 1; i <= 4; i++) {
        int ji = Ji[i];
        for (j = 1; j <= ji; j++) {
            sum = 0;
            for (k = 1; k <= ji; k++) {
                ak = (k == 1) ? 1 : 2;
                sum = sum + ((float)ak * Cik[i][k] * DCT_COS_M(ji, j, k));
            }
            Tl[l] = sum;
            l++;
        }
    }

    /* predictor: log2 magnitudes from the previous frame's */
    if (cur->L > prev->L) {
        for (l = prev->L + 1; l <= cur->L; l++)
            prev->log2Ml[l] = prev->log2Ml[prev->L];
    }
    prev->log2Ml[0] = prev->log2Ml[1];

    Sum43 = 0;
    for (l = 1; l <= cur->L; l++) {
        flokl[l] = ((float)prev->L / (float)cur->L) * (float)l;
        intkl[l] = (int)flokl[l];
        deltal[l] = flokl[l] - (float)intkl[l];
        Sum43 = Sum43 + ((((float)1 - deltal[l]) * prev->log2Ml[intkl[l]]) +
                         (deltal[l] * prev->log2Ml[intkl[l] + 1]));
    }
    Sum43 = ((float)0.65 / (float)cur->L) * Sum43;

    Sum42 = 0;
    for (l = 1; l <= cur->L; l++)
        Sum42 += Tl[l];
    Sum42 = Sum42 / (float)cur->L;
    BigGamma = cur->gamma - ((float)0.5 * dmr_enc_log2_L[cur->L]) - Sum42;

    for (l = 1; l <= cur->L; l++) {
        c1 = (float)0.65 * ((float)1 - deltal[l]) * prev->log2Ml[intkl[l]];
        c2 = (float)0.65 * deltal[l] * prev->log2Ml[intkl[l] + 1];
        cur->log2Ml[l] = Tl[l] + c1 + c2 - Sum43 + BigGamma;
    }
}

/* ------------------------------------------------------------------------
 * The quantiser: analysis parameters -> b[0..8].
 * ------------------------------------------------------------------------ */
static void enc_ambe(const imbe_param_t *p, int b[9])
{
    float m2[NUM_HARMS_MAX];
    float C[4][17];
    float R[8], G[8];
    float lsa[NUM_HARMS_MAX];
    float T[NUM_HARMS_MAX];
    float num_harms_f, log_l_2, log_l_w0, lsa_sum, gain, diff_gain;
    float error = 0.0f, en_min = 0.0f;
    int b0_i, error_index, L, l, i1, n, i, k, j;

    b0_i = (p->ref_pitch >> 5) - 159;
    if (b0_i < 0 || b0_i >= b0_lmax)
        goto silence;

    b[0] = b0_lookup[b0_i];
    L = dmr_enc_AmbeLtable[b[0]];
    while (L != p->num_harms) {
        if (L < p->num_harms)
            b0_i++;
        else
            b0_i--;
        if (b0_i < 0 || b0_i >= b0_lmax)
            goto silence;
        b[0] = b0_lookup[b0_i];
        L = dmr_enc_AmbeLtable[b[0]];
    }

    /* squared magnitudes */
    for (l = 1; l <= L; l++) {
        m2[l - 1] = (float)p->sa[l - 1];
        m2[l - 1] = m2[l - 1] * m2[l - 1];
    }

    /* b[1]: voiced/unvoiced pattern */
    b[1] = 0;
    for (n = 0; n < 17; n++) {
        float En = 0;
        for (l = 1; l <= L; l++) {
            int jl = (int)((float)l * 16.0f * dmr_enc_AmbeW0table[b[0]]);
            if (jl > 7)
                jl = 7;
            if (p->v_uv_dsn[l - 1] != dmr_enc_AmbeVuv[n][jl])
                En += m2[l - 1];
        }
        if (n == 0)
            en_min = En;
        else if (En < en_min) {
            b[1] = n;
            en_min = En;
        }
    }

    /* log spectral amplitudes, in the two normalisations the standard uses */
    num_harms_f = (float)p->num_harms;
    log_l_2 = dmr_enc_log2_b0[b[0]][0];
    log_l_w0 = dmr_enc_log2_b0[b[0]][1];

    lsa_sum = 0.0f;
    for (i1 = 0; i1 < p->num_harms; i1++) {
        float sa = (float)p->sa[i1];
        if (sa < 1)
            sa = 1.0f;
        if (p->v_uv_dsn[i1])
            lsa[i1] = log_l_2 + log2f(sa);
        else
            lsa[i1] = log_l_w0 + log2f(sa);
        lsa_sum += lsa[i1];
    }

    /* b[2]: gain, as a delta against the prediction */
    gain = lsa_sum / num_harms_f;
    /* The reference carries a "gain adjust" of 1.0 (opendmr_encoder_create
     * sets it and nothing changes it): it is a linear multiplier of 1, i.e. a
     * constant -1.0 in the log domain, and it is part of the quantiser's
     * definition, so it is here too. */
    diff_gain = gain - (float)0.5 * prev_mp.gamma - DMR_ENC_GAIN_ADJUST;
    error_index = 0;
    for (i1 = 0; i1 < 32; i1++) {
        float diff = fabsf(diff_gain - dmr_enc_AmbeDg[i1]);
        if (i1 == 0 || diff < error) {
            error = diff;
            error_index = i1;
        }
    }
    b[2] = error_index;
#ifdef DMR_ENC_DEBUG
    extern int dmr_enc_dbg_gain[4];
    dmr_enc_dbg_gain[0] = (int)(lsa_sum * 1000);
    dmr_enc_dbg_gain[1] = (int)(gain * 1000);
    dmr_enc_dbg_gain[2] = (int)(diff_gain * 1000);
    dmr_enc_dbg_gain[3] = (int)(dmr_enc_log2_b0[b[0]][1] * 1000);
#endif

    /* prediction residuals */
    {
        float l_prev_l = (float)prev_mp.L / num_harms_f;
        prev_mp.log2Ml[0] = prev_mp.log2Ml[1];
        for (i1 = 0; i1 < p->num_harms; i1++) {
            float kl = l_prev_l * (float)(i1 + 1);
            int kl_floor = (int)kl;
            float kl_frac = kl - (float)kl_floor;
            T[i1] = lsa[i1] - (float)0.65 * ((float)1 - kl_frac) * prev_mp.log2Ml[kl_floor] -
                    (float)0.65 * kl_frac * prev_mp.log2Ml[kl_floor + 1];
        }
    }

    /* DCT per block */
    {
        const int *J = dmr_enc_AmbeLmprbl[p->num_harms];
        float *c[4];
        int acc = 0;
        for (i = 0; i < 4; i++) {
            c[i] = &T[acc];
            acc += J[i];
        }
        for (i = 1; i <= 4; i++) {
            for (k = 1; k <= J[i - 1]; k++) {
                float s = 0.0f;
                for (j = 1; j <= J[i - 1]; j++)
                    s += c[i - 1][j - 1] * DCT_COS(J[i - 1], j, k);
                C[i - 1][k - 1] = s / (float)J[i - 1];
            }
        }
    }

    R[0] = C[0][0] + SQRT_2 * C[0][1];
    R[1] = C[0][0] - SQRT_2 * C[0][1];
    R[2] = C[1][0] + SQRT_2 * C[1][1];
    R[3] = C[1][0] - SQRT_2 * C[1][1];
    R[4] = C[2][0] + SQRT_2 * C[2][1];
    R[5] = C[2][0] - SQRT_2 * C[2][1];
    R[6] = C[3][0] + SQRT_2 * C[3][1];
    R[7] = C[3][0] - SQRT_2 * C[3][1];

    for (int mm = 1; mm <= 8; mm++) {
        G[mm - 1] = 0.0f;
        for (int ii = 1; ii <= 8; ii++)
            G[mm - 1] += R[ii - 1] * dmr_enc_cos_prba[mm][ii];
        G[mm - 1] /= 8.0f;
    }

    /* b[3]: PRBA24 */
    for (i = 0; i < 512; i++) {
        float err = 0.0f, diff;
        diff = G[1] - dmr_enc_AmbePRBA24[i][0]; err += diff * diff;
        diff = G[2] - dmr_enc_AmbePRBA24[i][1]; err += diff * diff;
        diff = G[3] - dmr_enc_AmbePRBA24[i][2]; err += diff * diff;
        if (i == 0 || err < error) {
            error = err;
            error_index = i;
        }
    }
    b[3] = error_index;

    /* b[4]: PRBA58 */
    for (i = 0; i < 128; i++) {
        float err = 0.0f, diff;
        diff = G[4] - dmr_enc_AmbePRBA58[i][0]; err += diff * diff;
        diff = G[5] - dmr_enc_AmbePRBA58[i][1]; err += diff * diff;
        diff = G[6] - dmr_enc_AmbePRBA58[i][2]; err += diff * diff;
        diff = G[7] - dmr_enc_AmbePRBA58[i][3]; err += diff * diff;
        if (i == 0 || err < error) {
            error = err;
            error_index = i;
        }
    }
    b[4] = error_index;

    /* b[5..8]: higher order coefficients, one search per block */
    {
        static const struct {
            int n;
            const float (*tbl)[4];
            int idx;
        } hoc[4] = {
            { 32, dmr_enc_AmbeHOCb5, 1 },
            { 16, dmr_enc_AmbeHOCb6, 2 },
            { 16, dmr_enc_AmbeHOCb7, 3 },
            { 8, dmr_enc_AmbeHOCb8, 4 },
        };
        const int *J = dmr_enc_AmbeLmprbl[p->num_harms];

        for (int h = 0; h < 4; h++) {
            int ii = hoc[h].idx;
            int nn = hoc[h].n;
            if (J[ii - 1] <= 2) {
                b[4 + ii] = 0;
                continue;
            }
            for (n = 0; n < nn; n++) {
                float err = 0.0f;
                for (j = 1; j <= J[ii - 1] - 2 && j <= 4; j++) {
                    float diff = hoc[h].tbl[n][j - 1] - C[ii - 1][j + 1];
                    err += diff * diff;
                }
                if (n == 0 || err < error) {
                    error = err;
                    error_index = n;
                }
            }
            b[4 + ii] = error_index;
        }
    }

    /* keep the predictor in sync with what was actually sent */
    {
        uint8_t bits49[49];
        pack_49(bits49, b);
        model_decode(bits49, &cur_mp, &prev_mp);
        prev_mp = cur_mp;
    }
    return;

silence:
    /* The reference bails out of the frame instead of transmitting something
     * it cannot index, and does *not* touch the predictor state. */
    b[0] = 40;
    for (i = 1; i < 9; i++)
        b[i] = 0;
}

/* ------------------------------------------------------------------------
 * b[0..8] -> the 9-byte frame, in the canonical A|B|C order.
 * ------------------------------------------------------------------------ */
static void pack_frame(const int b[9], uint8_t frame[9])
{
    uint8_t d[49];
    uint32_t c0 = 0, c1 = 0, c = 0;
    uint32_t a, bw;

    pack_49(d, b);

    for (int i = 0; i < 12; i++)
        c0 = (c0 << 1) | d[i];
    for (int i = 0; i < 12; i++)
        c1 = (c1 << 1) | d[12 + i];
    for (int i = 24; i < 49; i++)
        c = (c << 1) | d[i];

    a = golay_code24(c0);
    /* The B block is the 23-bit Golay codeword (which the reference keeps
     * shifted one place up) XOR the PRNG mask of C0, and the 23 bits that go
     * on the air are bits 22..0 of that word. */
    bw = (golay_code23(c1) << 1) ^ prng_mask(c0);

    /* bit 0 is the MSB of the frame */
    memset(frame, 0, 9);
    for (int i = 0; i < 24; i++)
        if ((a >> (23 - i)) & 1U)
            frame[i >> 3] |= (uint8_t)(0x80U >> (i & 7));
    for (int i = 0; i < 23; i++)
        if ((bw >> (22 - i)) & 1U)
            frame[(24 + i) >> 3] |= (uint8_t)(0x80U >> ((24 + i) & 7));
    for (int i = 0; i < 25; i++)
        if ((c >> (24 - i)) & 1U)
            frame[(47 + i) >> 3] |= (uint8_t)(0x80U >> ((47 + i) & 7));
}

void dmr_enc_frame_reset(void)
{
    /* the reference's mbe_initMbeParms(): L = 30, gamma = 0, no history */
    memset(&cur_mp, 0, sizeof(cur_mp));
    memset(&prev_mp, 0, sizeof(prev_mp));
    cur_mp.L = 30;
    prev_mp.L = 30;
}

#ifdef DMR_ENC_DEBUG
imbe_param_t dmr_enc_dbg_param;
int dmr_enc_dbg_b[9];

const float *dmr_enc_dbg_model(void)
{
    return prev_mp.log2Ml;
}
int dmr_enc_dbg_model_L(void)
{
    return prev_mp.L;
}
float dmr_enc_dbg_model_gamma(void)
{
    return prev_mp.gamma;
}
#endif

void dmr_enc_frame(imbe_param_t *param, uint8_t frame[9])
{
    int b[9] = {0};

    enc_ambe(param, b);
#ifdef DMR_ENC_DEBUG
    for (int i = 0; i < 9; i++)
        dmr_enc_dbg_b[i] = b[i];
#endif
    pack_frame(b, frame);
}

size_t dmr_enc_frame_state_bytes(void)
{
    return sizeof(cur_mp) + sizeof(prev_mp);
}
