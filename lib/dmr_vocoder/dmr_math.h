/* dmr_math.h -- dependency-free single-precision math for the AMBE+2 decoder.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Why this file exists: on a Cortex-M33 there is no double-precision FPU, so
 * every `double` operation becomes a call to __aeabi_dadd/__aeabi_dmul/
 * __aeabi_ddiv.  newlib's cosf()/sinf() do their argument reduction in double
 * (__kernel_rem_pio2f), which cost 36 % of the whole reference decode.  All
 * routines below are pure float and use only ROM tables + minimax polynomials;
 * they never call libm, so the decoder links without -lm.
 *
 * Accuracy (measured on host against glibc, see test/math_bench.c):
 *   dmr_sincosf : |err| <= 2 ulp for |x| <= 8.4e6 (2^23) rad
 *   dmr_exp2f   : <= 1 ulp relative
 *   dmr_log2f   : <= 2 ulp relative
 *   dmr_sqrtf   : exact (VSQRT.F32 on FPv5-SP)
 *
 * NB: dmr_sincosf performs a *correct* Cody-Waite reduction of the float
 * argument, so cosf(x) is reproduced for arbitrarily large phases accumulated
 * by the codec (the model's PSIl grows without bound, up to ~1e6 rad in 20 s
 * of audio).  Wrapping the phase instead would be cheaper but changes the
 * output: see README, section "phase reduction".
 */
#ifndef DMR_MATH_H
#define DMR_MATH_H

#include <stdint.h>

/* pi/2 split for Cody-Waite: A has 8 significant bits so that k*A is exact in
 * single precision for |k| < 2^16, B carries the next 24 bits, C the rest. */
#define DMR_PIO2_A 1.5703125f
#define DMR_PIO2_A2 6432.0f /* A * 4096, exact */
#define DMR_PIO2_B 4.83826792333275080e-04f /* pi/2 - A */
#define DMR_PIO2_C 2.56328291925456142e-12f /* pi/2 - A - B */
#define DMR_2_PI 0.636619772367581343f
#define DMR_PI 3.14159265358979323846f
#define DMR_TWO_PI 6.28318530717958647692f
#define DMR_PI_2 1.57079632679489661923f

typedef union {
    float f;
    uint32_t u;
    int32_t i;
} dmr_f32u;

static inline float dmr_bitcast_f(uint32_t u)
{
    dmr_f32u v;
    v.u = u;
    return v.f;
}
static inline uint32_t dmr_bitcast_u(float f)
{
    dmr_f32u v;
    v.f = f;
    return v.u;
}

/* Force a float to be materialised in an FP register, which stops GCC from
 * contracting the surrounding multiply-add into an FMA.  Needed by the model's
 * phase accumulator: it reaches ~1e6 rad, where one ulp of the sum is 0.06 rad,
 * i.e. the rounding of the product must match the reference (which is built
 * without FMA).  Everywhere else contraction is a free win. */
static inline float dmr_nofma(float x)
{
#if defined(__arm__) && defined(__ARM_PCS_VFP)
    __asm__("" : "+w"(x));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__("" : "+x"(x));
#endif
    return x;
}

static inline float dmr_fabsf(float x) { return __builtin_fabsf(x); }
static inline float dmr_sqrtf(float x) { return __builtin_sqrtf(x); }

/* sin/cos of a float.
 *
 * Valid for |x| <= 2^22 rad (~4.2e6), which covers the phase accumulators the
 * AMBE model builds up: the decoder wraps PSIl when it exceeds 2^21 (see
 * dmr_vocoder.c), so this range is never exceeded by construction.
 *
 * Reduction: k = rint(x*2/pi) is obtained branchlessly with the 1.5*2^23
 * magic constant; x - k*A is then computed exactly (A = pi/2 truncated to 8
 * significant bits, and k split in two halves so both products are exact),
 * and the two remaining terms of pi/2 are subtracted with FMA, which rounds
 * the exact product once.  Final |r| <= pi/4 (+-1e-3), |err| <= 1 ulp.
 */
static inline int32_t dmr_rint_2opi(float x, float *rest)
{
    float t = x * DMR_2_PI + 12582912.0f; /* 1.5 * 2^23 */
    int32_t k = (int32_t)(dmr_bitcast_u(t) & 0x7FFFFFu) - 0x400000;
    /* truncating split: k1 == 0 for |k| < 4096, so both products stay small
     * enough for the subtraction to be exact (Sterbenz) */
    int32_t k1 = k / 4096;
    int32_t k0 = k - k1 * 4096;
    float y = x - (float)k1 * DMR_PIO2_A2;
    y = y - (float)k0 * DMR_PIO2_A;
    /* |y| <= 2615 here; one FMA per term keeps the error at ~1 ulp of r */
    float r = __builtin_fmaf(-(float)k, DMR_PIO2_B, y);
    r = __builtin_fmaf(-(float)k, DMR_PIO2_C, r);
    *rest = r;
    return k;
}

