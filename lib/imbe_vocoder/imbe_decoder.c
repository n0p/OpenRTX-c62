/* imbe_decoder.c -- IMBE 7200x4400 decoder: parameters, FEC-free synthesis.
 *
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Derived from mbelib-neo (GPL-2.0-or-later), whose IMBE 7200x4400 decoder is
 * the reference this file is verified against: the parameter dequantiser of
 * its src/imbe/imbe7200x4400.c (TIA-102.BABA chapter 6), the spectral
 * enhancement, adaptive smoothing and mute noise of its src/core (TIA-102.BABA
 * 7.4-7.8), and its TIA-102.BABA 7.5 spec rendering (mbe_synthesizeSpeechf),
 * which is also what the firmware's AMBE+2 decoder implements -- the two
 * codecs share the MBE synthesis core, so this is the same code lineage as
 * lib/dmr_vocoder.
 *
 * What was changed for the M33, and why (same list as the AMBE+2 port):
 *   - one caller-owned instance instead of thread-local state: no TLS, no
 *     allocation, no FFT plan -- the transform is fixed at 256 points;
 *   - every table const in flash (imbe_tables.h, generated from the reference
 *     by tools/gen_imbe_tables.py) instead of a run-time DCT cosine cache;
 *   - libm replaced by imbe_math.h (float only: newlib's cosf reduces its
 *     argument in double, which costs more than the whole decoder);
 *   - the phase accumulators are wrapped into [0, 2*pi) per frame before any
 *     trigonometry touches them, exactly as the reference does;
 *   - the soft-bit, SIMD, tone and subframe paths dropped: the P25 receiver
 *     hands over hard bits and the spec rendering is the one this decoder
 *     implements.
 *
 * NOT implemented on purpose: the DVSI 10 ms subframe renderer that mbelib-neo
 * uses on its mbe_processImbe4400Data() path, and the frame marks that only
 * that renderer reads.  See docs/IMBE.md, "Renderer": the spec rendering is
 * TIA-102.BABA 7.5 and the reference's own public mbe_synthesizeSpeechf(), and
 * the two renderings differ by a documented amount that this port does not
 * hide -- the verification compares against the reference rendered the same
 * way, and quantifies the renderer gap separately.
 */

#include <string.h>

#include "imbe_decoder.h"
#include "imbe_fft.h"
#include "imbe_math.h"
#include "imbe_tables.h"

/* ---- constants of the IMBE path (mbe_adaptive.h / mbelib-neo's header) ---- */

#define HIGH_BAND_GAIN_IMBE 1.25892541f /* +2.0 dB above 3 kHz */
#define RENDER_VOICED_GAIN 1.03514217f  /* +0.3 dB, shared by the codec paths */
#define RENDER_UNVOICED_GAIN 0.95499259f
#define SPEECH_OUTPUT_GAIN (1.0f / 7.0f)
#define SPEC_MUTE_NOISE_AMPLITUDE 5.0f  /* TIA-102.BABA 7.8, on s(n) */
#define PCM_LIMIT (32767.0f * 0.95f)
#define FLOAT_LIMIT (PCM_LIMIT / 7.0f)

#define UNVOICED_SCALE_COEFF 146.17696f /* TIA-102.BABA eq. 122 */
#define LOW_BAND_CORNER_HZ 100.0f

#define DEFAULT_LOCAL_ENERGY 75000.0f
#define MIN_LOCAL_ENERGY 10000.0f
#define DEFAULT_AMPLITUDE_THRESHOLD 20480
#define ERROR_THRESHOLD_ENTRY 0.0125f
#define ERROR_THRESHOLD_LOW 0.005f
#define ADAPTIVE_GAIN 45.255f
#define ADAPTIVE_EXPONENT 277.26f
#define ADAPTIVE_ALT 1.414f
#define AMPLITUDE_PENALTY_PER_ERROR 300
#define AMPLITUDE_BASE 6000
#define AMPLITUDE_SAFETY_LIMIT 1.0e9 /* mbe_amplitude_limit NONE: no eq. 116 scaling */
#define LCG_DEFAULT_SEED 3147.0f

/* ---- model state ------------------------------------------------------- */

/* mbe_resetImbeModel(): the default voice model of the IMBE path. */
static void reset_imbe_model(imbe_parms_t *p)
{
    p->w0 = (float)(4.0 * 3.14159265358979323846 / (134.0 + 39.5));
    p->L = (int32_t)(0.9254 * (int32_t)(3.14159265358979323846 / (double)p->w0 + 0.25));
    p->K = 12;
    p->gamma = 0.0f;
    for (int l = 0; l <= IMBE_MAX_HARMONICS; l++) {
        p->Ml[l] = 1.0f;
        p->Vl[l] = 0;
        p->log2Ml[l] = 0.0f;
    }
    p->swn = 0;
    p->repeatCount = 0;
    p->PHIl[0] = 0.0f; /* the reference's frame-mark slot */
    p->localEnergy = DEFAULT_LOCAL_ENERGY;
    p->amplitudeThreshold = DEFAULT_AMPLITUDE_THRESHOLD;
    p->mutingThreshold = IMBE_MUTING_THRESHOLD;
}

