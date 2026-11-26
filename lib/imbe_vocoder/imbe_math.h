/* imbe_math.h -- dependency-free single-precision math for the IMBE decoder.
 *
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The same reasoning as lib/dmr_vocoder/dmr_math.h: the Cortex-M33 has no
 * double-precision FPU and newlib's cosf() reduces its argument in double, so
 * every routine here is pure float, table + polynomial, and calls no libm.
 * atan2f() is added because the IMBE synthesis renders DVSI's output high-pass
 * (mbe_low_band_phase(), TIA-102.BABA's flat spec rendering has no such
 * filter), whose phase is an arctangent per harmonic and per FFT bin.
 *
 * Accuracy against glibc (double atan2 rounded to float for imbe_atan2f):
 *   imbe_sincosf/cosf/sinf : <= 2 ulp for |x| <= 2^22
 *   imbe_exp2f             : <= 1 ulp relative
 *   imbe_log2f             : <= 2 ulp relative
 *   imbe_atan2f            : <= 2 ulp of the result
 *   imbe_sqrtf             : exact (VSQRT.F32)
 */
#ifndef IMBE_MATH_H
#define IMBE_MATH_H

#include <stdint.h>

#define IMBE_PIO2_A 1.5703125f                 /* pi/2, 8 significant bits */
#define IMBE_PIO2_A2 6432.0f                   /* A * 4096, exact */
#define IMBE_PIO2_B 4.83826792333275080e-04f   /* pi/2 - A */
#define IMBE_PIO2_C 2.56328291925456142e-12f   /* pi/2 - A - B */
#define IMBE_2_PI 0.636619772367581343f
#define IMBE_PI 3.14159265358979323846f
#define IMBE_TWO_PI 6.28318530717958647692f
#define IMBE_PI_2 1.57079632679489661923f
#define IMBE_SQRT2 1.41421356237309504880f

typedef union {
    float f;
    uint32_t u;
    int32_t i;
} imbe_f32u;

static inline float imbe_bitcast_f(uint32_t u)
{
    imbe_f32u v;
    v.u = u;
    return v.f;
}

static inline uint32_t imbe_bitcast_u(float f)
{
    imbe_f32u v;
    v.f = f;
    return v.u;
}

/* Materialise a float in an FP register so that GCC cannot fuse the product
 * around it into an FMA: the reference is built without contraction and the
 * model's phase accumulators reach ~1e6 rad, where one ulp changes cos(). */
static inline float imbe_nofma(float x)
{
#if defined(__arm__) && defined(__ARM_PCS_VFP)
    __asm__("" : "+w"(x));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__("" : "+x"(x));
#endif
    return x;
}

static inline float imbe_fabsf(float x) { return __builtin_fabsf(x); }
static inline float imbe_sqrtf(float x) { return __builtin_sqrtf(x); }

/* k = rint(x * 2/pi) and r = x - k*pi/2, both exact (Cody-Waite, as the
 * reference's sincosf does it). */
static inline int32_t imbe_rint_2opi(float x, float *rest)
{
    float t = x * IMBE_2_PI + 12582912.0f; /* 1.5 * 2^23 */
    int32_t k = (int32_t)(imbe_bitcast_u(t) & 0x7FFFFFu) - 0x400000;
    int32_t k1 = k / 4096;
    int32_t k0 = k - k1 * 4096;
    float y = x - (float)k1 * IMBE_PIO2_A2;
    y = y - (float)k0 * IMBE_PIO2_A;
    float r = __builtin_fmaf(-(float)k, IMBE_PIO2_B, y);
    r = __builtin_fmaf(-(float)k, IMBE_PIO2_C, r);
    *rest = r;
    return k;
}

/* cos(r) and sin(r) for |r| <= pi/4, |err| < 1.3e-8 */
static inline float imbe_cos_poly(float r)
{
    float r2 = r * r;
    return 1.0f + r2 * (-0.5f + r2 * (4.16666679e-02f + r2 * (-1.38888892e-03f + r2 * 2.48015876e-05f)));
}
static inline float imbe_sin_poly(float r)
{
    float r2 = r * r;
    return r * (1.0f + r2 * (-1.66666672e-01f + r2 * (8.33333377e-03f + r2 *
              (-1.98412700e-04f + r2 * 2.75573188e-06f))));
}

static inline void imbe_sincosf(float x, float *s_out, float *c_out)
{
    float r;
    int32_t k = imbe_rint_2opi(x, &r);
    float s = imbe_sin_poly(r), c = imbe_cos_poly(r);
    switch (k & 3) {
    case 0: *s_out = s; *c_out = c; break;
    case 1: *s_out = c; *c_out = -s; break;
    case 2: *s_out = -s; *c_out = -c; break;
    default: *s_out = -c; *c_out = s; break;
    }
}

static inline float imbe_cosf(float x)
{
    float r;
    int32_t k = imbe_rint_2opi(x, &r);
    if (k & 1) {
        float s = imbe_sin_poly(r);
        return (k & 2) ? s : -s;
    }
    float c = imbe_cos_poly(r);
    return (k & 2) ? -c : c;
}

