/* dstar_decoder.c -- AMBE 3600x2400 decoder core (D-STAR DV voice), pure C.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Port of mbelib-neo v2.6.2's D-STAR path (GPL-2.0-or-later) to a
 * Cortex-M33-class MCU, the way lib/dmr_vocoder ports the AMBE+2 one:
 *
 *   - one caller-owned state structure, every table in flash, no heap, no
 *     thread-local storage, no C++ and no libm (sin/cos/exp2/log2/sqrt come
 *     from dmr_math.h, the 256-point real FFT from dmr_fft.c);
 *   - all arithmetic single precision.  The reference's one double-precision
 *     step (the low-band roll-off's phase, mbe_low_band_phase()) is
 *     approximated with a float atanf and its spectral-amplitude enhancement
 *     has a power-of-two normalisation instead of a double-precision
 *     recomputation; both are measured against the reference in
 *     tests/ (see docs/RX_DSTAR.md, "Voice");
 *   - the frame is processed in place, so a 20 ms frame uses no stack beyond
 *     a few scalars.
 *
 * The functions below follow the reference one for one, in the same order and
 * with the same expression shapes, because the port is verified bit for bit
 * against it.  Where a name would collide with the reference's, the reference
 * name is quoted in the comment.
 */
#include "dstar_decoder.h"

#include <string.h>

#include "dmr_fft.h"
#include "dmr_math.h"
#include "dstar_tables.h"

/* ------------------------------------------------------------------ */
/* Constants (mbelib-neo: mbe_adaptive.h, mbe_unvoiced_fft.h,          */
/* ambe3600x2400_internal.h, ambe_common.h, mbelib.h)                  */
/* ------------------------------------------------------------------ */

#define D_PI 3.14159265358979323846f
#define D_TWO_PI 6.28318530717958647692f
#define D_UNVOICED_SCALE 146.17696f
/* the reference's MBE_256_OVER_2PI is a double expression and it is
 * applied to w0 in double before rounding, which decides band edges */
#define D_256_OVER_2PI (256.0f / (2.0f * 3.14159265358979323846))
#define D_LCG_SEED 3147.0f
#define D_LCG_MODULUS 53125u
#define D_NOISE_OVERLAP 96
#define D_MUTING_AMBE 0.096f
#define D_MUTE_ERROR_RATE 0.042f
#define D_MAX_REPEATS 4
#define D_HIGH_BAND_GAIN 1.33352143f /* +2.5 dB, AMBE 3600x2400 */
#define D_RENDER_VOICED_GAIN 1.03514217f   /* +0.3 dB */
#define D_RENDER_UNVOICED_GAIN 0.95499259f /* -0.4 dB */
#define D_SPEECH_OUTPUT_GAIN (1.0f / 7.0f)
#define D_FLOAT_LIMIT ((32767.0f * 0.95f) / 7.0f)
#define D_INIT_W0 DSTAR_SILENCE_W0
#define D_INIT_L 15
#define D_PREDICTION_RHO 0.8f
#define D_GAIN_SCALE 2.0f
#define D_GAIN_OFFSET (-0.09f)

/* Adaptive smoothing (TIA-102.BABA 111-116) */
#define D_DEFAULT_LOCAL_ENERGY 75000.0f
#define D_MIN_LOCAL_ENERGY 10000.0f
#define D_ENERGY_SMOOTH_ALPHA 0.95f
#define D_ENERGY_SMOOTH_BETA 0.05f
#define D_DEFAULT_AMPLITUDE_THRESHOLD 20480
#define D_ERROR_THRESHOLD_ENTRY 0.0125f
#define D_ERROR_THRESHOLD_LOW 0.005f
#define D_ADAPTIVE_GAIN 45.255f
#define D_ADAPTIVE_EXPONENT 277.26f
#define D_ADAPTIVE_ALT 1.414f
#define D_AMPLITUDE_PENALTY_PER_ERROR 300
#define D_AMPLITUDE_BASE 6000

/* Phase regeneration (US 5,701,390 eqs. 7-9) */
#define D_PHASE_KERNEL_HALF_LEN 19
#define D_PHASE_KERNEL_SCALE 0.44f
#define D_PHASE_EXTRAP_SLOPE 0.72f
#define D_PHASE_MIN_MAGNITUDE 1e-6f

/* Low-band roll-off: a second-order high-pass at 100 Hz, as DVSI's decoders */
#define D_LOW_BAND_CORNER_HZ 100.0f

/* Tone index of an AMBE 3600x2400 frame: b0 = 0x7E, index in the c1 bits. */
static const uint8_t toneHighBits[8] = {4, 0, 1, 2, 3, 7, 6, 5};
static const uint8_t toneVolumeBit[8] = {12, 13, 14, 15, 16, 44, 45, 17};

/* D-STAR tone frequencies (mbe_tone_lookup_dstar_freqs) */
static const float toneRows[4] = {697.0f, 770.0f, 852.0f, 941.0f};
static const float toneColumns[4] = {1209.0f, 1336.0f, 1477.0f, 1633.0f};

/* ------------------------------------------------------------------ */
/* Small float helpers                                                */
/* ------------------------------------------------------------------ */

static inline int valid_l(int L) { return L >= 1 && L <= DSTAR_MAX_HARMONICS; }

static inline int clamp_l(int L)
{
    if (L < 1)
        return 1;
    if (L > DSTAR_MAX_HARMONICS)
        return DSTAR_MAX_HARMONICS;
    return L;
}

static inline float fmax0(float a, float b)
{
    /* what fmaxf() does here: the first argument unless it is NaN */
    if (a != a)
        return b;
    return a > b ? a : b;
}

static inline float fabsf_(float x) { return dmr_fabsf(x); }

/* floor(x) for |x| < 2^23, exact and branch-free in practice. */
static inline float floor_(float x)
{
    float t = (float)(int32_t)x;
    if (t > x)
        t -= 1.0f;
    return t;
}

/* floorf(x / y) * y subtracted from x, with y = 2 pi: the reference wraps each
 * phase accumulator with fmodf().  This is the same operation computed
 * exactly, by subtracting power-of-two multiples of y: |x| <= 3e4, so every
 * partial subtraction is a Sterbenz subtraction (t <= ax < 2t) and the
 * remainder is exact. */
static float wrap_two_pi(float x)
{
    const float y = D_TWO_PI;
    float ax = fabsf_(x);
    if (ax < y)
        return x;
    float t = y;
    while (t * 2.0f <= ax)
        t *= 2.0f;
    while (ax >= y) {
        if (ax >= t)
            ax -= t;
        t *= 0.5f;
    }
    return (x < 0.0f) ? -ax : ax;
}

/* atan(x) for |x| <= 1.7 (the whole reachable range, see the caller), 1 ulp.
 * Two range reductions keep the polynomial in [-tan(pi/8), tan(pi/8)]. */