/* mbe_initMbeParms(): default model, fresh noise state. */
static void parms_init(imbe_parms_t *p)
{
    memset(p, 0, sizeof(*p));
    reset_imbe_model(p);
    p->noiseSeed = -1.0f;
}

static void parms_copy(imbe_parms_t *dst, const imbe_parms_t *src)
{
    *dst = *src;
}

/* mbe_repeat_load_model(): the previous model, keeping this frame's error and
 * generator state (a repeat must not replay the previous noise buffer). */
static void repeat_load_model(imbe_parms_t *cur, const imbe_parms_t *prev)
{
    imbe_parms_t m = *prev;
    m.errorRate = cur->errorRate;
    m.errorCountTotal = cur->errorCountTotal;
    m.errorCount4 = cur->errorCount4;
    m.repeatCount = cur->repeatCount;
    m.mutingThreshold = cur->mutingThreshold;
    m.swn = cur->swn;
    m.noiseSeed = cur->noiseSeed;
    memcpy(m.noiseOverlap, cur->noiseOverlap, sizeof(m.noiseOverlap));
    memcpy(m.previousUw, cur->previousUw, sizeof(m.previousUw));
    *cur = m;
}

static int repeat_next_count(int prev_count)
{
    if (prev_count < 0)
        return 1;
    return (prev_count >= IMBE_MAX_FRAME_REPEATS) ? IMBE_MAX_FRAME_REPEATS : prev_count + 1;
}

/* ---- bit helpers ------------------------------------------------------- */

/* mbe_bits_by_index_to_int(): the listed bits, first index most significant. */
static int bits_by_index(const uint8_t *d, const unsigned *idx, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (d[idx[i]] & 1u);
    return v;
}

/* mbe_bits_descending_to_int(): bits hi..lo, hi most significant. */
static int bits_descending(const uint8_t bb[], int hi, int lo)
{
    int v = 0;
    for (int i = hi; i >= lo; i--)
        v = (v << 1) | (bb[i] & 1u);
    return v;
}

/* ---- parameter dequantiser (TIA-102.BABA chapter 6) -------------------- */

static int decode_fundamental(const uint8_t *d, imbe_parms_t *cur, int *out_b0, int *out_l9)
{
    static const unsigned b0_idx[8] = {0u, 1u, 2u, 3u, 4u, 5u, 85u, 86u};
    int b0 = bits_by_index(d, b0_idx, 8);

    *out_b0 = b0;
    if (b0 > 207) /* 6.1: 208..255 are not voice frames */
        return 1;

    cur->w0 = (float)(4.0 * 3.14159265358979323846) / (float)((float)b0 + 39.5f);

    int L = (int)(0.9254 * (int)((3.14159265358979323846 / (double)cur->w0) + 0.25));
    if (L > IMBE_MAX_HARMONICS || L < 9)
        return 1;
    cur->L = L;
    *out_l9 = L - 9;

    if (L < 37)
        cur->K = (int)((float)(L + 2) / (float)3);
    else
        cur->K = 12;
    return 0;
}

static void read_bit_layout(const uint8_t *d, int l9, uint8_t bb[58][12])
{
    memset(bb, 0, 58 * 12);
    for (int i = 6; i < 85; i++) {
        bb[imbe_bo[l9][i - 6][0]][imbe_bo[l9][i - 6][1]] = d[i];
    }
}

static void decode_voicing(imbe_parms_t *cur, uint8_t bb[58][12])
{
    int j = 1;
    int k = cur->K - 1;

    for (int i = 1; i <= cur->L; i++) {
        cur->Vl[i] = bb[1][k];
        if (j == 3) {
            j = 1;
            k = (k > 0) ? k - 1 : 0;
        } else {
            j++;
        }
    }
}

static void decode_gains(uint8_t bb[58][12], int l9, float Gm[7])
{
    int b2 = bits_descending(bb[2], 5, 0);
    Gm[1] = imbe_B2[b2];

    for (int i = 2; i <= 6; i++) {
        const float ba1 = imbe_ba[l9][i - 2][0];
        const float ba2 = imbe_ba[l9][i - 2][1];
        int bm = bits_descending(bb[i + 1], (int)ba1 - 1, 0);
        Gm[i] = ba2 * ((float)bm - imbe_exp2f(ba1 - 1.0f) + 0.5f);
    }
}

static void compute_ri(const float Gm[7], float Ri[7])
{
    for (int i = 1; i <= 6; i++) {
        float sum = 0.0f;
        for (int m = 1; m <= 6; m++) {
            int am = (m == 1) ? 1 : 2;
            sum = sum + ((float)am * Gm[m] * imbe_ri_cos[m][i]);
        }
        Ri[i] = sum;
    }
}

static void decode_hoc(uint8_t bb[58][12], int l9, const float Ri[7], float Cik[7][11])
{
    int m = 8;

    memset(Cik, 0, 7 * 11 * sizeof(float));
    for (int i = 1; i <= 6; i++) {
        Cik[i][1] = Ri[i];
        for (int k = 2; k <= imbe_ImbeJi[l9][i - 1]; k++) {
            int Bm = imbe_hoba[l9][m - 8];
            if (Bm <= 0) {
                Cik[i][k] = 0.0f;
            } else {
                int bm = bits_descending(bb[m], Bm - 1, 0);
                Cik[i][k] = (imbe_quantstep[Bm - 1] * imbe_standdev[k - 2]) *
                            (((float)bm - imbe_exp2f((float)Bm - 1.0f)) + 0.5f);
            }
            m++;
        }
    }
}