static inline float imbe_sinf(float x)
{
    float r;
    int32_t k = imbe_rint_2opi(x, &r);
    if (k & 1) {
        float c = imbe_cos_poly(r);
        return (k & 2) ? -c : c;
    }
    float s = imbe_sin_poly(r);
    return (k & 2) ? -s : s;
}

/* 2^x, |err| <= 2e-7 relative; 0 below -126, +inf above 128. */
static inline float imbe_exp2f(float x)
{
    int32_t i = (int32_t)(x + (x >= 0.0f ? 0.5f : -0.5f));
    float f = x - (float)i;
    float p = 1.00000000e+00f
              + f * (6.93147182e-01f
                     + f * (2.40226507e-01f
                            + f * (5.55042996e-02f
                                   + f * (9.61812556e-03f
                                          + f * (1.33335581e-03f
                                                 + f * (1.54035304e-04f + f * 1.52527338e-05f))))));
    if (i > 127)
        return imbe_bitcast_f(0x7F800000u);
    if (i < -126)
        return 0.0f;
    return p * imbe_bitcast_f((uint32_t)(i + 127) << 23);
}

static inline float imbe_expf(float x) { return imbe_exp2f(x * 1.44269504088896341f); }

/* log2 of a positive float, |err| <= 2 ulp. */
static inline float imbe_log2f(float x)
{
    imbe_f32u v;
    v.f = x;
    int32_t e = (int32_t)((v.u >> 23) & 0xFFu) - 127;
    float m = imbe_bitcast_f((v.u & 0x007FFFFFu) | 0x3F800000u);
    float y = (m - 1.0f) / (m + 1.0f);
    float y2 = y * y;
    float t = y * (1.0f + y2 * (3.33333343e-01f + y2 * (2.00000003e-01f + y2 * (1.42857149e-01f +
              y2 * (1.11111112e-01f + y2 * 9.09090924e-02f)))));
    return (float)e + t * 2.88539008177792681f;
}

static inline float imbe_logf(float x) { return imbe_log2f(x) * 0.693147180559945286f; }

static inline int32_t imbe_ifloor(float x)
{
    int32_t i = (int32_t)x;
    if ((float)i > x)
        i--;
    return i;
}
static inline int32_t imbe_iceil(float x)
{
    int32_t i = (int32_t)x;
    if ((float)i < x)
        i++;
    return i;
}

/* atan(x), full range, |err| < 1.5 ulp: fdlibm's atanf argument reduction and
 * kernel.  The polynomial alone is only good for |x| <= 0.4375; the reduction
 * maps |x| into that window in four pieces. */
static inline float imbe_atanf(float x)
{
    static const float aT[5] = {3.3333328366e-01f, -1.9999158382e-01f, 1.4253635705e-01f,
                                -1.0648017377e-01f, 6.1687607318e-02f};
    static const float atanhi[4] = {4.6364760399e-01f, 7.8539812565e-01f, 9.8279368877e-01f, 1.5707962513e+00f};
    static const float atanlo[4] = {5.0121582440e-09f, 3.7748947079e-08f, 3.4473217170e-08f, 7.5497894159e-08f};
    const float ax = imbe_fabsf(x);
    int id;
    float z, w, s1, s2;

    const int sign = imbe_bitcast_u(x) >> 31;

    if (ax < 0.4375f) {
        if (ax < 1.0e-4f)
            return x;
        id = -1;
    } else if (ax < 0.6875f) {
        x = ax;  /* the reduction works on the magnitude */
        id = 0;
        x = ((2.0f * x) - 1.0f) / (2.0f + x);
    } else if (ax < 1.1875f) {
        x = ax;
        id = 1;
        x = (x - 1.0f) / (x + 1.0f);
    } else if (ax < 2.4375f) {
        x = ax;
        id = 2;
        x = (x - 1.5f) / (1.0f + (1.5f * x));
    } else {
        x = ax;
        id = 3;
        x = -1.0f / x;
    }

    z = x * x;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * aT[4]));
    s2 = w * (aT[1] + w * aT[3]);
    if (id < 0)
        return x - x * (s1 + s2);
    z = atanhi[id] - (((x * (s1 + s2)) - atanlo[id]) - x);
    return sign ? -z : z;
}

/* atan2(y, x), principal value.  Used for the phase of the output high-pass;
 * the reference computes it in double, whose rounded result this reproduces to
 * within a couple of hundredths of an ulp of pi. */
static inline float imbe_atan2f(float y, float x)
{
    float a;

    if (x == 0.0f) {
        if (y == 0.0f)
            return 0.0f;
        return (y > 0.0f) ? IMBE_PI_2 : -IMBE_PI_2;
    }
    a = imbe_atanf(y / x);
    if (x < 0.0f)
        a += (y >= 0.0f) ? IMBE_PI : -IMBE_PI;
    return a;
}

#endif /* IMBE_MATH_H */