static float atan_small(float x)
{
    /* fdlibm atanf's 11-term polynomial, |x| <= 7/16, ~1 ulp.  The odd
     * coefficients are negative: the series alternates. */
    static const float aT[11] = {
        3.3333334327e-01f,  -2.0000000298e-01f, 1.4285714924e-01f,
        -1.1111110449e-01f, 9.0908870101e-02f,  -7.6918758452e-02f,
        6.6610731184e-02f,  -5.8335704505e-02f, 4.9768779427e-02f,
        -3.6531571299e-02f, 1.6285819933e-02f,
    };
    const float z = x * x;
    const float w = z * z;
    const float s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    const float s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    return x - x * (s1 + s2);
}

static float atan_(float x)
{
    const float ax = fabsf_(x);
    const float sign = x < 0.0f ? -1.0f : 1.0f;
    float r;
    if (ax > 1.0f) {
        /* atan(a) = pi/2 - atan(1/a) */
        const float u = 1.0f / ax;
        if (u > 0.4375f) {
            const float v = (u - 1.0f) / (u + 1.0f);
            r = 0.7853981633974483f + atan_small(v);
        } else {
            r = atan_small(u);
        }
        return sign * (1.5707963267948966f - r);
    }
    if (ax > 0.4375f) {
        /* atan(a) = pi/4 + atan((a-1)/(a+1)) */
        const float v = (ax - 1.0f) / (ax + 1.0f);
        r = 0.7853981633974483f + atan_small(v);
    } else {
        r = atan_small(ax);
    }
    return sign * r;
}

/* The phase the low-band roll-off adds to a harmonic at hz, as the reference's
 * mbe_low_band_phase(): arg of H(s) = s^2 / (s^2 + sqrt2 s + 1) at
 * s = j hz / corner.  The reference evaluates it with a double atan2 for
 * reproducibility across C libraries; this port uses atan_() in float, which
 * moves the phase by at most a couple of ulp (2e-7 rad, three orders of
 * magnitude below one PCM step -- measured in tests/). */
static float low_band_phase(float hz)
{
    if (!(hz > 0.0f))
        return 0.0f;
    /* pi - atan2(sqrt2 x, 1 - x^2) with x = hz/100, scaled by 100^2 to stay
     * in float: theta = atan2(100 sqrt2 hz, 10000 - hz^2) + 0 ... the identity
     * atan2(y, X) with y > 0 equals pi/2 + atan(-X/y) is used to keep the
     * argument near unity. */
    const float x = hz * (1.0f / D_LOW_BAND_CORNER_HZ);
    const float y = 1.4142135623730951f * x;
    const float q = (1.0f - x * x) / y; /* 1/tan(atan2(y, 1-x^2)) */
    return 1.5707963267948966f + atan_(q);
}

/* The magnitude of the same filter. */
static float low_band_gain(float hz)
{
    if (!(hz > 0.0f))
        return 0.0f;
    const float hz2 = hz * hz;
    const float corner2 = D_LOW_BAND_CORNER_HZ * D_LOW_BAND_CORNER_HZ;
    return hz2 / dmr_sqrtf((hz2 * hz2) + (corner2 * corner2));
}

/* High-band shelf of the spectral amplitude enhancement (mbe_high_band_gain) */
static float high_band_gain(float hz, float gain)
{
    float t;
    if (hz <= 2500.0f || hz >= 3800.0f)
        return 1.0f;
    if (hz < 3000.0f)
        t = (hz - 2500.0f) / 500.0f;
    else if (hz <= 3600.0f)
        t = 1.0f;
    else
        t = (3800.0f - hz) / 200.0f;
    return 1.0f + ((gain - 1.0f) * t);
}

/* ------------------------------------------------------------------ */
/* Parameter state                                                    */
/* ------------------------------------------------------------------ */

/* mbe_initMbeParms() / mbe_initAmbeParms_common() */
static void parms_reset(dstar_parms_t *p)
{
    memset(p, 0, sizeof(*p));
    p->w0 = D_INIT_W0;
    p->L = D_INIT_L;
    for (int l = 0; l <= DSTAR_MAX_HARMONICS; l++) {
        p->Ml[l] = 1.0f;
        p->Vl[l] = 0;
        p->log2Ml[l] = 0.0f;
    }
    p->noiseSeed = -1.0f;
    p->localEnergy = D_DEFAULT_LOCAL_ENERGY;
    p->amplitudeThreshold = D_DEFAULT_AMPLITUDE_THRESHOLD;
    p->mutingThreshold = D_MUTING_AMBE;
}

static void parms_init(dstar_parms_t *cur, dstar_parms_t *prev,
                       dstar_parms_t *enh)
{
    parms_reset(cur);
    *prev = *cur;
    *enh = *cur;
}

/* mbe_setAmbeErasureParms_common() */
static void parms_erasure(dstar_parms_t *p, const dstar_parms_t *src)
{
    memcpy(p->PHIl, src->PHIl, sizeof(p->PHIl));
    memcpy(p->PSIl, src->PSIl, sizeof(p->PSIl));
    p->noiseSeed = src->noiseSeed;
    memcpy(p->noiseOverlap, src->noiseOverlap, sizeof(p->noiseOverlap));
    memcpy(p->previousUw, src->previousUw, sizeof(p->previousUw));
    p->w0 = 0.0f;
    p->L = 9;
    p->gamma = 0.0f;
    p->swn = 0;
    p->tonePhase = 0;
    for (int l = 0; l <= DSTAR_MAX_HARMONICS; l++) {
        p->Ml[l] = 1.0f;
        p->Vl[l] = 0;
        p->log2Ml[l] = 0.0f;
    }
    p->localEnergy = D_DEFAULT_LOCAL_ENERGY;
    p->amplitudeThreshold = D_DEFAULT_AMPLITUDE_THRESHOLD;
}

/* mbe_repeat_load_model(): the model of the frame to repeat, keeping this
 * frame's error accounting and generator state. */
static void parms_repeat_model(dstar_parms_t *cur, const dstar_parms_t *src)
{
    dstar_parms_t model = *src;

    model.errorRate = cur->errorRate;
    model.errorCountTotal = cur->errorCountTotal;
    model.errorCount4 = cur->errorCount4;
    model.repeatCount = cur->repeatCount;
    model.mutingThreshold = cur->mutingThreshold;
    model.swn = cur->swn;
    model.tonePhase = cur->tonePhase;
    model.noiseSeed = cur->noiseSeed;
    memcpy(model.noiseOverlap, cur->noiseOverlap, sizeof(model.noiseOverlap));
    *cur = model;
}

static int repeat_next_count(int prev)
{
    if (prev < 0)
        return 1;
    return (prev >= D_MAX_REPEATS) ? D_MAX_REPEATS : prev + 1;
}

/* ------------------------------------------------------------------ */
/* Tone frames                                                        */
/* ------------------------------------------------------------------ */

static int tone_index_of(const uint8_t *d)
{
    const int selector = (d[6] << 2) | (d[7] << 1) | d[8];
    return (toneHighBits[selector & 7] << 5) | (d[9] << 4) | (d[42] << 3) |
           (d[43] << 2) | (d[10] << 1) | d[11];
}

static int tone_volume_of(const uint8_t *d)
{
    int volume = 0;
    for (int i = 0; i < 8; i++)
        volume = (volume << 1) | (d[toneVolumeBit[i]] & 1);
    return volume;
}