static void inverse_dct_tl(float Cik[7][11], int l9, float Tl[57])
{
    int l = 1;

    memset(Tl, 0, 57 * sizeof(float));
    for (int i = 1; i <= 6; i++) {
        const int ji = imbe_ImbeJi[l9][i - 1];
        const float *tab = &imbe_idct_cos[imbe_idct_off[ji]];
        for (int j = 1; j <= ji; j++) {
            const float *row = tab + (j - 1) * ji;
            float sum = 0.0f;
            for (int k = 1; k <= ji; k++) {
                int ak = (k == 1) ? 1 : 2;
                sum = sum + ((float)ak * Cik[i][k] * row[k - 1]);
            }
            Tl[l] = sum;
            l++;
        }
    }
}

static float spectral_rho(int L)
{
    if (L <= 15)
        return 0.4f;
    if (L <= 24)
        return (0.03f * (float)L) - 0.05f;
    return 0.7f;
}

static void update_spectral_amplitudes(imbe_parms_t *cur, imbe_parms_t *prev, const float Tl[57], float rho)
{
    int intkl[57];
    float deltal[57];
    const int cur_l = cur->L;
    const int prev_l = prev->L;

    if (cur_l > prev_l) {
        for (int l = prev_l + 1; l <= cur_l; l++) {
            prev->Ml[l] = prev->Ml[prev_l];
            prev->log2Ml[l] = prev->log2Ml[prev_l];
        }
    }
    prev->log2Ml[0] = prev->log2Ml[1];
    prev->Ml[0] = prev->Ml[1];

    float sum = 0.0f;
    for (int l = 1; l <= cur_l; l++) {
        const float flokl = ((float)prev_l / (float)cur_l) * (float)l;
        intkl[l] = (int)flokl;
        if (intkl[l] < 0)
            intkl[l] = 0;
        else if (intkl[l] > IMBE_MAX_HARMONICS)
            intkl[l] = IMBE_MAX_HARMONICS;
        deltal[l] = flokl - (float)intkl[l];
        int upper = intkl[l] + 1;
        if (upper > IMBE_MAX_HARMONICS)
            upper = IMBE_MAX_HARMONICS;
        sum = sum + (((1.0f - deltal[l]) * prev->log2Ml[intkl[l]]) + (deltal[l] * prev->log2Ml[upper]));
    }
    sum = (rho / (float)cur_l) * sum;

    for (int l = 1; l <= cur_l; l++) {
        int upper = intkl[l] + 1;
        if (upper > IMBE_MAX_HARMONICS)
            upper = IMBE_MAX_HARMONICS;
        const float c1 = rho * ((1.0f - deltal[l]) * prev->log2Ml[intkl[l]]);
        const float c2 = rho * deltal[l] * prev->log2Ml[upper];
        cur->log2Ml[l] = Tl[l] + c1 + c2 - sum;
        cur->Ml[l] = imbe_exp2f(cur->log2Ml[l]);
    }
}

/* mbe_decodeImbe4400Parms(): 0 on a voice frame, 1 when b0 is not voice. */
static int decode_imbe4400_parms(const uint8_t *d, imbe_parms_t *cur, imbe_parms_t *prev)
{
    float Cik[7][11], Tl[57], Gm[7], Ri[7];
    uint8_t bb[58][12];
    int b0, l9;

    if (decode_fundamental(d, cur, &b0, &l9) != 0)
        return 1;

    read_bit_layout(d, l9, bb);
    decode_voicing(cur, bb);
    decode_gains(bb, l9, Gm);
    compute_ri(Gm, Ri);
    decode_hoc(bb, l9, Ri, Cik);
    inverse_dct_tl(Cik, l9, Tl);
    update_spectral_amplitudes(cur, prev, Tl, spectral_rho(cur->L));
    return 0;
}

/* ---- spectral amplitude enhancement (TIA-102.BABA 7.4) ----------------- */

static float high_band_gain(float hz, float gain)
{
    if (hz <= 2500.0f || hz >= 3800.0f)
        return 1.0f;
    float t;
    if (hz < 3000.0f)
        t = (hz - 2500.0f) / 500.0f;
    else if (hz <= 3600.0f)
        t = 1.0f;
    else
        t = (3800.0f - hz) / 200.0f;
    return 1.0f + ((gain - 1.0f) * t);
}

/* mbe_spectralAmpEnhanceWithRm0(); returns the pre-enhancement Rm0, which the
 * adaptive smoothing of the same frame uses. */