/* cos(r) and sin(r) for |r| <= pi/4, |err| < 1.3e-8 */
static inline float dmr_cos_poly(float r)
{
    float r2 = r * r;
    return 1.0f + r2 * (-0.5f + r2 * (4.16666679e-02f + r2 * (-1.38888892e-03f + r2 * 2.48015876e-05f)));
}
static inline float dmr_sin_poly(float r)
{
    float r2 = r * r;
    return r * (1.0f + r2 * (-1.66666672e-01f + r2 * (8.33333377e-03f + r2 *
              (-1.98412700e-04f + r2 * 2.75573188e-06f))));
}

static inline void dmr_sincosf(float x, float *s_out, float *c_out)
{
    float r;
    int32_t k = dmr_rint_2opi(x, &r);
    float s = dmr_sin_poly(r), c = dmr_cos_poly(r);
    switch (k & 3) {
    case 0: *s_out = s; *c_out = c; break;
    case 1: *s_out = c; *c_out = -s; break;
    case 2: *s_out = -s; *c_out = -c; break;
    default: *s_out = -c; *c_out = s; break;
    }
}

/* cos only: one polynomial instead of two (used 400+ times per frame) */
static inline float dmr_cosf(float x)
{
    float r;
    int32_t k = dmr_rint_2opi(x, &r);
    if (k & 1) {
        float s = dmr_sin_poly(r);
        return (k & 2) ? s : -s;
    }
    float c = dmr_cos_poly(r);
    return (k & 2) ? -c : c;
}

static inline float dmr_sinf(float x)
{
    float r;
    int32_t k = dmr_rint_2opi(x, &r);
    if (k & 1) {
        float c = dmr_cos_poly(r);
        return (k & 2) ? -c : c;
    }
    float s = dmr_sin_poly(r);
    return (k & 2) ? -s : s;
}

/* 2^x, |err| <= 2e-7 relative; returns 0 for x < -126, +inf for x > 128. */
static inline float dmr_exp2f(float x)
{
    /* split x = i + f with i = round(x) and |f| <= 0.5 */
    int32_t i = (int32_t)(x + (x >= 0.0f ? 0.5f : -0.5f));
    float f = x - (float)i;
    /* 2^f = exp(f*ln2), Taylor on |f*ln2| <= 0.347 (|err| < 5e-8) */
    float p = 1.00000000e+00f
              + f * (6.93147182e-01f
                     + f * (2.40226507e-01f
                            + f * (5.55042996e-02f
                                   + f * (9.61812556e-03f
                                          + f * (1.33335581e-03f
                                                 + f * (1.54035304e-04f + f * 1.52527338e-05f))))));
    if (i > 127)
        return dmr_bitcast_f(0x7F800000u); /* +inf, like exp2f */
    if (i < -126)
        return 0.0f;
    return p * dmr_bitcast_f((uint32_t)(i + 127) << 23);
}

static inline float dmr_expf(float x) { return dmr_exp2f(x * 1.44269504088896341f); }

/* log2 of a positive float, |err| <= 2 ulp. */
static inline float dmr_log2f(float x)
{
    dmr_f32u v;
    v.f = x;
    int32_t e = (int32_t)((v.u >> 23) & 0xFFu) - 127;
    /* mantissa in [1,2) */
    float m = dmr_bitcast_f((v.u & 0x007FFFFFu) | 0x3F800000u);
    /* atanh series: log2(m) = 2/ln2 * (y + y^3/3 + y^5/5 + y^7/7), y=(m-1)/(m+1) */
    float y = (m - 1.0f) / (m + 1.0f);
    float y2 = y * y;
    float t = y * (1.0f + y2 * (3.33333343e-01f + y2 * (2.00000003e-01f + y2 * (1.42857149e-01f + y2 * (1.11111112e-01f + y2 * 9.09090924e-02f)))));
    return (float)e + t * 2.88539008177792681f;
}

static inline float dmr_logf(float x) { return dmr_log2f(x) * 0.693147180559945286f; }

/* floor/ceil without libm (values used by the codec are small) */
static inline int32_t dmr_ifloor(float x)
{
    int32_t i = (int32_t)x;
    if ((float)i > x)
        i--;
    return i;
}
static inline int32_t dmr_iceil(float x)
{
    int32_t i = (int32_t)x;
    if ((float)i < x)
        i++;
    return i;
}

#endif /* DMR_MATH_H */