static int tone_freqs(int id, float *f1, float *f2)
{
    *f1 = 0.0f;
    *f2 = 0.0f;
    if (id == 5) {
        *f1 = *f2 = 156.25f;
        return 1;
    }
    if (id == 6) {
        *f1 = *f2 = 187.5f;
        return 1;
    }
    if (id >= 7 && id <= 122) {
        *f1 = *f2 = 31.25f * (float)id;
        return 1;
    }
    if (id >= 128 && id <= 143) {
        *f1 = toneRows[(id - 128) & 3];
        *f2 = toneColumns[(id - 128) >> 2];
        return 1;
    }
    return 0;
}

/* D-STAR's tone set: 144..147 are AMBE+2's call-progress tones 160..163. */
static int tone_freqs_dstar(int id, float *f1, float *f2)
{
    if (id >= 128 && id <= 143) {
        *f1 = toneRows[(id - 128) & 3];
        *f2 = toneColumns[(id - 128) >> 2];
        return 1;
    }
    if (id >= 144 && id <= 147) {
        static const float cp[4][2] = {{440.0f, 350.0f}, {480.0f, 440.0f},
                                       {620.0f, 480.0f}, {490.0f, 350.0f}};
        *f1 = cp[id - 144][0];
        *f2 = cp[id - 144][1];
        return 1;
    }
    return (id <= 122) && tone_freqs(id, f1, f2);
}

static float clip_float(float x)
{
    /* mbe_clipFloatSample(): magnitude-clip to +-MBE_FLOAT_LIMIT, leaving NaN
     * and infinities alone (the PCM conversion turns NaN into 0). */
    const uint32_t bits = dmr_bitcast_u(x);
    const uint32_t mag = bits & 0x7FFFFFFFu;
    const uint32_t limit = dmr_bitcast_u(D_FLOAT_LIMIT);
    if (mag > limit && mag <= 0x7F800000u)
        return (bits & 0x80000000u) ? -D_FLOAT_LIMIT : D_FLOAT_LIMIT;
    return x;
}

/* mbe_renderTonef(): a 32-bit phase accumulator per component. */
static void render_tone(float *out, dstar_parms_t *mp, float f1, float f2,
                        float gain)
{
    if (!(f1 > 0.0f) || f1 != f1 || f2 != f2 || gain != gain) {
        memset(out, 0, DSTAR_FRAME_SAMPLES * sizeof(float));
        return;
    }
    /* frequency_increment(): round(f * 2^32 / 8000); every tone frequency is
     * below 8 kHz, so the reference's fmod is the identity. */
    const double k = 4294967296.0 / 8000.0;
    const uint32_t step1 = (uint32_t)(f1 * k + 0.5);
    const int dual = f2 > 0.0f && fabsf_(f2 - f1) > 1.0e-6f;
    const uint32_t step2 = dual ? (uint32_t)(f2 * k + 0.5) : 0u;
    uint32_t phase1 = mp->swn;
    uint32_t phase2 = mp->tonePhase;

    for (int n = 0; n < DSTAR_FRAME_SAMPLES; n++) {
        phase1 += step1;
        /* phase_sine(): sin(2 pi phase / 2^32 - pi/2), the angle formed in
         * double as the reference forms it */
        float value = gain * dmr_sinf((float)((double)phase1 *
                                              (2.0 * 3.14159265358979323846 / 4294967296.0) -
                                              1.57079632679489661923));
        if (dual) {
            phase2 += step2;
            value += gain * dmr_sinf((float)((double)phase2 *
                                             (2.0 * 3.14159265358979323846 / 4294967296.0) -
                                             1.57079632679489661923));
        }
        out[n] = clip_float(value);
    }
    mp->swn = phase1;
    mp->tonePhase = phase2;
}

/* ------------------------------------------------------------------ */
/* Comfort noise and the unvoiced noise generator                     */
/* ------------------------------------------------------------------ */

/* mbe_synthesizeUniformNoisef(): 48-bit LCG, state in the decoder
 * (the reference keeps it thread-local and seeds it once per process). */
static void uniform_noise(dstar_decoder_t *d, float *out, float amplitude)
{
    uint64_t s = d->comfortState;
    for (int i = 0; i < DSTAR_FRAME_SAMPLES; i++) {
        s = ((s * 0x5deece66dULL) + 11u) & 0xffffffffffffULL;
        const float unit = (float)(s >> 24) * (1.0f / 16777216.0f);
        out[i] = ((unit * 2.0f) - 1.0f) * amplitude;
    }
    d->comfortState = s;
}

static void comfort_noise(dstar_decoder_t *d, float *out)
{
    uniform_noise(d, out, (0.003f * 32767.0f) / 7.0f);
}

static uint32_t usable_noise_seed(float seed)
{
    const uint32_t bits = dmr_bitcast_u(seed);
    if (bits == 0x80000000u)
        return 0;
    if (bits > 0x4f7fffffu)
        return (uint32_t)D_LCG_SEED;
    return (uint32_t)seed % D_LCG_MODULUS;
}

/* mbe_generate_noise_lcg() */
static void noise_lcg(float *buf, int count, float *seed)
{
    uint32_t state = usable_noise_seed(*seed);
    for (int i = 0; i < count; i++) {
        buf[i] = (float)state;
        state = (171u * state + 11213u) % D_LCG_MODULUS;
    }
    *seed = (float)state;
}

/* mbe_generate_noise_with_overlap() */
static void noise_with_overlap(dstar_parms_t *cur, float *buf)
{
    if (cur->noiseSeed < 0.0f) {
        memset(buf, 0, DSTAR_FFT_SIZE * sizeof(float));
        memset(cur->noiseOverlap, 0, sizeof(cur->noiseOverlap));
        cur->noiseSeed = D_LCG_SEED; /* the reference's pending seed is unset */
        return;
    }
    memmove(buf, cur->noiseOverlap, D_NOISE_OVERLAP * sizeof(float));
    noise_lcg(buf + D_NOISE_OVERLAP, DSTAR_FFT_SIZE - D_NOISE_OVERLAP,
              &cur->noiseSeed);
    memmove(cur->noiseOverlap, buf + DSTAR_FFT_SIZE - D_NOISE_OVERLAP,
            D_NOISE_OVERLAP * sizeof(float));
}

/* ------------------------------------------------------------------ */
/* AMBE 2400 parameter decode                                         */
/* ------------------------------------------------------------------ */

/* The frame classes the reference's mbe_decodeAmbe2400Parms() returns. */
#define BAD_VOICE 0
#define BAD_ERASURE 2
#define BAD_TONE_FRAME 3 /* a tone the decoder does not play */

static int is_tone_class(int bad)
{
    return (bad >= 5 && bad <= 122) || (bad >= 128 && bad <= 147);
}