static float spectral_amp_enhance_with_rm0(imbe_decoder_t *v, imbe_parms_t *cur)
{
    const int L = cur->L;
    float *cos_tab = v->cos_tab;

    /* harmonic cosines by complex rotation, as the reference */
    float s_step, c_step;
    imbe_sincosf(cur->w0, &s_step, &c_step);
    {
        float c = 1.0f, s = 0.0f;
        for (int l = 1; l <= L; l++) {
            const float cn = (c * c_step) - (s * s_step);
            const float sn = (s * c_step) + (c * s_step);
            c = cn;
            s = sn;
            cos_tab[l] = c;
        }
    }

    float Rm0 = 0.0f, Rm1 = 0.0f;
    for (int l = 1; l <= L; l++) {
        const float m2 = cur->Ml[l] * cur->Ml[l];
        Rm0 += m2;
        Rm1 += m2 * cos_tab[l];
    }

    if (Rm0 > 0.0f && cur->w0 > 0.0f) {
        const float r = Rm1 / Rm0;
        const float shape = (0.96f * IMBE_PI) / (cur->w0 * (1.0f - (r * r)));
        const float inv_sqrt_rm0 = 1.0f / imbe_sqrtf(Rm0);

        for (int l = 1; l <= L; l++) {
            if (cur->Ml[l] != 0.0f) {
                const float spread = imbe_fabsf(((1.0f + (r * r)) - (2.0f * r * cos_tab[l])));
                const float Wl = imbe_sqrtf(cur->Ml[l] * inv_sqrt_rm0) * imbe_sqrtf(imbe_sqrtf(shape * spread));
                if ((8 * l) <= L) {
                    /* low harmonics keep their amplitude */
                } else if (Wl > 1.2f) {
                    cur->Ml[l] = 1.2f * cur->Ml[l];
                } else if (Wl < 0.5f) {
                    cur->Ml[l] = 0.5f * cur->Ml[l];
                } else {
                    cur->Ml[l] = Wl * cur->Ml[l];
                }
            }
        }

        float sum = 0.0f;
        for (int l = 1; l <= L; l++) {
            const float m = cur->Ml[l];
            sum += m * m;
        }
        const float gamma = (sum == 0.0f) ? 1.0f : imbe_sqrtf(Rm0 / sum);
        const float hz_per_harmonic = cur->w0 * (8000.0f / (2.0f * IMBE_PI));
        for (int l = 1; l <= L; l++) {
            cur->Ml[l] = cur->Ml[l] * gamma * high_band_gain(hz_per_harmonic * (float)l, HIGH_BAND_GAIN_IMBE);
        }
    }
    return Rm0;
}

/* ---- adaptive smoothing (TIA-102.BABA 7.6, algorithms 111-116) --------- */

static float voicing_threshold(const imbe_parms_t *p)
{
    if (p->errorRate <= ERROR_THRESHOLD_LOW && p->errorCountTotal <= 4)
        return 3.40282347e+38f; /* FLT_MAX */

    const float root = imbe_sqrtf(imbe_sqrtf(imbe_sqrtf(p->localEnergy)));
    const float energy_power = (root * root) * root;
    if (p->errorRate <= ERROR_THRESHOLD_ENTRY && p->errorCount4 == 0) {
        const float gain = ADAPTIVE_GAIN * energy_power;
        const float decay = imbe_expf(ADAPTIVE_EXPONENT * p->errorRate);
        return gain / decay;
    }
    return ADAPTIVE_ALT * energy_power;
}

static int amplitude_threshold(const imbe_parms_t *cur, int prior)
{
    if (cur->errorRate <= ERROR_THRESHOLD_LOW && cur->errorCountTotal <= 6)
        return DEFAULT_AMPLITUDE_THRESHOLD;
    if (prior <= 0)
        prior = DEFAULT_AMPLITUDE_THRESHOLD;
    return prior + AMPLITUDE_BASE - (AMPLITUDE_PENALTY_PER_ERROR * cur->errorCountTotal);
}

/* mbe_applyAdaptiveSmoothingWithRm0Limit(..., limit = NONE): the IMBE path
 * scales by the safety ceiling, not by eq. 116's amplitude threshold. */
static void apply_adaptive_smoothing(imbe_parms_t *cur, const imbe_parms_t *prev, float rm0)
{
    float prior_energy = prev->localEnergy;
    if (prior_energy < MIN_LOCAL_ENERGY)
        prior_energy = DEFAULT_LOCAL_ENERGY;

    float energy = (0.95f * prior_energy) + (0.05f * rm0);
    if (energy < MIN_LOCAL_ENERGY)
        energy = MIN_LOCAL_ENERGY;
    cur->localEnergy = energy;

    const float threshold = voicing_threshold(cur);
    double amplitude_sum = 0.0;
    for (int l = 1; l <= cur->L; l++) {
        if (cur->Ml[l] > threshold)
            cur->Vl[l] = 1;
        amplitude_sum += (double)cur->Ml[l];
    }
    cur->amplitudeThreshold = amplitude_threshold(cur, prev->amplitudeThreshold);

    float scale = 1.0f;
    const double ceiling = AMPLITUDE_SAFETY_LIMIT;
    if (amplitude_sum > 0.0 && ceiling <= amplitude_sum)
        scale = (float)(ceiling / amplitude_sum);
    if (scale < 0.0f)
        scale = 0.0f;
    for (int l = 1; l <= cur->L; l++)
        cur->Ml[l] *= scale;
}

/* ---- DVSI output shaping: low-band roll-off and its phase ------------- */

static float low_band_gain(float hz)
{
    if (!(hz > 0.0f))
        return 0.0f;
    const float hz2 = hz * hz;
    const float corner2 = LOW_BAND_CORNER_HZ * LOW_BAND_CORNER_HZ;
    return hz2 / imbe_sqrtf((hz2 * hz2) + (corner2 * corner2));
}

