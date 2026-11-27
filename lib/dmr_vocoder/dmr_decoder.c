/* dmr_decoder.c -- AMBE+2 (DMR 3600x2450) decoder core, pure C, no libm.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reimplementation of the mbelib-neo AMBE 2450 decoder path used by OpenDMR,
 * restructured for a Cortex-M33 class MCU:
 *
 *   - no C++, no thread-local storage, no malloc: one caller-owned state
 *     structure, every table in flash;
 *   - no libm: sin/cos/exp2/log2/sqrt come from dmr_math.h, the 256-point
 *     real FFT from dmr_fft.c, the DCT/HOC cosines from dmr_tables.h;
 *   - all arithmetic single precision (the reference silently promoted parts
 *     of the synthesis to double, which on ARMv8-M means __aeabi_d* calls);
 *   - the frame-to-frame copies only carry the fields that are actually read.
 *
 * The result is numerically the same as the reference (see test/compare.py):
 * the phase reduction is exact for the huge phase accumulators the model
 * produces, so no "phase wrap" approximation is used.
 */
#include "dmr_decoder.h"

#include <string.h>

#include "dmr_fft.h"
#include "dmr_math.h"
#include "dmr_tables.h"

#define N_SAMPLES 160

/* AMBE+2 constants (matching mbelib-neo) */
#define MBE_UNVOICED_SCALE_COEFF 146.17696f
#define MBE_256_OVER_2PI (256.0f / 6.28318530717958647692f)
#define MBE_WHITE_NOISE_SCALAR (6.28318530717958647692f / 53125.0f)
#define MBE_LCG_DEFAULT_SEED 3147.0f
#define MBE_MUTING_THRESHOLD_AMBE 0.096f
#define MBE_MAX_FRAME_REPEATS 4
#define DMR_INIT_W0 0.09378f
#define DMR_INIT_L 30

/* ------------------------------------------------------------------ */
/* Golay (23,12) / (24,12) FEC                                        */
/* ------------------------------------------------------------------ */

#define GOLAY_GENPOL 0x00C75u

/* Remainder of a 23-bit vector modulo g(x); ~23 iterations of shift/xor. */
static inline uint32_t golay_syndrome(uint32_t pattern)
{
    uint32_t rem = pattern & 0x7FFFFFu;
    for (int i = 22; i >= 11; i--) {
        if (rem & (1u << i))
            rem ^= GOLAY_GENPOL << (i - 11);
    }
    return rem & 0x7FFu;
}

/* Correct a (23,12) codeword and return its 12 information bits.  The code is
 * perfect (sphere-packing radius 3), so the syndrome table is a bijection
 * between the 2048 syndromes and the 2048 patterns of weight <= 3: the leader
 * is unique and matches the reference's table-driven decoder. */
static inline uint32_t golay_decode23(uint32_t code)
{
    uint32_t syn = golay_syndrome(code);
    return (code ^ (uint32_t)dmr_golay_hi[syn]) >> 11;
}

/* ------------------------------------------------------------------ */
/* Frame unpacking                                                    */
/* ------------------------------------------------------------------ */

static uint32_t get_bits(const uint8_t *f, int first, int count)
{
    uint32_t v = 0;
    for (int i = 0; i < count; i++) {
        int p = first + i;
        v = (v << 1) | ((f[p >> 3] >> (7 - (p & 7))) & 1u);
    }
    return v;
}

/* Popcount of a <=23-bit value (avoids the libgcc __popcountsi2 call). */
static inline int popcount23(uint32_t x)
{
    int n = 0;
    while (x) {
        x &= x - 1;
        n++;
    }
    return n;
}

/* 72-bit canonical frame -> 49 AMBE parameter bits (MSB first).
 * Returns the number of FEC-corrected bits (informational). */
static int unpack_frame(const uint8_t frame[DMR_FRAME_BYTES], uint8_t d[49], int want_errs)
{
    uint32_t a = get_bits(frame, 0, 24);
    uint32_t b = get_bits(frame, 24, 23);
    uint32_t c = get_bits(frame, 47, 25);

    uint32_t a23 = a >> 1; /* (24,12): drop the parity bit */
    uint32_t e0 = (uint32_t)dmr_golay_hi[golay_syndrome(a23)];
    uint32_t c0 = (a23 ^ e0) >> 11;

    /* B is scrambled with a PRNG seeded by C0 */
    uint16_t pr = (uint16_t)(16u * c0);
    uint32_t mask = 0;
    for (int i = 1; i <= 23; i++) {
        pr = (uint16_t)((173u * pr + 13849u) & 0xFFFFu);
        if (pr >> 15)
            mask |= 1u << (23 - i);
    }
    uint32_t b23 = b ^ mask;
    uint32_t e1 = (uint32_t)dmr_golay_hi[golay_syndrome(b23)];
    uint32_t c1 = (b23 ^ e1) >> 11;

    for (int i = 0; i < 12; i++) {
        d[i] = (uint8_t)((c0 >> (11 - i)) & 1u);
        d[12 + i] = (uint8_t)((c1 >> (11 - i)) & 1u);
    }
    for (int i = 0; i < 25; i++)
        d[24 + i] = (uint8_t)((c >> (24 - i)) & 1u);

    if (!want_errs)
        return 0;
    return popcount23(e0) + popcount23(e1);
}