static int decode_parms(dstar_decoder_t *dec, const uint8_t *d,
                        dstar_parms_t *cur, dstar_parms_t *prev)
{
    int b0 = (d[0] << 6) | (d[1] << 5) | (d[2] << 4) | (d[3] << 3) |
             (d[4] << 2) | (d[5] << 1) | d[48];
    int b1, b2, b3, b4, b5, b6, b7, b8;
    float f0, unvc, rconst, sum;
    float Cik[5][18], Gm[9], Ri[9];
    int Ji[5], L, l, i, m, j, k, ji, am, ak;
    int16_t *intkl = dec->intkl;
    float *flokl = dec->flokl, *deltal = dec->deltal, *Tl = dec->Tl;

    memset(Tl, 0, 57 * sizeof(float));

    /* Tone and silence frames (mbe_handle_tone_frame). */
    if ((b0 & 0x7E) == 0x7E) {
        const int index = tone_index_of(d);
        if (index >= 5 && index <= 122)
            return index;
        if (index >= 128 && tone_volume_of(d) != 0) {
            float f1, f2;
            if (tone_freqs_dstar(index, &f1, &f2))
                return index;
        }
        if (!(index >= 128 && index <= 163)) {
            /* the silence frame: an all-unvoiced model at the silence pitch */
            cur->w0 = DSTAR_SILENCE_W0;
            L = DSTAR_SILENCE_L;
            cur->L = DSTAR_SILENCE_L;
            for (l = 1; l <= L; l++)
                cur->Vl[l] = 0;
        }
        return BAD_TONE_FRAME;
    }

    f0 = dstar_f0[b0];
    cur->w0 = dstar_w0[b0];
    L = dstar_l[b0];
    cur->L = (int16_t)L;
    unvc = 0.2046f / dmr_sqrtf(cur->w0);

    /* V/UV decisions: 4-bit code, one bit per 2 harmonics */
    b1 = (d[38] << 3) | (d[39] << 2) | (d[40] << 1) | d[41];
    for (l = 1; l <= L; l++) {
        const int jl = (int)((float)l * (float)16.0 * f0);
        cur->Vl[l] = dstar_vuv[b1][jl];
    }

    /* gain: absolute, not differential (AMBE 2400) */
    b2 = (d[6] << 5) | (d[7] << 4) | (d[8] << 3) | (d[9] << 2) |
         (d[42] << 1) | d[43];
    cur->gamma = (D_GAIN_SCALE * dstar_dg[b2]) + D_GAIN_OFFSET;

    /* PRBA -> Gm[2..8], Gm[1] = 0 */
    Gm[1] = 0.0f;
    b3 = (d[10] << 8) | (d[11] << 7) | (d[12] << 6) | (d[13] << 5) |
         (d[14] << 4) | (d[15] << 3) | (d[16] << 2) | (d[44] << 1) | d[45];
    Gm[2] = dstar_prba24[b3 * 3 + 0];
    Gm[3] = dstar_prba24[b3 * 3 + 1];
    Gm[4] = dstar_prba24[b3 * 3 + 2];
    b4 = (d[17] << 6) | (d[18] << 5) | (d[19] << 4) | (d[20] << 3) |
         (d[21] << 2) | (d[46] << 1) | d[47];
    Gm[5] = dstar_prba58[b4 * 4 + 0];
    Gm[6] = dstar_prba58[b4 * 4 + 1];
    Gm[7] = dstar_prba58[b4 * 4 + 2];
    Gm[8] = dstar_prba58[b4 * 4 + 3];

    /* Ri: inverse DCT of the Gm block with the reference's cosine table */
    for (i = 1; i <= 8; i++) {
        sum = 0.0f;
        for (m = 1; m <= 8; m++) {
            am = (m == 1) ? 1 : 2;
            sum = sum + ((float)am * Gm[m] * dstar_ri_cos[m * 9 + i]);
        }
        Ri[i] = sum;
    }

    rconst = (float)(1.0 / (2.0 * 1.41421356237309504880));
    Cik[1][1] = (float)0.5 * (Ri[1] + Ri[2]);
    Cik[1][2] = rconst * (Ri[1] - Ri[2]);
    Cik[2][1] = (float)0.5 * (Ri[3] + Ri[4]);
    Cik[2][2] = rconst * (Ri[3] - Ri[4]);
    Cik[3][1] = (float)0.5 * (Ri[5] + Ri[6]);
    Cik[3][2] = rconst * (Ri[5] - Ri[6]);
    Cik[4][1] = (float)0.5 * (Ri[7] + Ri[8]);
    Cik[4][2] = rconst * (Ri[7] - Ri[8]);

    /* higher order coefficients */
    b5 = (d[22] << 3) | (d[23] << 2) | (d[25] << 1) | d[26];
    b6 = (d[27] << 3) | (d[28] << 2) | (d[29] << 1) | d[30];
    b7 = (d[31] << 3) | (d[32] << 2) | (d[33] << 1) | d[34];
    b8 = (d[35] << 3) | (d[36] << 2) | (d[37] << 1);

    Ji[1] = dstar_lmprbl[L][0];
    Ji[2] = dstar_lmprbl[L][1];
    Ji[3] = dstar_lmprbl[L][2];
    Ji[4] = dstar_lmprbl[L][3];

    for (k = 3; k <= Ji[1]; k++)
        Cik[1][k] = (k > 6) ? 0.0f : dstar_hoc_b5[b5 * 4 + (k - 3)];
    for (k = 3; k <= Ji[2]; k++)
        Cik[2][k] = (k > 6) ? 0.0f : dstar_hoc_b6[b6 * 4 + (k - 3)];
    for (k = 3; k <= Ji[3]; k++)
        Cik[3][k] = (k > 6) ? 0.0f : dstar_hoc_b7[b7 * 4 + (k - 3)];
    for (k = 3; k <= Ji[4]; k++)
        Cik[4][k] = (k > 6) ? 0.0f : dstar_hoc_b8[b8 * 4 + (k - 3)];

    /* per-block inverse DCT -> Tl[] */
    l = 1;
    for (i = 1; i <= 4; i++) {
        ji = Ji[i];
        for (j = 1; j <= ji; j++) {
            const float *row = &dstar_idct_cos[dstar_idct_off[ji] + (j - 1) * ji];
            sum = 0.0f;
            for (k = 1; k <= ji; k++) {
                ak = (k == 1) ? 1 : 2;
                sum = sum + ((float)ak * Cik[i][k] * row[k - 1]);
            }
            Tl[l] = sum;
            l++;
        }
    }

    /* spectral amplitude prediction from the previous frame's log spectrum */
    {
        const int prevL = clamp_l(prev->L);
        cur->L = (int16_t)clamp_l(cur->L);
        if (cur->L > prevL) {
            for (l = prevL + 1; l <= cur->L; l++) {
                prev->Ml[l] = prev->Ml[prevL];
                prev->log2Ml[l] = prev->log2Ml[prevL];
            }
        }
        prev->log2Ml[0] = prev->log2Ml[1];
        prev->Ml[0] = prev->Ml[1];

        float Sum43 = 0.0f;
        for (l = 1; l <= cur->L; l++) {
            flokl[l] = ((float)prevL / (float)cur->L) * (float)l;
            intkl[l] = (int)flokl[l];
            deltal[l] = flokl[l] - (float)intkl[l];
            int upper = intkl[l] + 1;
            if (upper > DSTAR_MAX_HARMONICS)
                upper = DSTAR_MAX_HARMONICS;
            Sum43 = Sum43 + ((((float)1 - deltal[l]) * prev->log2Ml[intkl[l]]) +
                             (deltal[l] * prev->log2Ml[upper]));
        }
        Sum43 = ((D_PREDICTION_RHO / (float)cur->L) * Sum43);

        float Sum42 = 0.0f;
        for (l = 1; l <= cur->L; l++)
            Sum42 += Tl[l];
        Sum42 = Sum42 / (float)cur->L;
        const float BigGamma =
            cur->gamma - (0.5f * dstar_log2_l[cur->L]) - Sum42;

        for (l = 1; l <= cur->L; l++) {
            int upper = intkl[l] + 1;
            if (upper > DSTAR_MAX_HARMONICS)
                upper = DSTAR_MAX_HARMONICS;
            const float c1 = D_PREDICTION_RHO * ((float)1 - deltal[l]) *
                             prev->log2Ml[intkl[l]];
            const float c2 =
                D_PREDICTION_RHO * deltal[l] * prev->log2Ml[upper];
            cur->log2Ml[l] = Tl[l] + c1 + c2 - Sum43 + BigGamma;
            if (cur->Vl[l] == 1)
                cur->Ml[l] = dmr_exp2f(cur->log2Ml[l]);
            else
                cur->Ml[l] = unvc * dmr_exp2f(cur->log2Ml[l]);
        }
    }
    return BAD_VOICE;
}