static float low_band_phase(float hz)
{
    if (!(hz > 0.0f))
        return 0.0f;
    const float x = hz / LOW_BAND_CORNER_HZ;
    return IMBE_PI - imbe_atan2f(IMBE_SQRT2 * x, 1.0f - (x * x));
}

/* ---- regenerated phase (US 5,701,390 eqs. 7-9) ------------------------ */

#define PHASE_KERNEL_HALF_LEN 19
#define PHASE_KERNEL_SCALE 0.44f
#define PHASE_EXTRAP_SLOPE 0.72f
#define PHASE_MIN_MAGNITUDE 1e-6f

static void regenerate_phases(const float Ml[57], int L, float phi[57])
{
    static const float inv_odd[10] = {1.0f, 1.0f / 3, 1.0f / 5, 1.0f / 7, 1.0f / 9,
                                      1.0f / 11, 1.0f / 13, 1.0f / 15, 1.0f / 17, 1.0f / 19};
    const int D = PHASE_KERNEL_HALF_LEN;
    float B[56 + 2 * PHASE_KERNEL_HALF_LEN + 1];

    B[D] = 0.0f;
    for (int l = 1; l <= L; l++) {
        const float m = (Ml[l] > PHASE_MIN_MAGNITUDE) ? Ml[l] : PHASE_MIN_MAGNITUDE;
        B[D + l] = imbe_log2f(m);
    }
    for (int l = L + 1; l <= 56 + D; l++)
        B[D + l] = B[D + L] - PHASE_EXTRAP_SLOPE * (float)(l - L);
    for (int l = 1; l <= D; l++)
        B[D - l] = B[D + l];

    for (int l = 1; l <= 56; l++) {
        float acc = 0.0f;
        for (int k = 0; k < 10; k++) {
            const int m = 2 * k + 1;
            acc += (B[D + l + m] - B[D + l - m]) * inv_odd[k];
        }
        phi[l] = PHASE_KERNEL_SCALE * acc;
    }
}

/* ---- unvoiced noise and the synthesis window -------------------------- */

static float synthesis_window(int n)
{
    if (n <= -105 || n >= 105)
        return 0.0f;
    if (n < 0)
        n = -n;
    if (n <= 55)
        return 1.0f;
    return imbe_taper[105 - n];
}

static uint32_t usable_noise_seed(float seed)
{
    const uint32_t bits = imbe_bitcast_u(seed);
    if (bits == 0x80000000u) /* -0.0: no state */
        return 0u;
    if (bits > 0x4f7fffffu) /* not an exact 16-bit value */
        return (uint32_t)LCG_DEFAULT_SEED;
    return (uint32_t)seed % 53125u;
}

static void generate_noise_lcg(float *buffer, int count, float *seed)
{
    uint32_t state = usable_noise_seed(*seed);
    for (int i = 0; i < count; i++) {
        buffer[i] = (float)state;
        state = (171u * state + 11213u) % 53125u;
    }
    *seed = (float)state;
}

/* mbe_generate_noise_with_overlap(): 256 samples, the first 96 from the
 * previous frame's tail.  A negative seed is a cold start: silence this frame,
 * and the stream restarts from the default seed. */
static void generate_noise_with_overlap(float *buffer, float *seed, float *overlap)
{
    if (*seed < 0.0f) {
        memset(buffer, 0, IMBE_FFT_N * sizeof(float));
        memset(overlap, 0, 96 * sizeof(float));
        *seed = LCG_DEFAULT_SEED;
        return;
    }
    memmove(buffer, overlap, 96 * sizeof(float));
    generate_noise_lcg(buffer + 96, IMBE_FFT_N - 96, seed);
    memmove(overlap, buffer + IMBE_FFT_N - 96, 96 * sizeof(float));
}

/* ---- voiced rendering (TIA-102.BABA 7.5) ------------------------------ */

/* Add 160 samples of a windowed cosine oscillator.  The oscillator advances by
 * a complex rotation four samples at a time, exactly like the reference's
 * scalar path (and its vector kernel, whose four lanes hold the same four
 * chained values). */
static void add_voiced(float *Ss, const float *W, float amp, float phi, float step)
{
    float sd, cd;
    imbe_sincosf(step, &sd, &cd);
    float s, c;
    imbe_sincosf(phi, &s, &c);
    const float wa = 2.0f * amp; /* exact: a power of two */

    for (int n = 0; n < IMBE_PCM_SAMPLES; n += 4) {
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

        Ss[n + 0] += (wa * W[n + 0]) * c0;
        Ss[n + 1] += (wa * W[n + 1]) * c1;
        Ss[n + 2] += (wa * W[n + 2]) * c2;
        Ss[n + 3] += (wa * W[n + 3]) * c3;
    }
}