/* ------------------------------------------------------------------ */
/* Parameter state helpers                                            */
/* ------------------------------------------------------------------ */

static void parms_init(dmr_parms_t *p)
{
    p->w0 = DMR_INIT_W0;
    p->L = DMR_INIT_L;
    p->gamma = 0.0f;
    p->swn = 0;
    p->repeat = 0;
    p->repeatCount = 0;
    p->amplitudeThreshold = 20480;
    p->errorCountTotal = 0;
    p->errorCount4 = 0;
    p->errorRate = 0.0f;
    p->localEnergy = 75000.0f;
    p->mutingThreshold = 0.0875f;
    for (int l = 0; l <= DMR_MAX_BANDS + 1; l++) {
        p->Ml[l] = 0.0f;
        p->Vl[l] = 0;
        p->log2Ml[l] = 0.0f;
        p->PHIl[l] = 0.0f;
        p->PSIl[l] = DMR_PI_2;
    }
}

/* ------------------------------------------------------------------ */
/* AMBE 2450 parameter decode (mbe_decodeAmbe2450Parms)               */
/* ------------------------------------------------------------------ */

static int decode_parms_values(const uint8_t *d, dmr_parms_t *cur, dmr_parms_t *prev)
{
    int ji, i, j, k, l, L = 0, m, am, ak;
    int intkl[57];
    int b0, b1, b2, b3, b4, b5, b6, b7, b8;
    float f0, Cik[5][18], flokl[57], deltal[57];
    float Sum42, Sum43, Tl[57] = {0}, Gm[9], Ri[9], sum, c1, c2;
    int silence = 0;
    int Ji[5], jl;
    float deltaGamma, BigGamma;
    float unvc, rconst;

    int u0 = 0, u3 = 0;
    for (i = 0; i < 12; i++)
        u0 = (u0 << 1) | d[i];
    for (i = 35; i < 49; i++)
        u3 = (u3 << 1) | d[i];

    int bitchk1 = (u0 >> 6) & 0x3F;
    int bitchk2 = u3 & 0xF;

    cur->repeat = prev->repeat;

    b0 = (d[0] << 6) | (d[1] << 5) | (d[2] << 4) | (d[3] << 3) | (d[37] << 2) | (d[38] << 1) | d[39];

    if (bitchk1 == 63 && bitchk2 == 0)
        return 7; /* tone frame */
    if (b0 >= 120 && b0 <= 123)
        return 2; /* erasure */
    if (b0 == 124 || b0 == 125) {
        cur->w0 = 0.19634954084936207f; /* 2*pi/32 */
        f0 = 1.0f / 32.0f;
        L = 14;
        cur->L = 14;
        for (l = 1; l <= L; l++)
            cur->Vl[l] = 0;
        silence = 1;
    }
    if (b0 == 126 || b0 == 127)
        return 3; /* tone frame */

    if (silence == 0) {
        f0 = dmr_w0[b0];
        cur->w0 = dmr_w0_rad[b0];
    }

    unvc = 0.2046f / dmr_sqrtf(cur->w0);

    if (silence == 0) {
        L = dmr_l_table[b0];
        cur->L = L;
    }

    /* V/UV decisions */
    b1 = (d[4] << 4) | (d[5] << 3) | (d[6] << 2) | (d[7] << 1) | d[35];
    if (silence == 0) {
        for (l = 1; l <= L; l++) {
            jl = (int)((float)l * 16.0f * f0);
            /*
             * dmr_vuv is 8 columns wide (dmr_tables.h) and the generated tables
             * reach exactly 7 today, with no margin: the index is an invariant
             * of tools/gen_tables.py, not of this code.  mbelib, where this
             * comes from, clamps here; without the clamp a regenerated table
             * -- or another f0 on the silence path -- reads up to 15 rows past
             * the codebook and the V/UV decisions come out of the next
             * constant, which is noise in the received speech and a fault that
             * cannot be attributed.
             */
            if (jl < 0)
                jl = 0;
            else if (jl > 7)
                jl = 7;
            cur->Vl[l] = (uint8_t)dmr_vuv[b1][jl];
        }
    }

    /* gain */
    b2 = (d[8] << 4) | (d[9] << 3) | (d[10] << 2) | (d[11] << 1) | d[36];
    deltaGamma = dmr_dg[b2];
    cur->gamma = deltaGamma + 0.5f * prev->gamma;

    /* PRBA -> Gm[2..8], Gm[1] = 0 */
    Gm[1] = 0.0f;
    b3 = (d[12] << 8) | (d[13] << 7) | (d[14] << 6) | (d[15] << 5) | (d[16] << 4) | (d[17] << 3) |
         (d[18] << 2) | (d[19] << 1) | d[40];
    Gm[2] = dmr_prba24[b3][0];
    Gm[3] = dmr_prba24[b3][1];
    Gm[4] = dmr_prba24[b3][2];
    b4 = (d[20] << 6) | (d[21] << 5) | (d[22] << 4) | (d[23] << 3) | (d[41] << 2) | (d[42] << 1) | d[43];
    Gm[5] = dmr_prba58[b4][0];
    Gm[6] = dmr_prba58[b4][1];
    Gm[7] = dmr_prba58[b4][2];
    Gm[8] = dmr_prba58[b4][3];

    /* Ri = IDCT of the Gm block (ROM cosines) */
    for (i = 1; i <= 8; i++) {
        sum = 0.0f;
        for (m = 1; m <= 8; m++) {
            am = (m == 1) ? 1 : 2;
            sum = sum + ((float)am * Gm[m] * dmr_ri_cos[m][i]);
        }
        Ri[i] = sum;
    }

    rconst = 0.35355339059327373f; /* 1/(2*sqrt(2)) */
    Cik[1][1] = 0.5f * (Ri[1] + Ri[2]);
    Cik[1][2] = rconst * (Ri[1] - Ri[2]);
    Cik[2][1] = 0.5f * (Ri[3] + Ri[4]);
    Cik[2][2] = rconst * (Ri[3] - Ri[4]);
    Cik[3][1] = 0.5f * (Ri[5] + Ri[6]);
    Cik[3][2] = rconst * (Ri[5] - Ri[6]);
    Cik[4][1] = 0.5f * (Ri[7] + Ri[8]);
    Cik[4][2] = rconst * (Ri[7] - Ri[8]);

    /* higher order coefficients */
    b5 = (d[24] << 4) | (d[25] << 3) | (d[26] << 2) | (d[27] << 1) | d[44];
    b6 = (d[28] << 3) | (d[29] << 2) | (d[30] << 1) | d[45];
    b7 = (d[31] << 3) | (d[32] << 2) | (d[33] << 1) | d[46];
    b8 = (d[34] << 2) | (d[47] << 1) | d[48];

    Ji[1] = dmr_lmprbl[L][0];
    Ji[2] = dmr_lmprbl[L][1];
    Ji[3] = dmr_lmprbl[L][2];
    Ji[4] = dmr_lmprbl[L][3];

    for (k = 3; k <= Ji[1]; k++)
        Cik[1][k] = (k > 6) ? 0.0f : dmr_hoc_b5[b5][k - 3];
    for (k = 3; k <= Ji[2]; k++)
        Cik[2][k] = (k > 6) ? 0.0f : dmr_hoc_b6[b6][k - 3];
    for (k = 3; k <= Ji[3]; k++)
        Cik[3][k] = (k > 6) ? 0.0f : dmr_hoc_b7[b7][k - 3];
    for (k = 3; k <= Ji[4]; k++)
        Cik[4][k] = (k > 6) ? 0.0f : dmr_hoc_b8[b8][k - 3];

    /* per-block inverse DCT -> Tl[] */
    l = 1;
    for (i = 1; i <= 4; i++) {
        ji = Ji[i];
        const float *tab = &dmr_idct_cos[dmr_idct_off[ji]];
        for (j = 1; j <= ji; j++) {
            const float *row = tab + (j - 1) * ji;
            sum = 0.0f;
            for (k = 1; k <= ji; k++) {
                ak = (k == 1) ? 1 : 2;
                sum = sum + ((float)ak * Cik[i][k] * row[k - 1]);
            }
            Tl[l] = sum;
            l++;
        }
    }

    /* prediction of the spectral magnitudes */
    if (cur->L > prev->L) {
        for (l = (prev->L) + 1; l <= cur->L; l++) {
            prev->Ml[l] = prev->Ml[prev->L];
            prev->log2Ml[l] = prev->log2Ml[prev->L];
        }
    }
    prev->log2Ml[0] = prev->log2Ml[1];
    prev->Ml[0] = prev->Ml[1];

    Sum43 = 0.0f;
    for (l = 1; l <= cur->L; l++) {
        flokl[l] = ((float)prev->L / (float)cur->L) * (float)l;
        intkl[l] = (int)(flokl[l]);
        deltal[l] = flokl[l] - (float)intkl[l];
        Sum43 = Sum43 + (((1.0f - deltal[l]) * prev->log2Ml[intkl[l]]) +
                         (deltal[l] * prev->log2Ml[intkl[l] + 1]));
    }
    Sum43 = (0.65f / (float)cur->L) * Sum43;

    Sum42 = 0.0f;
    for (l = 1; l <= cur->L; l++)
        Sum42 += Tl[l];
    Sum42 = Sum42 / (float)cur->L;
    BigGamma = cur->gamma - (0.5f * dmr_log2_int[cur->L]) - Sum42;

    for (l = 1; l <= cur->L; l++) {
        c1 = 0.65f * (1.0f - deltal[l]) * prev->log2Ml[intkl[l]];
        c2 = 0.65f * deltal[l] * prev->log2Ml[intkl[l] + 1];
        cur->log2Ml[l] = Tl[l] + c1 + c2 - Sum43 + BigGamma;
        if (cur->Vl[l] == 1)
            cur->Ml[l] = dmr_exp2f(cur->log2Ml[l]);
        else
            cur->Ml[l] = unvc * dmr_exp2f(cur->log2Ml[l]);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Spectral amplitude enhancement (mbe_spectralAmpEnhance)            */
/* ------------------------------------------------------------------ */

static void spectral_amp_enhance(dmr_parms_t *cur, float *cos_tab)
{
    const int L = cur->L;
    float Rm0 = 0.0f, Rm1 = 0.0f, R2m0, R2m1, sum, gamma;
    float s_step, c_step;

    dmr_sincosf(cur->w0, &s_step, &c_step);
    float c = 1.0f, s = 0.0f;
    for (int l = 1; l <= L; ++l) {
        float cn = (c * c_step) - (s * s_step);
        float sn = (s * c_step) + (c * s_step);
        c = cn;
        s = sn;
        cos_tab[l] = c;
    }

    for (int l = 1; l <= L; l++) {
        const float m2 = cur->Ml[l] * cur->Ml[l];
        Rm0 += m2;
        Rm1 += m2 * cos_tab[l];
    }
    R2m0 = Rm0 * Rm0;
    R2m1 = Rm1 * Rm1;

    const float k96pi = 0.96f * DMR_PI;
    for (int l = 1; l <= L; l++) {
        if (cur->Ml[l] != 0.0f) {
            float Wl = dmr_sqrtf(cur->Ml[l]) *
                       dmr_sqrtf(dmr_sqrtf((k96pi * ((R2m0 + R2m1) - (2.0f * Rm0 * Rm1 * cos_tab[l]))) /
                                           (cur->w0 * Rm0 * (R2m0 - R2m1))));
            if ((8 * l) <= L) {
                /* no-op for low harmonics */
            } else if (Wl > 1.2f) {
                cur->Ml[l] = 1.2f * cur->Ml[l];
            } else if (Wl < 0.5f) {
                cur->Ml[l] = 0.5f * cur->Ml[l];
            } else {
                cur->Ml[l] = Wl * cur->Ml[l];
            }
        }
    }

    sum = 0.0f;
    for (int l = 1; l <= L; l++) {
        float M = cur->Ml[l];
        if (M < 0.0f)
            M = -M;
        sum += (M * M);
    }
    if (sum == 0.0f)
        gamma = 1.0f;
    else
        gamma = dmr_sqrtf(Rm0 / sum);
    for (int l = 1; l <= L; l++)
        cur->Ml[l] = gamma * cur->Ml[l];
}

/* ------------------------------------------------------------------ */
/* Adaptive smoothing (JMBE #111-116)                                 */
/* ------------------------------------------------------------------ */

#define MBE_ERROR_THRESHOLD_ENTRY 0.0125f
#define MBE_ERROR_THRESHOLD_LOW 0.005f
#define MBE_DEFAULT_LOCAL_ENERGY 75000.0f
#define MBE_MIN_LOCAL_ENERGY 10000.0f
#define MBE_DEFAULT_AMPLITUDE_THRESHOLD 20480
#define MBE_ADAPTIVE_GAIN 45.255f
#define MBE_ADAPTIVE_EXPONENT 277.26f
#define MBE_ADAPTIVE_ALT 1.414f
#define MBE_AMPLITUDE_PENALTY_PER_ERROR 300
#define MBE_AMPLITUDE_BASE 6000

static void apply_adaptive_smoothing(dmr_parms_t *cur, const dmr_parms_t *prev)
{
    const int L = cur->L;
    float *M = cur->Ml;
    uint8_t *V = cur->Vl;
    const float errorRate = cur->errorRate;
    const int errorTotal = cur->errorCountTotal;

    float RM0 = 0.0f;
    for (int l = 1; l <= L; l++)
        RM0 += M[l] * M[l];

    float prevEnergy = prev->localEnergy;
    if (prevEnergy < MBE_MIN_LOCAL_ENERGY)
        prevEnergy = MBE_DEFAULT_LOCAL_ENERGY;
    cur->localEnergy = 0.95f * prevEnergy + 0.05f * RM0;
    if (cur->localEnergy < MBE_MIN_LOCAL_ENERGY)
        cur->localEnergy = MBE_MIN_LOCAL_ENERGY;

    /* clean channel: nothing to smooth (this is the normal path) */
    if (errorRate <= MBE_ERROR_THRESHOLD_ENTRY && errorTotal <= 4) {
        cur->amplitudeThreshold = MBE_DEFAULT_AMPLITUDE_THRESHOLD;
        return;
    }

    float VM;
    if (errorRate <= MBE_ERROR_THRESHOLD_LOW && errorTotal <= 4) {
        VM = 3.40282347e+38f; /* FLT_MAX */
    } else {
        float x8 = dmr_sqrtf(dmr_sqrtf(dmr_sqrtf(cur->localEnergy)));
        float energy = x8 * x8 * x8;
        if (errorRate <= MBE_ERROR_THRESHOLD_ENTRY && cur->errorCount4 == 0)
            VM = (MBE_ADAPTIVE_GAIN * energy) / dmr_expf(MBE_ADAPTIVE_EXPONENT * errorRate);
        else
            VM = MBE_ADAPTIVE_ALT * energy;
    }
    for (int l = 1; l <= L; l++)
        if (M[l] > VM)
            V[l] = 1;

    float Am = 0.0f;
    for (int l = 1; l <= L; l++)
        Am += M[l];

    int Tm;
    int prevThreshold = prev->amplitudeThreshold;
    if (prevThreshold <= 0)
        prevThreshold = MBE_DEFAULT_AMPLITUDE_THRESHOLD;
    if (errorRate <= MBE_ERROR_THRESHOLD_LOW && errorTotal <= 6) {
        Tm = MBE_DEFAULT_AMPLITUDE_THRESHOLD;
    } else {
        Tm = MBE_AMPLITUDE_BASE - (MBE_AMPLITUDE_PENALTY_PER_ERROR * errorTotal) + prevThreshold;
        if (Tm < 0)
            Tm = 0;
    }
    cur->amplitudeThreshold = (int16_t)Tm;
    if (Am > (float)Tm && Am > 0.0f) {
        float scale = (float)Tm / Am;
        for (int l = 1; l <= L; l++)
            M[l] *= scale;
    }
}

/* ------------------------------------------------------------------ */
/* Noise, comfort noise, tone, silence                                */
/* ------------------------------------------------------------------ */

static void gen_noise_lcg(float *buf, int count, float *seed)
{
    uint32_t state = (uint32_t)(*seed) % 53125u;
    for (int i = 0; i < count; i++) {
        buf[i] = (float)state;
        state = (171u * state + 11213u) % 53125u;
    }
    *seed = (float)state;
}

static void gen_noise_overlap(float *buf, float *seed, float *overlap)
{
    memcpy(buf, overlap, 96 * sizeof(float));
    gen_noise_lcg(buf + 96, 160, seed);
    memcpy(overlap, buf + 160, 96 * sizeof(float));
}

static void synth_comfort_noise(dmr_vocoder_t *v, float *out)
{
    const float gain = 0.003f * 32767.0f;
    for (int i = 0; i < N_SAMPLES; i += 2) {
        uint32_t x = v->cn_seed;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v->cn_seed = x ? x : 0x6d25357bu;
        float u1 = ((float)(v->cn_seed & 0xFFFFFFu) + 1.0f) / 16777217.0f;
        x = v->cn_seed;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v->cn_seed = x ? x : 0x6d25357bu;
        float u2 = (float)(v->cn_seed & 0xFFFFFFu) / 16777216.0f;
        float r = dmr_sqrtf(-2.0f * dmr_logf(u1));
        float theta = DMR_TWO_PI * u2;
        float s, c;
        dmr_sincosf(theta, &s, &c);
        out[i] = (r * c) * gain;
        if (i + 1 < N_SAMPLES)
            out[i + 1] = (r * s) * gain;
    }
}

static void synth_tone(float *out, const uint8_t *d, dmr_parms_t *cur)
{
    int u0 = 0, u1 = 0, u2 = 0, u3 = 0;
    for (int i = 0; i < 12; i++)
        u0 = (u0 << 1) | d[i];
    for (int i = 12; i < 24; i++)
        u1 = (u1 << 1) | d[i];
    for (int i = 24; i < 35; i++)
        u2 = (u2 << 1) | d[i];
    for (int i = 35; i < 49; i++)
        u3 = (u3 << 1) | d[i];

    int AD = ((u0 & 0x3F) << 1) + ((u3 >> 4) & 1);
    int ID1 = (u1 & 0xFFF) >> 4;
    float freq1 = 0.0f, freq2 = 0.0f;

    switch (ID1) {
    case 5: freq1 = 156.25f; freq2 = freq1; break;
    case 6: freq1 = 187.5f; freq2 = freq1; break;
    case 128: freq1 = 1336; freq2 = 941; break;
    case 129: freq1 = 1209; freq2 = 697; break;
    case 130: freq1 = 1336; freq2 = 697; break;
    case 131: freq1 = 1477; freq2 = 697; break;
    case 132: freq1 = 1209; freq2 = 770; break;
    case 133: freq1 = 1336; freq2 = 770; break;
    case 134: freq1 = 1477; freq2 = 770; break;
    case 135: freq1 = 1209; freq2 = 852; break;
    case 136: freq1 = 1336; freq2 = 852; break;
    case 137: freq1 = 1477; freq2 = 852; break;
    case 138: freq1 = 1633; freq2 = 697; break;
    case 139: freq1 = 1633; freq2 = 770; break;
    case 140: freq1 = 1633; freq2 = 852; break;
    case 141: freq1 = 1633; freq2 = 941; break;
    case 142: freq1 = 1209; freq2 = 941; break;
    case 143: freq1 = 1477; freq2 = 941; break;
    case 144: freq1 = 1162; freq2 = 820; break;
    case 145: freq1 = 1052; freq2 = 606; break;
    case 146: freq1 = 1162; freq2 = 606; break;
    case 147: freq1 = 1279; freq2 = 606; break;
    case 148: freq1 = 1052; freq2 = 672; break;
    case 149: freq1 = 1162; freq2 = 672; break;
    case 150: freq1 = 1279; freq2 = 672; break;
    case 151: freq1 = 1052; freq2 = 743; break;
    case 152: freq1 = 1162; freq2 = 743; break;
    case 153: freq1 = 1279; freq2 = 743; break;
    case 154: freq1 = 1430; freq2 = 606; break;
    case 155: freq1 = 1430; freq2 = 672; break;
    case 156: freq1 = 1430; freq2 = 743; break;
    case 157: freq1 = 1430; freq2 = 820; break;
    case 158: freq1 = 1052; freq2 = 820; break;
    case 159: freq1 = 1279; freq2 = 820; break;
    case 160: freq1 = 440; freq2 = 350; break;
    case 161: freq1 = 480; freq2 = 440; break;
    case 162: freq1 = 620; freq2 = 480; break;
    case 163: freq1 = 490; freq2 = 350; break;
    case 255: freq1 = 0; freq2 = 0; break;
    default:
        if (ID1 >= 7 && ID1 <= 122) {
            freq1 = 31.25f * (float)ID1;
            freq2 = freq1;
        }
        break;
    }

    if (freq1 == 0.0f && freq2 == 0.0f) {
        memset(out, 0, N_SAMPLES * sizeof(float));
        return;
    }
    const float step1 = DMR_TWO_PI * freq1 / 8000.0f;
    const float step2 = DMR_TWO_PI * freq2 / 8000.0f;
    const float amplitude = (float)AD * 75.0f;
    for (int n = 0; n < N_SAMPLES; n++) {
        float ph = (float)cur->swn;
        out[n] = amplitude * (dmr_sinf(ph * step1) / 2.0f + dmr_sinf(ph * step2) / 2.0f);
        out[n] = out[n] / 6.0f;
        cur->swn++;
    }
}

/* ------------------------------------------------------------------ */
/* Voiced synthesis                                                   */
/* ------------------------------------------------------------------ */

/* Add 160 samples of a windowed cosine oscillator.  The oscillator is
 * advanced with a complex rotation, four samples per iteration, exactly like
 * the reference. */
static void add_voiced(float *Ss, const float *W, float amp, float phi, float step)
{
    float sd, cd;
    dmr_sincosf(step, &sd, &cd);
    float s, c;
    dmr_sincosf(phi, &s, &c);
    /* 2*amp is exact (power of two), so folding it here does not change the
     * product; the accumulate is an explicit FMA, which is safe here because
     * the result is a plain sum with nothing decided from it. */
    const float wa = 2.0f * amp;
    for (int n = 0; n < N_SAMPLES; n += 4) {
        float c0 = c;
        float cn = (c * cd) - (s * sd);
        float sn = (s * cd) + (c * sd);
        float c1 = cn;
        cn = (cn * cd) - (sn * sd);
        sn = (sn * cd) + (c1 * sd);
        float c2 = cn;
        cn = (cn * cd) - (sn * sd);
        sn = (sn * cd) + (c2 * sd);
        float c3 = cn;
        c = (cn * cd) - (sn * sd);
        s = (sn * cd) + (c3 * sd);

        Ss[n + 0] = __builtin_fmaf(W[n + 0] * wa, c0, Ss[n + 0]);
        Ss[n + 1] = __builtin_fmaf(W[n + 1] * wa, c1, Ss[n + 1]);
        Ss[n + 2] = __builtin_fmaf(W[n + 2] * wa, c2, Ss[n + 2]);
        Ss[n + 3] = __builtin_fmaf(W[n + 3] * wa, c3, Ss[n + 3]);
    }
}

/* ------------------------------------------------------------------ */
/* Unvoiced synthesis (FFT + WOLA, JMBE #117-126)                     */
/* ------------------------------------------------------------------ */

static void synth_unvoiced(dmr_vocoder_t *v, float *out, dmr_parms_t *cur)
{
    const int L = cur->L;
    const float w0 = cur->w0;

    for (int i = 0; i < DMR_FFT_N; i++)
        v->uw[i] = v->noise[i] * dmr_win256[i];
    dmr_rfft256(v->uw, v->fft_re, v->fft_im);

    const float multiplier = dmr_nofma(MBE_256_OVER_2PI * w0); /* band edges are
                                                               * decided by a ceil() */
    for (int l = 1; l <= L; l++) {
        int a = dmr_iceil((l - 0.5f) * multiplier);
        int b = dmr_iceil((l + 0.5f) * multiplier);
        if (a < 0)
            a = 0;
        if (b > DMR_FFT_N / 2)
            b = DMR_FFT_N / 2;
        v->a_min[l] = (int16_t)a;
        v->b_max[l] = (int16_t)b;
    }

    memset(v->bin_scale, 0, sizeof(v->bin_scale));
    for (int l = 1; l <= L; l++) {
        if (cur->Vl[l] == 0) {
            const int a = v->a_min[l], b = v->b_max[l];
            const int nb = b - a;
            float num = 0.0f;
            for (int bin = a; bin < b; bin++)
                num += (v->fft_re[bin] * v->fft_re[bin]) + (v->fft_im[bin] * v->fft_im[bin]);
            if (nb > 0 && num > 1e-10f) {
                float scalor = MBE_UNVOICED_SCALE_COEFF * cur->Ml[l] / dmr_sqrtf(num / (float)nb);
                for (int bin = a; bin < b; bin++)
                    v->bin_scale[bin] = scalor;
            }
        }
    }
    for (int bin = 0; bin <= DMR_FFT_N / 2; bin++) {
        v->fft_re[bin] *= v->bin_scale[bin];
        v->fft_im[bin] *= v->bin_scale[bin];
    }

    dmr_irfft256(v->fft_re, v->fft_im, v->uw);
    const float sc = 1.0f / (float)DMR_FFT_N;
    for (int i = 0; i < DMR_FFT_N; i++)
        v->uw[i] *= sc;

    /* WOLA against the previous frame */
    const float *puw = v->last_uw;
    const float *cuw = v->uw;
    for (int n = 0; n < N_SAMPLES; n++) {
        float ps = (n < 128) ? puw[n + 128] : 0.0f;
        float cs = (n >= 32) ? cuw[n - 32] : 0.0f;
        out[n] += (dmr_wola_wprev[n] * ps + dmr_wola_wcurr[n] * cs) * dmr_wola_inv[n];
    }
    memcpy(v->last_uw, v->uw, sizeof(v->last_uw));
}

/* ------------------------------------------------------------------ */
/* Speech synthesis                                                   */
/* ------------------------------------------------------------------ */

static void synth_speech(dmr_vocoder_t *v, float *aout, dmr_parms_t *cur, dmr_parms_t *prev)
{
    int l, n, maxl, numUv;
    float cw0, pw0, cw0l, pw0l;
    const int N = N_SAMPLES;

    if (cur->repeatCount >= MBE_MAX_FRAME_REPEATS || cur->errorRate > cur->mutingThreshold) {
        synth_comfort_noise(v, aout);
        *cur = *prev;
        cur->repeatCount = 0;
        return;
    }

    gen_noise_overlap(v->noise, &v->noise_seed, v->noise_overlap);

    numUv = 0;
    for (l = 1; l <= cur->L; l++)
        if (cur->Vl[l] == 0)
            numUv++;

    cw0 = cur->w0;
    pw0 = prev->w0;

    memset(aout, 0, N * sizeof(float));

    apply_adaptive_smoothing(cur, prev);

    if (cur->L > prev->L) {
        maxl = cur->L;
        for (l = prev->L + 1; l <= maxl; l++) {
            prev->Ml[l] = 0.0f;
            prev->Vl[l] = 1;
        }
    } else {
        maxl = prev->L;
        for (l = cur->L + 1; l <= maxl; l++) {
            cur->Ml[l] = 0.0f;
            cur->Vl[l] = 1;
        }
    }

    for (l = 1; l <= DMR_MAX_BANDS; l++) {
        /* the product must be rounded before the accumulation (see dmr_nofma) */
        float inc = (pw0 + cw0) * ((float)(l * N) / 2.0f);
        float psi = prev->PSIl[l] + dmr_nofma(inc);
        /* The model's phase accumulator is unbounded (~1 krad per frame), while
         * cos() is only exact below 2^22.  Subtract a whole number of periods
         * whenever it leaves +-2^21: the phase is unchanged, and after ~90 s of
         * continuous audio the float accumulator has no more resolution to lose
         * anyway.  Inside the test vectors (1000 frames) this never triggers. */
        if (psi > 2097152.0f || psi < -2097152.0f)
            psi -= DMR_TWO_PI * (float)dmr_ifloor(psi * 0.15915494309189535f + 0.5f);
        cur->PSIl[l] = psi;
        if (l <= (cur->L / 4)) {
            cur->PHIl[l] = cur->PSIl[l];
        } else {
            float pl = (MBE_WHITE_NOISE_SCALAR * v->noise[l]) - DMR_PI;
            cur->PHIl[l] = cur->PSIl[l] + (((float)numUv * pl) / (float)cur->L);
        }
    }

    for (l = 1; l <= maxl; l++) {
        cw0l = cw0 * (float)l;
        pw0l = pw0 * (float)l;
        int cur_voiced = (cur->Vl[l] == 1);
        int prev_voiced = (prev->Vl[l] == 1);
        if (!(cur_voiced || prev_voiced))
            continue;

        int use_interp = (l < 8) && cur_voiced && prev_voiced &&
                         (dmr_fabsf(cw0 - pw0) < (0.1f * cw0));
        if (use_interp) {
            float deltaphil = cur->PHIl[l] - prev->PHIl[l] - (((pw0 + cw0) * (float)(l * N)) / 2.0f);
            float deltawl = (1.0f / (float)N) *
                            (deltaphil - (DMR_TWO_PI * (float)dmr_ifloor((deltaphil + DMR_PI) / DMR_TWO_PI)));
            float phi0 = prev->PHIl[l];
            float ampd = cur->Ml[l] - prev->Ml[l];
            float mprev = prev->Ml[l];
            float wstep = pw0l + deltawl;
            float quad = (cw0 - pw0) * (float)l / (float)(2 * N);
            for (n = 0; n < N; n++) {
                float thetaln = phi0 + (wstep * (float)n) + (quad * (float)(n * n));
                float aln = mprev + ((float)n / (float)N) * ampd;
                aout[n] = __builtin_fmaf(2.0f * aln, dmr_cosf(thetaln), aout[n]);
            }
        } else {
            if (prev_voiced)
                add_voiced(aout, dmr_ws_prev, prev->Ml[l], prev->PHIl[l], pw0l);
            if (cur_voiced)
                add_voiced(aout, dmr_ws_cur, cur->Ml[l], cur->PHIl[l] - (cw0l * (float)N), cw0l);
        }
    }

    synth_unvoiced(v, aout, cur);
}

/* ------------------------------------------------------------------ */
/* PCM output helpers (ARMv8-M DSP where available)                   */
/* ------------------------------------------------------------------ */

/* Signed saturate to 16 bits: SSAT on Cortex-M33, plain clamp elsewhere.
 * The value is already clipped to +-31128 so SSAT is a no-op numerically; it
 * keeps the conversion overflow-proof for free. */
static inline int16_t sat16(int32_t x)
{
#if defined(__arm__)
    int32_t r;
    __asm__("ssat %0, #16, %1" : "=r"(r) : "r"(x));
    return (int16_t)r;
#else
    if (x > 32767)
        x = 32767;
    else if (x < -32768)
        x = -32768;
    return (int16_t)x;
#endif
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

static void vocoder_reset(dmr_vocoder_t *v)
{
    parms_init(&v->prev);
    v->cur = v->prev;
    v->enh = v->prev;
    memset(v->last_uw, 0, sizeof(v->last_uw));
    memset(v->noise_overlap, 0, sizeof(v->noise_overlap));
    v->noise_seed = MBE_LCG_DEFAULT_SEED;
}

void dmr_vocoder_init(dmr_vocoder_t *v)
{
    if (v) {
        vocoder_reset(v);
        v->cn_seed = 0x12345678u;
    }
}

size_t dmr_vocoder_state_size(void) { return sizeof(dmr_vocoder_t); }

int dmr_golay24_correct(uint32_t w24, uint32_t *info12)
{
    /* Extended Golay(24,12): 12 information bits, 11 parity, one overall
     * parity bit last.  The (23,12) core is perfect, so the syndrome table
     * gives the error pattern directly; the parity bit then says whether the
     * correction is inside the three-bit radius. */
    uint32_t a23 = (w24 >> 1) & 0x7FFFFFu;
    uint32_t e = (uint32_t)dmr_golay_hi[golay_syndrome(a23)];
    uint32_t corrected = a23 ^ e;
    int weight = popcount23(e) + (((popcount23(corrected) & 1) != (int)(w24 & 1u)) ? 1 : 0);

    if (weight > 3)
        return -1;

    if (info12)
        *info12 = corrected >> 11;

    return weight;
}

/**
 * The part of a frame decode that does not depend on the channel format: the
 * 49 AMBE parameters in, 20 ms of PCM out.
 */
static void decode_params(dmr_vocoder_t *v, const uint8_t d[49],
                          int16_t pcm[DMR_PCM_SAMPLES], int nerr, int *errs)
{
    dmr_parms_t *cur = &v->cur;
    dmr_parms_t *prev = &v->prev;
    dmr_parms_t *enh = &v->enh;
    float fbuf[N_SAMPLES];

    cur->mutingThreshold = MBE_MUTING_THRESHOLD_AMBE;
    /* The reference wrapper feeds zero FEC counters into the mbe layer, so
     * errorRate is only decayed.  Keep that behaviour to stay bit-exact and
     * report the corrected-bit count to the caller instead. */
    if (errs)
        *errs = nerr;
    cur->errorCountTotal = 0;
    cur->errorCount4 = 0;
    cur->errorRate = 0.95f * prev->errorRate;

    int bad = decode_parms_values(d, cur, prev);
    cur->repeat = 0;
    cur->repeatCount = 0;

    if (bad == 0) {
        if (cur->repeat <= 3) {
            *prev = *cur;
            spectral_amp_enhance(cur, v->cos_tab);
            synth_speech(v, fbuf, cur, enh);
            *enh = *cur;
        } else {
            memset(fbuf, 0, sizeof(fbuf));
            vocoder_reset(v);
        }
    } else if (bad == 7) {
        synth_tone(fbuf, d, cur);
        *prev = *cur;
    } else {
        memset(fbuf, 0, sizeof(fbuf));
        vocoder_reset(v);
    }

    /* float -> int16, 8x gain and soft clip at 95 % (as the reference) */
    const float maxamp = 32767.0f * 0.95f;
    for (int i = 0; i < N_SAMPLES; i++) {
        float a = 8.0f * fbuf[i];
        if (a > maxamp)
            a = maxamp;
        else if (a < -maxamp)
            a = -maxamp;
        pcm[i] = sat16((int32_t)a);
    }
}

int dmr_vocoder_decode_ambe_params(dmr_vocoder_t *v, const uint8_t d[49],
                           int16_t pcm[DMR_PCM_SAMPLES], int *errs)
{
    if (!v || !d || !pcm)
        return -1;

    decode_params(v, d, pcm, 0, errs);

    return 0;
}

/* Synthesis straight from the 49 parameter bits, the mirror of mbelib's
 * mbe_processAmbe2450Dataf(): the FEC (and any keystream the caller had to
 * take off d[]) is already done.  This is the second half of
 * dmr_vocoder_decode_frame(), split out for the encrypted DMR paths. */
int dmr_vocoder_decode_data(dmr_vocoder_t *v, const uint8_t d[49],
                            int16_t pcm[DMR_PCM_SAMPLES])
{
    if (!v || !d || !pcm)
        return -1;

    decode_params(v, d, pcm, 0, NULL);

    return 0;
}

/* 72-bit canonical frame -> the 49 FEC-corrected (still scrambled) data bits.
 * Returns the number of corrected bits, so that a caller taking a keystream
 * off them (Basic/Enhanced Privacy) reports the same figure as the clear
 * path; -1 on bad arguments. */
int dmr_vocoder_unpack(const uint8_t frame[DMR_FRAME_BYTES], uint8_t d[49])
{
    if (!frame || !d)
        return -1;

    return unpack_frame(frame, d, 1);
}

int dmr_vocoder_decode_frame(dmr_vocoder_t *v, const uint8_t frame[DMR_FRAME_BYTES],
                             int16_t pcm[DMR_PCM_SAMPLES], int *errs)
{
    if (!v || !frame || !pcm)
        return -1;

    uint8_t d[49];
    int nerr = unpack_frame(frame, d, errs != NULL);

    decode_params(v, d, pcm, nerr, errs);

    return 0;
}