/* ------------------------------------------------------------------ */
/* Spectral amplitude enhancement (TIA-102.BABA 128-133)              */
/* ------------------------------------------------------------------ */

/* Harmonic cosines of w0 (mbe_precompute_harmonic_cosines) */
static void harmonic_cosines(float *cosTab, int L, float w0)
{
    float s_step, c_step;
    dmr_sincosf(w0, &s_step, &c_step);
    float c = 1.0f, s = 0.0f;
    for (int l = 1; l <= L; l++) {
        const float cn = (c * c_step) - (s * s_step);
        const float sn = (s * c_step) + (c * s_step);
        c = cn;
        s = sn;
        cosTab[l] = c;
    }
}

/* mbe_spectralAmpEnhanceWithRm0(): the spectral amplitude enhancement of
 * TIA-102.BABA 128-133, in the reference's own expression shapes -- the
 * weighted amplitude is the closed form with Rm0^2 - Rm1^2 in the
 * denominator, which is what mbelib-neo v2.6.2 evaluates (a later revision
 * rewrites it in a scale-invariant form).  Returns Rm0, the pre-enhancement
 * energy the synthesis core's adaptive smoothing needs. */
static float spectral_enhance(dstar_decoder_t *d, dstar_parms_t *cur)
{
    const int L = cur->L;
    float Rm0 = 0.0f, Rm1 = 0.0f, sum, gamma;

    if (!valid_l(L))
        return 0.0f;

    harmonic_cosines(d->cosTab, L, cur->w0);
    for (int l = 1; l <= L; l++) {
        const float m2 = cur->Ml[l] * cur->Ml[l];
        Rm0 += m2;
        Rm1 += m2 * d->cosTab[l];
    }

    {
        const float R2m0 = Rm0 * Rm0;
        const float R2m1 = Rm1 * Rm1;
        for (int l = 1; l <= L; l++) {
            if (cur->Ml[l] != 0.0f) {
                const float cosW0l = d->cosTab[l];
                const float Wl =
                    dmr_sqrtf(cur->Ml[l]) *
                    dmr_sqrtf(dmr_sqrtf((((float)0.96 * D_PI) *
                                         ((R2m0 + R2m1) -
                                          ((float)2 * Rm0 * Rm1 * cosW0l))) /
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
    }

    sum = 0.0f;
    for (int l = 1; l <= L; l++) {
        float M = cur->Ml[l];
        if (M < 0.0f)
            M = -M;
        sum += M * M;
    }
    gamma = (sum == 0.0f) ? 1.0f : dmr_sqrtf(Rm0 / sum);
    {
        const float hzPerHarmonic = cur->w0 * (8000.0f / (2.0f * D_PI));
        for (int l = 1; l <= L; l++) {
            cur->Ml[l] = gamma * cur->Ml[l];
            cur->Ml[l] *= high_band_gain(hzPerHarmonic * (float)l,
                                         D_HIGH_BAND_GAIN);
        }
    }
    return Rm0;
}

/* ------------------------------------------------------------------ */
/* Adaptive smoothing (TIA-102.BABA 111-116)                          */
/* ------------------------------------------------------------------ */

static void adaptive_smoothing(dstar_parms_t *cur, const dstar_parms_t *prev,
                               float rm0)
{
    if (!valid_l(cur->L) || !valid_l(prev->L))
        return;

    float prior = prev->localEnergy;
    const int priorThreshold = prev->amplitudeThreshold;
    if (prior < D_MIN_LOCAL_ENERGY)
        prior = D_DEFAULT_LOCAL_ENERGY;
    float energy = D_ENERGY_SMOOTH_ALPHA * prior + D_ENERGY_SMOOTH_BETA * rm0;
    if (energy < D_MIN_LOCAL_ENERGY)
        energy = D_MIN_LOCAL_ENERGY;
    cur->localEnergy = energy;

    /* voicing threshold, eq. 112 */
    float threshold;
    if (cur->errorRate <= D_ERROR_THRESHOLD_LOW && cur->errorCountTotal <= 4) {
        threshold = dmr_bitcast_f(0x7F7FFFFFu); /* FLT_MAX */
    } else {
        const float root =
            dmr_sqrtf(dmr_sqrtf(dmr_sqrtf(cur->localEnergy)));
        const float energyPower = (root * root) * root;
        if (cur->errorRate <= D_ERROR_THRESHOLD_ENTRY && cur->errorCount4 == 0)
            threshold = (D_ADAPTIVE_GAIN * energyPower) /
                        dmr_expf(D_ADAPTIVE_EXPONENT * cur->errorRate);
        else
            threshold = D_ADAPTIVE_ALT * energyPower;
    }

    float amplitudeSum = 0.0f;
    for (int l = 1; l <= cur->L; l++) {
        if (cur->Ml[l] > threshold)
            cur->Vl[l] = 1;
        amplitudeSum += cur->Ml[l];
    }

    int amplitudeThreshold;
    if (cur->errorRate <= D_ERROR_THRESHOLD_LOW && cur->errorCountTotal <= 6) {
        amplitudeThreshold = D_DEFAULT_AMPLITUDE_THRESHOLD;
    } else {
        int p = priorThreshold;
        if (p <= 0)
            p = D_DEFAULT_AMPLITUDE_THRESHOLD;
        amplitudeThreshold = p + D_AMPLITUDE_BASE -
                             (D_AMPLITUDE_PENALTY_PER_ERROR * cur->errorCountTotal);
    }
    cur->amplitudeThreshold = amplitudeThreshold;

    float scale = 1.0f;
    if (amplitudeSum > 0.0f && (float)amplitudeThreshold <= amplitudeSum)
        scale = (float)amplitudeThreshold / amplitudeSum;
    if (scale < 0.0f)
        scale = 0.0f;
    for (int l = 1; l <= cur->L; l++)
        cur->Ml[l] *= scale;
}

/* ------------------------------------------------------------------ */
/* Phase regeneration and rendering                                   */
/* ------------------------------------------------------------------ */

/* mbe_regenerate_harmonic_phases(): odd 1/m edge kernel over log2(Ml) */
static void regenerate_phases(dstar_decoder_t *dec, const dstar_parms_t *cur,
                              float *phi)
{
    const int D = D_PHASE_KERNEL_HALF_LEN;
    const int L = cur->L;
    static const float invOdd[10] = {
        1.0f,        1.0f / 3,  1.0f / 5,  1.0f / 7,  1.0f / 9,
        1.0f / 11,   1.0f / 13, 1.0f / 15, 1.0f / 17, 1.0f / 19,
    };
    float *B = dec->phaseB;

    B[D] = 0.0f;
    for (int l = 1; l <= L; l++)
        B[D + l] = dmr_log2f(fmax0(cur->Ml[l], D_PHASE_MIN_MAGNITUDE));
    for (int l = L + 1; l <= 56 + D; l++)
        B[D + l] = B[D + L] - D_PHASE_EXTRAP_SLOPE * (float)(l - L);
    for (int l = 1; l <= D; l++)
        B[D - l] = B[D + l];

    for (int l = 1; l <= 56; l++) {
        float acc = 0.0f;
        for (int k = 0; k < 10; k++) {
            const int m = 2 * k + 1;
            acc += (B[D + l + m] - B[D + l - m]) * invOdd[k];
        }
        phi[l] = D_PHASE_KERNEL_SCALE * acc;
    }
}

/* mbe_update_speech_phases(): pulse position plus the regenerated envelope
 * phase, with the previous accumulator wrapped into [0, 2 pi). */
static void update_phases(dstar_decoder_t *dec, dstar_parms_t *cur,
                          dstar_parms_t *prev, int N)
{
    const float cw0 = cur->w0;
    const float pw0 = prev->w0;
    float *phi = dec->phi;

    regenerate_phases(dec, cur, phi);
    for (int l = 1; l <= DSTAR_MAX_HARMONICS; l++) {
        float wrapped = wrap_two_pi(prev->PSIl[l]);
        if (wrapped < 0.0f)
            wrapped += D_TWO_PI;
        prev->PSIl[l] = wrapped;
        cur->PSIl[l] = wrapped + ((pw0 + cw0) * ((float)(l * N) / 2.0f));
        cur->PHIl[l] = cur->PSIl[l] + phi[l];
    }
}

/* mbe_render_prepare(): the amplitude and phase the output actually renders,
 * with the low-band roll-off and the voicing gains.  Keeps the decoded model
 * in *savedMl / *savedPHIl for mbe_render_restore(). */
static void render_prepare(dstar_parms_t *mp, float *savedMl, float *savedPHIl,
                           int maxl)
{
    const float hzPerHarmonic = mp->w0 * (8000.0f / (2.0f * D_PI));
    for (int l = 1; l <= DSTAR_MAX_HARMONICS; l++) {
        const float hz = hzPerHarmonic * (float)l;
        const int voiced = (l <= mp->L) && (mp->Vl[l] == 1);
        savedMl[l] = mp->Ml[l];
        savedPHIl[l] = mp->PHIl[l];
        mp->Ml[l] *= low_band_gain(hz) *
                     (voiced ? D_RENDER_VOICED_GAIN : D_RENDER_UNVOICED_GAIN);
        if (l <= maxl && mp->Vl[l] == 1)
            mp->PHIl[l] += low_band_phase(hz);
    }
}

static void render_restore(dstar_parms_t *mp, const float *savedMl,
                           const float *savedPHIl)
{
    for (int l = 1; l <= DSTAR_MAX_HARMONICS; l++) {
        mp->Ml[l] = savedMl[l];
        mp->PHIl[l] = savedPHIl[l];
    }
}

/* Add four samples of a windowed cosine oscillator, advanced by a complex
 * rotation (mbe_fill_voiced_cos_block4 / mbe_add_voiced_block4). */
static void add_voiced_block4(float *Ss, const float *W, float gain, float *c,
                              float *s, float sd, float cd)
{
    float cblk[4];
    float cc = *c, ss = *s;
    for (int k = 0; k < 4; k++) {
        cblk[k] = cc;
        const float cpn = (cc * cd) - (ss * sd);
        const float spn = (ss * cd) + (cc * sd);
        cc = cpn;
        ss = spn;
    }
    *c = cc;
    *s = ss;
    Ss[0] += gain * W[0] * cblk[0];
    Ss[1] += gain * W[1] * cblk[1];
    Ss[2] += gain * W[2] * cblk[2];
    Ss[3] += gain * W[3] * cblk[3];
}

/* mbe_render_voiced_windowed() */
static void render_voiced_windowed(float *out, const dstar_parms_t *cur,
                                   const dstar_parms_t *prev, int l, int N,
                                   float cw0l, float pw0l, int curVoiced,
                                   int prevVoiced)
{
    float *Ss = out;

    if (prevVoiced && curVoiced) {
        const float gainPrev = 2.0f * prev->Ml[l];
        float sdPrev, cdPrev, sPrev, cPrev;
        dmr_sincosf(pw0l, &sdPrev, &cdPrev);
        dmr_sincosf(prev->PHIl[l], &sPrev, &cPrev);
        const float gainCur = 2.0f * cur->Ml[l];
        float sdCur, cdCur, sCur, cCur;
        dmr_sincosf(cw0l, &sdCur, &cdCur);
        dmr_sincosf(cur->PHIl[l] - (cw0l * (float)N), &sCur, &cCur);

        for (int n = 0; n < N; n += 4) {
            add_voiced_block4(Ss, dstar_ws + n + N, gainPrev, &cPrev, &sPrev,
                              sdPrev, cdPrev);
            add_voiced_block4(Ss, dstar_ws + n, gainCur, &cCur, &sCur, sdCur,
                              cdCur);
            Ss += 4;
        }
    } else if (prevVoiced) {
        const float gainPrev = 2.0f * prev->Ml[l];
        float sdPrev, cdPrev, sPrev, cPrev;
        dmr_sincosf(pw0l, &sdPrev, &cdPrev);
        dmr_sincosf(prev->PHIl[l], &sPrev, &cPrev);
        for (int n = 0; n < N; n += 4) {
            add_voiced_block4(Ss, dstar_ws + n + N, gainPrev, &cPrev, &sPrev,
                              sdPrev, cdPrev);
            Ss += 4;
        }
    } else if (curVoiced) {
        const float gainCur = 2.0f * cur->Ml[l];
        float sdCur, cdCur, sCur, cCur;
        dmr_sincosf(cw0l, &sdCur, &cdCur);
        dmr_sincosf(cur->PHIl[l] - (cw0l * (float)N), &sCur, &cCur);
        for (int n = 0; n < N; n += 4) {
            add_voiced_block4(Ss, dstar_ws + n, gainCur, &cCur, &sCur, sdCur,
                              cdCur);
            Ss += 4;
        }
    }
}

/* mbe_render_voiced_interpolated(): harmonics 1..7 at steady pitch */
static void render_voiced_interpolated(float *out, const dstar_parms_t *cur,
                                       const dstar_parms_t *prev, int l, int N,
                                       float cw0, float pw0, float pw0l)
{
    const float deltaphil = cur->PHIl[l] - prev->PHIl[l] -
                            (((pw0 + cw0) * (float)(l * N)) / 2.0f);
    const float deltawl =
        (1.0f / (float)N) *
        (deltaphil - (D_TWO_PI * floor_((deltaphil + D_PI) / D_TWO_PI)));
    const float phi0 = prev->PHIl[l];
    const float mPrev = prev->Ml[l];
    const float ampd = cur->Ml[l] - mPrev;
    const float wstep = pw0l + deltawl;
    for (int n = 0; n < N; n++) {
        /* the reference's expression, term for term: the quadratic term is
         * formed as (cw0 - pw0) * (l*n*n) / (2N), not folded */
        const float thetaln =
            phi0 + (wstep * (float)n) +
            (((cw0 - pw0) * (float)(l * n * n)) / (float)(2 * N));
        const float aln = mPrev + (((float)n / (float)N) * ampd);
        out[n] += 2.0f * aln * dmr_cosf(thetaln);
    }
}

/* mbe_reconcile_speech_model_lengths(): eq. 128 and 129 */
static int reconcile_lengths(dstar_parms_t *cur, dstar_parms_t *prev)
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

static void render_voiced_speech(float *out, const dstar_parms_t *cur,
                                 const dstar_parms_t *prev, int maxl, int N)
{
    const float cw0 = cur->w0;
    const float pw0 = prev->w0;

    for (int l = 1; l <= maxl; l++) {
        const float cw0l = cw0 * (float)l;
        const float pw0l = pw0 * (float)l;
        const int curVoiced = (cur->Vl[l] == 1) && (cw0l < D_PI);
        const int prevVoiced = (prev->Vl[l] == 1) && (pw0l < D_PI);
        if (!curVoiced && !prevVoiced)
            continue;
        const int interp = (l < 8) && curVoiced && prevVoiced &&
                           (fabsf_(cw0 - pw0) < (0.1f * cw0));
        if (interp)
            render_voiced_interpolated(out, cur, prev, l, N, cw0, pw0, pw0l);
        else
            render_voiced_windowed(out, cur, prev, l, N, cw0l, pw0l, curVoiced,
                                   prevVoiced);
    }
}

/* ---------------------------------------------------------------- */
/* Unvoiced synthesis (TIA-102.BABA 117-126)                        */
/* ---------------------------------------------------------------- */

/* mbe_synthesisWindow(): 1.0 in the flat part, a 50-sample linear taper */
static float synthesis_window(int n)
{
    static const float taper[51] = {
        0.00f, 0.02f, 0.04f, 0.06f, 0.08f, 0.10f, 0.12f, 0.14f, 0.16f, 0.18f,
        0.20f, 0.22f, 0.24f, 0.26f, 0.28f, 0.30f, 0.32f, 0.34f, 0.36f, 0.38f,
        0.40f, 0.42f, 0.44f, 0.46f, 0.48f, 0.50f, 0.52f, 0.54f, 0.56f, 0.58f,
        0.60f, 0.62f, 0.64f, 0.66f, 0.68f, 0.70f, 0.72f, 0.74f, 0.76f, 0.78f,
        0.80f, 0.82f, 0.84f, 0.86f, 0.88f, 0.90f, 0.92f, 0.94f, 0.96f, 0.98f,
        1.00f,
    };
    if (n <= -105 || n >= 105)
        return 0.0f;
    if (n < 0)
        n = -n;
    if (n <= 55)
        return 1.0f;
    return taper[105 - n];
}

/* mbe_wola_combine(): overlap-add of the last two unvoiced frames */
static void wola_combine(float *out, const float *prevUw, const float *currUw,
                         int N)
{
    for (int n = 0; n < N; n++) {
        const float previousWindow = synthesis_window(n);
        const float currentWindow = synthesis_window(n - N);
        const float weight =
            previousWindow * previousWindow + currentWindow * currentWindow;
        float sample = 0.0f;
        if (previousWindow > 0.0f)
            sample += previousWindow * prevUw[128 + n];
        if (currentWindow > 0.0f)
            sample += currentWindow * currUw[128 + n - N];
        if (weight > 0.0f)
            out[n] += sample / weight;
    }
}

static int band_edge(float frequency)
{
    if (frequency >= 128.0f)
        return 128;
    return dmr_iceil(frequency);
}

/* mbe_synthesizeUnvoicedFFTWithNoise(), on the re/im arrays of dmr_fft.c:
 * the reference works on pffft's interleaved "ordered" layout, where
 * spectrum[2k], spectrum[2k+1] is bin k for k >= 1 and bin 0's imaginary part
 * is not stored (so it counts as 0). */
static void synthesize_unvoiced(dstar_decoder_t *d, float *out,
                                dstar_parms_t *cur, const dstar_parms_t *prev)
{
    const int L = cur->L;
    const int half = DSTAR_FFT_SIZE / 2;

    if (!valid_l(L))
        return;

    {
        float *time = d->scratch;
        for (int i = 0; i < DSTAR_FFT_SIZE; i++)
            time[i] = d->noise[i] * synthesis_window(i - DSTAR_FFT_SIZE / 2);
        dmr_rfft256(time, d->fftRe, d->fftIm);
    }

    /* shape each unvoiced harmonic's band of the noise spectrum */
    memset(d->binScale, 0, sizeof(d->binScale));
    {
        const float spacing = (float)((double)D_256_OVER_2PI * (double)cur->w0);
        for (int l = 1; l <= L; l++) {
            if (cur->Vl[l] != 0)
                continue;
            const int lower = band_edge(((float)l - 0.5f) * spacing);
            const int upper = band_edge(((float)l + 0.5f) * spacing);
            float power = 0.0f;
            for (int k = lower; k < upper; k++)
                power += (d->fftRe[k] * d->fftRe[k]) + (d->fftIm[k] * d->fftIm[k]);
            if (upper <= lower || power <= 1.0e-10f)
                continue;
            /* one scale per bin, so the loop below is the reference's
             * "shaped[k] = spectrum[k] * scale" */
            const float scale =
                (D_UNVOICED_SCALE * cur->Ml[l]) /
                dmr_sqrtf(power / (float)(upper - lower));
            for (int k = lower; k < upper; k++)
                d->binScale[k] = scale;
        }
    }

    {
        float *uw = d->scratch;
        for (int k = 0; k <= half; k++) {
            d->fftRe[k] *= d->binScale[k];
            d->fftIm[k] *= d->binScale[k];
        }
        d->fftRe[half] = 0.0f; /* the reference never writes the Nyquist bin */
        d->fftIm[half] = 0.0f;
        dmr_irfft256(d->fftRe, d->fftIm, uw);
        const float sc = 1.0f / (float)DSTAR_FFT_SIZE;
        for (int i = 0; i < DSTAR_FFT_SIZE; i++)
            uw[i] *= sc;

        wola_combine(out, prev->previousUw, uw, DSTAR_FRAME_SAMPLES);
        memcpy(cur->previousUw, uw, sizeof(cur->previousUw));
    }
}

/* ------------------------------------------------------------------ */
/* Speech synthesis core (mbe_synthesizeSpeechCore)                   */
/* ------------------------------------------------------------------ */

static void synthesize_speech(dstar_decoder_t *d, float *out,
                              dstar_parms_t *cur, dstar_parms_t *prev,
                              float preEnhRm0)
{
    const int N = DSTAR_FRAME_SAMPLES;

    if (!valid_l(cur->L) || !valid_l(prev->L)) {
        memset(out, 0, N * sizeof(float));
        return;
    }

    adaptive_smoothing(cur, prev, preEnhRm0);

    /* AMBE only mutes on the repeat limit here (the IMBE error-rate threshold
     * never applies: mutingThreshold stays at the AMBE value). */
    if (cur->repeatCount >= D_MAX_REPEATS) {
        comfort_noise(d, out);
        return;
    }

    noise_with_overlap(cur, d->noise);

    memset(out, 0, N * sizeof(float));
    const int maxl = reconcile_lengths(cur, prev);
    update_phases(d, cur, prev, N);

    render_prepare(cur, d->savedMl, d->savedPHIl, maxl);
    const int shared = (prev == cur);
    if (!shared)
        render_prepare(prev, d->savedMlPrev, d->savedPHIlPrev, maxl);

    render_voiced_speech(out, cur, prev, maxl, N);
    synthesize_unvoiced(d, out, cur, prev);

    render_restore(cur, d->savedMl, d->savedPHIl);
    if (!shared)
        render_restore(prev, d->savedMlPrev, d->savedPHIlPrev);

    for (int i = 0; i < N; i++)
        out[i] *= D_SPEECH_OUTPUT_GAIN;
    for (int i = 0; i < N; i++)
        out[i] = clip_float(out[i]);
}

/* ------------------------------------------------------------------ */
/* Frame state machine (mbe_processAmbe2400Dataf)                     */
/* ------------------------------------------------------------------ */

static void synthesize_voice(dstar_decoder_t *d, float *out, dstar_parms_t *cur,
                             dstar_parms_t *prev, dstar_parms_t *enh)
{
    if (cur->repeatCount < D_MAX_REPEATS) {
        *prev = *cur;
        const float rm0 = spectral_enhance(d, cur);
        synthesize_speech(d, out, cur, enh, rm0);
        *enh = *cur;
        if (cur->errorRate > D_MUTE_ERROR_RATE) {
            comfort_noise(d, out);
            d->mutedFrames++;
        }
        return;
    }

    /* The repeat limit restarts the model; the error rate keeps accumulating. */
    const float errorRate = cur->errorRate;
    comfort_noise(d, out);
    parms_init(cur, prev, enh);
    cur->errorRate = errorRate;
    prev->errorRate = errorRate;
    enh->errorRate = errorRate;
    d->mutedFrames++;
}

static void synthesize_frame(dstar_decoder_t *d, float *out, const uint8_t *p,
                             int bad, int c0Errors, int totalErrors,
                             dstar_parms_t *cur, dstar_parms_t *prev,
                             dstar_parms_t *enh)
{
    const int acceptedTone = is_tone_class(bad) && c0Errors < 2 && totalErrors < 3;

    if (acceptedTone) {
        float f1, f2;
        if (tone_freqs_dstar(bad, &f1, &f2))
            render_tone(out, cur, f1, f2, dstar_tone_amp[tone_volume_of(p)]);
        else
            memset(out, 0, DSTAR_FRAME_SAMPLES * sizeof(float));
        *prev = *cur;
        d->toneFrames++;
        return;
    }
    if (bad == BAD_VOICE) {
        synthesize_voice(d, out, cur, prev, enh);
        return;
    }
    if (bad == BAD_ERASURE) {
        comfort_noise(d, out);
        *prev = *cur;
        *enh = *cur;
        d->erasures++;
        return;
    }
    comfort_noise(d, out);
    parms_init(cur, prev, enh);
}

/* float PCM -> int16, as the reference's mbe_floattoshort(): 7x scale, clip at
 * 95 % of full scale, truncate toward zero, NaN to 0. */
static void pcm_convert(const float *in, int16_t *out)
{
    for (int i = 0; i < DSTAR_FRAME_SAMPLES; i++) {
        float v = in[i] * 7.0f;
        if (v != v)
            v = 0.0f;
        else if (v > (32767.0f * 0.95f))
            v = 32767.0f * 0.95f;
        else if (v < -(32767.0f * 0.95f))
            v = -(32767.0f * 0.95f);
        out[i] = (int16_t)v;
    }
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                */
/* ------------------------------------------------------------------ */

void dstar_decoder_init(dstar_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
    parms_init(&d->cur, &d->prev, &d->enh);
    /* mbe_synthesizeComfortNoisef()'s seed: 0x12345678 ^ the LCG multiplier,
     * as the reference's thread-local state starts. */
    d->comfortState = (0x12345678ULL ^ 0x5deece66dULL) & 0xffffffffffffULL;
}

size_t dstar_decoder_state_size(void) { return sizeof(dstar_decoder_t); }

void dstar_decoder_frame(dstar_decoder_t *d, const uint8_t params[49],
                         int c0Errors, int protectedErrors,
                         int16_t pcm[DSTAR_FRAME_SAMPLES])
{
    if (d == NULL || params == NULL || pcm == NULL)
        return;

    dstar_parms_t *cur = &d->cur;
    dstar_parms_t *prev = &d->prev;
    dstar_parms_t *enh = &d->enh;
    const int totalErrors = c0Errors + protectedErrors;

    d->frames++;
    d->lastErrors = (uint16_t)totalErrors;

    cur->mutingThreshold = D_MUTING_AMBE;
    cur->errorCountTotal = (int16_t)totalErrors;
    cur->errorCount4 = 0;
    cur->errorRate = (0.95f * prev->errorRate) + (0.001064f * (float)totalErrors);

    const int bad = decode_parms(d, params, cur, prev);

    /* mbe_update_decode_state() */
    if (bad == BAD_ERASURE) {
        cur->repeatCount = 0;
        parms_erasure(cur, prev);
    } else if (bad == BAD_TONE_FRAME) {
        cur->repeatCount = 0;
    } else if (is_tone_class(bad) && c0Errors < 2 && totalErrors < 3) {
        /* tone frame accepted: played by synthesize_frame() */
    } else if (totalErrors > 3) {
        parms_repeat_model(cur, prev);
        cur->repeatCount = (int16_t)repeat_next_count(prev->repeatCount);
        d->badFrames++;
    } else {
        cur->repeatCount = 0;
    }

    synthesize_frame(d, d->fbuf, params, bad, c0Errors, totalErrors, cur, prev,
                     enh);
    pcm_convert(d->fbuf, pcm);
}