static void render_voiced_interpolated(float *out, const imbe_parms_t *cur, const imbe_parms_t *prev,
                                       int l, int N, float cw0, float pw0, float pw0l)
{
    const float deltaphil = cur->PHIl[l] - prev->PHIl[l] - (((pw0 + cw0) * (float)(l * N)) / 2.0f);
    const float deltawl = (1.0f / (float)N) *
        (deltaphil - (IMBE_TWO_PI * (float)imbe_ifloor((deltaphil + IMBE_PI) / IMBE_TWO_PI)));
    const float phi0 = prev->PHIl[l];
    const float ampd = cur->Ml[l] - prev->Ml[l];
    const float mprev = prev->Ml[l];
    const float wstep = pw0l + deltawl;

    for (int n = 0; n < N; n++) {
        /* the reference multiplies by (l*n*n) and divides by 2N, in that
         * order: the integer product is exact, so the rounding differs from
         * folding l/(2N) into a constant */
        const float thetaln = phi0 + (wstep * (float)n) + (((cw0 - pw0) * (float)(l * n * n)) / (float)(2 * N));
        const float aln = mprev + ((float)n / (float)N) * ampd;
        out[n] += (2.0f * aln) * imbe_cosf(thetaln);
    }
}

/* mbe_render_voiced_speech() with the low-band roll-off already folded into
 * Ml/PHIl by render_prepare(). */
static void render_voiced(float *out, const imbe_parms_t *cur, const imbe_parms_t *prev, int maxl, int N)
{
    const float cw0 = cur->w0;
    const float pw0 = prev->w0;

    for (int l = 1; l <= maxl; l++) {
        const float cw0l = cw0 * (float)l;
        const float pw0l = pw0 * (float)l;
        const int cur_voiced = (cur->Vl[l] == 1) && (cw0l < IMBE_PI);
        const int prev_voiced = (prev->Vl[l] == 1) && (pw0l < IMBE_PI);

        if (!(cur_voiced || prev_voiced))
            continue;

        const int use_interp = (l < 8) && cur_voiced && prev_voiced &&
                               (imbe_fabsf(cw0 - pw0) < (0.1f * cw0));
        if (use_interp) {
            render_voiced_interpolated(out, cur, prev, l, N, cw0, pw0, pw0l);
        } else {
            if (prev_voiced)
                add_voiced(out, imbe_ws + N, prev->Ml[l], prev->PHIl[l], pw0l);
            if (cur_voiced)
                add_voiced(out, imbe_ws, cur->Ml[l], cur->PHIl[l] - (cw0l * (float)N), cw0l);
        }
    }
}

/* Fold the output shaping (low-band roll-off) and the voicing gains into what
 * the render reads, keeping the model as decoded. */
typedef struct {
    float Ml[57];
    float PHIl[57];
} render_saved_t;

static void render_prepare(imbe_parms_t *mp, render_saved_t *saved, int maxl, float voiced_gain)
{
    const float hz_per_harmonic = mp->w0 * (8000.0f / (2.0f * IMBE_PI));
    for (int l = 1; l <= 56; l++) {
        const float hz = hz_per_harmonic * (float)l;
        const int voiced = (l <= mp->L) && (mp->Vl[l] == 1);
        saved->Ml[l] = mp->Ml[l];
        saved->PHIl[l] = mp->PHIl[l];
        mp->Ml[l] *= low_band_gain(hz) * (voiced ? voiced_gain : RENDER_UNVOICED_GAIN);
        if (l <= maxl && mp->Vl[l] == 1)
            mp->PHIl[l] += low_band_phase(hz);
    }
}

static void render_restore(imbe_parms_t *mp, const render_saved_t *saved)
{
    for (int l = 1; l <= 56; l++) {
        mp->Ml[l] = saved->Ml[l];
        mp->PHIl[l] = saved->PHIl[l];
    }
}

/* ---- unvoiced rendering (TIA-102.BABA eqs. 117-126) ------------------- */

static int band_edge(float frequency)
{
    if (frequency >= 128.0f)
        return 128;
    const int e = imbe_iceil(frequency);
    return (e < 0) ? 0 : e;
}

static void shape_unvoiced(imbe_decoder_t *v, const imbe_parms_t *cur)
{
    /* 256/(2*pi) as the reference computes it: the literal is double, so the
     * quotient is too and only the product below is float.  Rounding it in
     * float instead moves a band edge by one bin on a fraction of the frames,
     * which changes that band's noise amplitude by percent. */
    const float spacing = (float)(256.0 / (2.0 * 3.14159265358979323846)) * cur->w0;

    memset(v->bin_scale, 0, sizeof(v->bin_scale));
    for (int l = 1; l <= cur->L; l++) {
        if (cur->Vl[l] != 0)
            continue;
        const int lower = band_edge(((float)l - 0.5f) * spacing);
        const int upper = band_edge(((float)l + 0.5f) * spacing);
        if (upper <= lower)
            continue;
        float power = 0.0f;
        for (int k = lower; k < upper; k++)
            power += (v->fft_re[k] * v->fft_re[k]) + (v->fft_im[k] * v->fft_im[k]);
        if (!(power > 1.0e-10f))
            continue;
        const float scale = (UNVOICED_SCALE_COEFF * cur->Ml[l]) / imbe_sqrtf(power / (float)(upper - lower));
        for (int k = lower; k < upper; k++)
            v->bin_scale[k] = scale;
    }
    for (int k = 0; k <= IMBE_FFT_N / 2; k++) {
        v->fft_re[k] *= v->bin_scale[k];
        v->fft_im[k] *= v->bin_scale[k];
    }
}

/* WOLA of the previous and current unvoiced blocks, TIA-102.BABA eqs. 124-126:
 * the windows add up in power, so the sum is divided by their power sum. */
static void wola_combine(float *out, const float *prev_uw, const float *cur_uw, int N)
{
    for (int n = 0; n < N; n++) {
        const float wp = synthesis_window(n);
        const float wc = synthesis_window(n - N);
        const float weight = (wp * wp) + (wc * wc);
        float sample = 0.0f;
        if (wp > 0.0f)
            sample += wp * prev_uw[128 + n];
        if (wc > 0.0f)
            sample += wc * cur_uw[(128 + n) - N];
        if (weight > 0.0f)
            out[n] += sample / weight;
    }
}

static void render_unvoiced(imbe_decoder_t *v, float *out, imbe_parms_t *cur, const imbe_parms_t *prev,
                            const float *noise_buffer)
{
    for (int i = 0; i < IMBE_FFT_N; i++)
        v->uw[i] = noise_buffer[i] * synthesis_window(i - (IMBE_FFT_N / 2));

    imbe_rfft256(v->uw, v->fft_re, v->fft_im);
    shape_unvoiced(v, cur);
    imbe_irfft256(v->fft_re, v->fft_im, v->uw);

    const float sc = 1.0f / (float)IMBE_FFT_N;
    for (int i = 0; i < IMBE_FFT_N; i++)
        v->uw[i] *= sc;

    wola_combine(out, prev->previousUw, v->uw, IMBE_PCM_SAMPLES);
    memcpy(cur->previousUw, v->uw, sizeof(cur->previousUw));
}

/* ---- one frame (mbe_render_spec_frame + its callers) ------------------ */

static int reconcile_lengths(imbe_parms_t *cur, imbe_parms_t *prev)
{
    if (cur->L > prev->L) {
        for (int l = prev->L + 1; l <= cur->L; l++) {
            prev->Ml[l] = 0.0f;
            prev->Vl[l] = 1;
        }
        return cur->L;
    }
    for (int l = cur->L + 1; l <= prev->L; l++) {
        cur->Ml[l] = 0.0f;
        cur->Vl[l] = 1;
    }
    return prev->L;
}

/* x reduced into [0, 2*pi): the reference's fmodf(psi, 2*pi) plus the negative
 * fix-up, done without libm.  2*pi is split so k*hi is exact (hi has 8
 * significant bits) and k*lo carries the rest; the residue is then within an
 * ulp of the exact remainder. */
static float wrap_two_pi(float x)
{
    const float k = (float)(int32_t)((x * 0.15915494309189535f) + ((x >= 0.0f) ? 0.5f : -0.5f));
    float r = x - (k * 6.28125f);        /* exact product */
    r = r - (k * 0.00193530717958647692f);
    if (r < 0.0f)
        r += IMBE_TWO_PI;
    if (r >= IMBE_TWO_PI)
        r -= IMBE_TWO_PI;
    return r;
}

/* PSIl is the integrated linear phase (TIA-102.BABA eq. 139), PHIl the phase
 * at the frame's end: PSIl plus the regenerated spectral-envelope phase. */
static void update_speech_phases(imbe_parms_t *cur, imbe_parms_t *prev, int N)
{
    const float cw0 = cur->w0;
    const float pw0 = prev->w0;
    float phi[57];

    regenerate_phases(cur->Ml, cur->L, phi);

    for (int l = 1; l <= 56; l++) {
        /* Wrapped before any trigonometry touches it, as the reference does:
         * cos() of a small argument is where the float arithmetic is exact. */
        const float wrapped = wrap_two_pi(prev->PSIl[l]);
        prev->PSIl[l] = wrapped;

        cur->PSIl[l] = wrapped + ((pw0 + cw0) * ((float)(l * N) / 2.0f));
        cur->PHIl[l] = cur->PSIl[l] + phi[l];
    }
}

static void render_spec_frame(imbe_decoder_t *v, float *out, imbe_parms_t *cur, imbe_parms_t *prev, int N)
{
    render_saved_t saved_cur, saved_prev;
    int maxl;

    /* Algorithm 117: the unvoiced noise first, so its overlap and generator
     * state advance independently of the voiced phase. */
    generate_noise_with_overlap(v->noise, &cur->noiseSeed, cur->noiseOverlap);

    maxl = reconcile_lengths(cur, prev);
    update_speech_phases(cur, prev, N);
    render_prepare(cur, &saved_cur, maxl, RENDER_VOICED_GAIN);
    render_prepare(prev, &saved_prev, maxl, RENDER_VOICED_GAIN);

    render_voiced(out, cur, prev, maxl, N);
    render_unvoiced(v, out, cur, prev, v->noise);

    render_restore(cur, &saved_cur);
    render_restore(prev, &saved_prev);
}

/* TIA-102.BABA 7.8 mute noise: uniform in [-5, 5] on s(n).  Java's Random LCG,
 * as the reference draws it. */
static void render_mute_noise(imbe_decoder_t *v, float *out)
{
    /* s(n) scale: the core's 1/7 is applied by the PCM conversion, which for a
     * muted frame is the only scaling (the reference's mute branch returns
     * before the core scales). */
    const float amplitude = SPEC_MUTE_NOISE_AMPLITUDE;
    for (int i = 0; i < IMBE_PCM_SAMPLES; i++) {
        v->cn_state = ((v->cn_state * 0x5deece66dULL) + 11u) & 0xffffffffffffULL;
        const float unit = (float)(v->cn_state >> 24) * (1.0f / 16777216.0f);
        out[i] = ((unit * 2.0f) - 1.0f) * amplitude;
    }
}

static int synthesize(imbe_decoder_t *v, float *out, imbe_parms_t *cur, imbe_parms_t *prev, float rm0)
{
    apply_adaptive_smoothing(cur, prev, rm0);

    if (cur->repeatCount >= IMBE_MAX_FRAME_REPEATS || cur->errorRate > cur->mutingThreshold) {
        render_mute_noise(v, out);
        return 1;
    }

    memset(out, 0, IMBE_PCM_SAMPLES * sizeof(float));
    render_spec_frame(v, out, cur, prev, IMBE_PCM_SAMPLES);
#ifdef IMBE_TRACE
    {   extern int imbe_trace_frame, imbe_trace_want; extern void imbe_trace_dump(const float *);
        if (imbe_trace_frame == imbe_trace_want) imbe_trace_dump(out);
        imbe_trace_frame++;
    }
#endif
    return 0;
}

/* ---- PCM output ------------------------------------------------------- */

static int16_t pcm_from_float(float s, int already_scaled)
{
    /* mbe_synthesizeSpeechCore() scales by 1/7 and limits, mbe_floattoshort()
     * multiplies by 7, limits again and truncates toward zero. */
    float x = already_scaled ? s : (s * SPEECH_OUTPUT_GAIN);
    if (x > FLOAT_LIMIT)
        x = FLOAT_LIMIT;
    else if (x < -FLOAT_LIMIT)
        x = -FLOAT_LIMIT;
    x *= 7.0f;
    if (x != x) /* NaN */
        return 0;
    if (x > PCM_LIMIT)
        x = PCM_LIMIT;
    else if (x < -PCM_LIMIT)
        x = -PCM_LIMIT;
    return (int16_t)x;
}

/* ---- public API ------------------------------------------------------- */

void imbe_decoder_init(imbe_decoder_t *v)
{
    if (v == NULL)
        return;
    /* The instance may live in .psram_section, which is NOLOAD and holds
     * whatever the previous power-up left there: clear it whole. */
    memset(v, 0, sizeof(*v));
    parms_init(&v->prev);
    parms_copy(&v->cur, &v->prev);
    parms_copy(&v->enh, &v->prev);
    v->cn_state = 0x12345678ULL ^ 0x5deece66dULL;
}

size_t imbe_decoder_state_size(void)
{
    return sizeof(imbe_decoder_t);
}

void imbe_unpack_params(const uint8_t bytes[11], uint8_t params[IMBE_PARAM_BITS])
{
    for (int i = 0; i < IMBE_PARAM_BITS; i++)
        params[i] = (bytes[i >> 3] >> (7 - (i & 7))) & 1u;
}

int imbe_decoder_decode(imbe_decoder_t *v, const uint8_t params[IMBE_PARAM_BITS],
                        int total_errors, int16_t pcm[IMBE_PCM_SAMPLES])
{
    return imbe_decoder_decode_ex(v, params, total_errors, pcm, NULL);
}

int imbe_decoder_decode_ex(imbe_decoder_t *v, const uint8_t params[IMBE_PARAM_BITS],
                           int total_errors, int16_t pcm[IMBE_PCM_SAMPLES], unsigned *flags)
{
    imbe_parms_t *cur, *prev, *enh;
    float fbuf[IMBE_PCM_SAMPLES];
    float rm0;
    int bad, repeat, muted;
    unsigned f = 0;

    if (v == NULL || params == NULL || pcm == NULL)
        return -1;

    cur = &v->cur;
    prev = &v->prev;
    enh = &v->enh;

    /* mbe_result_prepare_synthesis() + imbe4400_prepare_process(): error
     * accounting first, because the repeat and mute decisions read it. */
    if (total_errors < 0)
        total_errors = 0;
    cur->errorCount4 = 0;
    cur->mutingThreshold = IMBE_MUTING_THRESHOLD;
    cur->errorCountTotal = total_errors;
    cur->errorRate = (0.95f * prev->errorRate) + (0.000365f * (float)total_errors);

    bad = decode_imbe4400_parms(params, cur, prev);
    if (bad != 0)
        f |= 4u;

    /* The parameter-bits entry point has no C0 count, so the reference's
     * repeat rule for it is the total-error one (imbe4400_repeat_required). */
    repeat = (bad == 1) || (total_errors > 5);

    if (repeat) {
        f |= 1u;
        if (prev->repeatCount > (IMBE_MAX_FRAME_REPEATS - 1)) {
            reset_imbe_model(cur);
        } else {
            repeat_load_model(cur, prev);
            cur->repeatCount = repeat_next_count(prev->repeatCount);
        }
    } else {
        cur->repeatCount = 0;
    }

    parms_copy(prev, cur);
    rm0 = spectral_amp_enhance_with_rm0(v, cur);
    muted = synthesize(v, fbuf, cur, enh, rm0);
    if (muted)
        f |= 2u;
    parms_copy(enh, cur);

    for (int i = 0; i < IMBE_PCM_SAMPLES; i++)
        pcm[i] = pcm_from_float(fbuf[i], muted);

    if (flags != NULL)
        *flags = f;
    return (f & 1u) ? 1 : 0;
}
