/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_math.c
 * @brief Deterministic transcendentals for the DMR vocoder, see dmr_math.h.
 *
 * Everything is evaluated with IEEE-754 double arithmetic from exactly
 * representable coefficients, so the results are identical on every target
 * that rounds to nearest even and does not contract multiply-adds.  The
 * polynomials are plain Taylor series, truncated where the first omitted term
 * is below the resolution needed by the caller: the worst case (|r| = pi/4 for
 * sin/cos, |t| = ln 2 for exp2) leaves an error around 1e-13, far below the
 * 6e-8 quantum of a single-precision result.
 */

#include "dmr_math.h"

/* pi/2, split into three doubles so that k * (HI + LO + LO2) keeps the low bits
 * of the reduced argument for |x| up to about 1e8 radians (mbelib's harmonic
 * phases grow without bound during a long transmission). */
#define PIO2_HI 1.57079632673412561417e+00
#define PIO2_LO 6.07710050630396597660e-11
#define PIO2_LO2 2.02226624879595063154e-21
#define TWO_OVER_PI 6.36619772367581382433e-01

#define LN2 6.93147180559945286227e-01
#define LN2_HI 6.93147180369123816490e-01
#define LN2_LO 1.90821492927058770002e-10
#define SQRT2 1.41421356237309504880e+00
#define LOG2E 1.44269504088896340736e+00

/* Taylor coefficients: -1/6, 1/120, -1/5040, 1/362880, -1/39916800, 1/6227020800 */
#define SIN1 (-1.66666666666666657415e-01)
#define SIN2 8.33333333333333321769e-03
#define SIN3 (-1.98412698412698412526e-04)
#define SIN4 2.75573192239858925110e-06
#define SIN5 (-2.50521083854417202244e-08)
#define SIN6 1.60590438368216145994e-10

/* Taylor coefficients: -1/2, 1/24, -1/720, 1/40320, -1/3628800, 1/479001600 */
#define COS1 (-5.00000000000000000000e-01)
#define COS2 4.16666666666666643537e-02
#define COS3 (-1.38888888888888894189e-03)
#define COS4 2.48015873015873022091e-05
#define COS5 (-2.75573192239858925110e-07)
#define COS6 2.08767569878680989792e-09

typedef union {
    double d;
    unsigned long long u;
} dbl_bits;

static double kernel_sin(double r)
{
    double z = r * r;

    return r + r * z * (SIN1 + z * (SIN2 + z * (SIN3 + z * (SIN4 + z * (SIN5 + z * SIN6)))));
}

static double kernel_cos(double r)
{
    double z = r * r;

    return 1.0 + z * (COS1 + z * (COS2 + z * (COS3 + z * (COS4 + z * (COS5 + z * COS6)))));
}

/**
 * @brief Reduce an angle to r in [-pi/4, pi/4] plus a quadrant index.
 *
 * @param x Angle in radians.
 * @param q Output quadrant, 0..3, such that x = q*pi/2 + r.
 * @return Reduced argument.
 */
static double reduce(double x, int *q)
{
    /* Round half away from zero: truncation and cast are deterministic
     * everywhere, unlike rint()'s dependence on the rounding mode. */
    double k = x * TWO_OVER_PI;
    double kn = (k < 0.0) ? (double)(long)(k - 0.5) : (double)(long)(k + 0.5);
    double r = x - kn * PIO2_HI - kn * PIO2_LO - kn * PIO2_LO2;

    *q = (int)((long)kn & 3L);
    return r;
}

double dmr_sin(double x)
{
    int q;
    double r = reduce(x, &q);

    switch (q) {
        case 0:
            return kernel_sin(r);
        case 1:
            return kernel_cos(r);
        case 2:
            return -kernel_sin(r);
        default:
            return -kernel_cos(r);
    }
}

double dmr_cos(double x)
{
    int q;
    double r = reduce(x, &q);

    switch (q) {
        case 0:
            return kernel_cos(r);
        case 1:
            return -kernel_sin(r);
        case 2:
            return -kernel_cos(r);
        default:
            return kernel_sin(r);
    }
}

float dmr_sinf(float x)
{
    return (float)dmr_sin((double)x);
}

float dmr_cosf(float x)
{
    return (float)dmr_cos((double)x);
}

void dmr_sincosf(float x, float *s, float *c)
{
    int q;
    double r = reduce((double)x, &q);
    double sn = kernel_sin(r);
    double cs = kernel_cos(r);

    switch (q) {
        case 0:
            *s = (float)sn;
            *c = (float)cs;
            break;
        case 1:
            *s = (float)cs;
            *c = (float)(-sn);
            break;
        case 2:
            *s = (float)(-sn);
            *c = (float)(-cs);
            break;
        default:
            *s = (float)(-cs);
            *c = (float)sn;
            break;
    }
}

/**
 * @brief log(m) for m in [sqrt2/2, sqrt2), from the atanh series.
 */
static double log_kernel(double m)
{
    static const double inv[] = {
        1.0 / 19.0, 1.0 / 17.0, 1.0 / 15.0, 1.0 / 13.0, 1.0 / 11.0,
        1.0 / 9.0,  1.0 / 7.0,  1.0 / 5.0,  1.0 / 3.0,  1.0
    };
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    double acc = inv[0];

    for (unsigned int i = 1; i < sizeof inv / sizeof inv[0]; i++)
        acc = inv[i] + s2 * acc;

    return 2.0 * s * acc;
}

/**
 * @brief Split a positive double into a mantissa in [sqrt2/2, sqrt2) and exponent.
 */
static double frexp_sqrt2(double x, int *exp)
{
    dbl_bits v;
    int e;
    double m;

    v.d = x;
    e = (int)((v.u >> 52) & 0x7FFu) - 1023;                 /* x = m * 2^e */
    v.u = (v.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull; /* m in [1,2) */
    m = v.d;
    if (m >= SQRT2) {
        m *= 0.5;
        e += 1;
    }
    *exp = e;
    return m;
}

float dmr_logf(float x)
{
    int e;

    if (!((double)x > 0.0))
        return 0.0f; /* not reachable in the decoder: amplitudes are positive */

    double m = frexp_sqrt2((double)x, &e);

    return (float)((double)e * LN2 + log_kernel(m));
}

float dmr_log2f(float x)
{
    int e;

    if (!((double)x > 0.0))
        return 0.0f; /* not reachable in the decoder */

    double m = frexp_sqrt2((double)x, &e);

    return (float)((double)e + log_kernel(m) * LOG2E);
}

/**
 * @brief exp(p) for |p| <= ln 2 / 2, Taylor to p^13.
 */
static double kernel_exp(double p)
{
    static const double inv_fact[] = {
        1.0 / 479001600.0, 1.0 / 39916800.0, 1.0 / 3628800.0, 1.0 / 362880.0,
        1.0 / 40320.0, 1.0 / 5040.0, 1.0 / 720.0, 1.0 / 120.0, 1.0 / 24.0,
        1.0 / 6.0, 0.5, 1.0, 1.0
    };
    double acc = inv_fact[0];

    for (unsigned int i = 1; i < sizeof inv_fact / sizeof inv_fact[0]; i++)
        acc = inv_fact[i] + p * acc;

    return acc;
}

float dmr_expf(float x)
{
    double xd = (double)x;
    double k;
    double kn;
    double r;
    int e;
    dbl_bits scale;

    if (xd > 700.0)
        xd = 700.0;
    if (xd < -700.0)
        xd = -700.0;

    /* x = k*ln2 + r, with |r| <= ln2/2 */
    k = xd * LOG2E;
    kn = (k < 0.0) ? (double)(long)(k - 0.5) : (double)(long)(k + 0.5);
    r = xd - kn * LN2_HI - kn * LN2_LO;
    e = (int)kn;

    if (e < -1022)
        return 0.0f;
    if (e > 1023)
        return 3.4028234663852886e+38f;

    scale.u = (unsigned long long)(e + 1023) << 52;
    scale.d *= kernel_exp(r);

    return (float)scale.d;
}

float dmr_exp2f(float x)
{
    double xd = (double)x;
    double n;
    double f;
    double t;
    double p;
    int e;
    dbl_bits scale;

    if (xd > 1000.0)
        xd = 1000.0;
    if (xd < -1000.0)
        xd = -1000.0;

    n = (double)(long)xd;
    if (n > xd)
        n -= 1.0; /* floor() without a library call */
    f = xd - n;   /* [0, 1) */
    t = f * LN2;

    /* exp(t) for t in [0, ln 2), Taylor to t^13 */
    {
        static const double inv_fact[] = {
            1.0 / 479001600.0, 1.0 / 39916800.0, 1.0 / 3628800.0, 1.0 / 362880.0,
            1.0 / 40320.0, 1.0 / 5040.0, 1.0 / 720.0, 1.0 / 120.0, 1.0 / 24.0,
            1.0 / 6.0, 0.5, 1.0, 1.0
        };
        p = inv_fact[0];
        for (unsigned int i = 1; i < sizeof inv_fact / sizeof inv_fact[0]; i++)
            p = inv_fact[i] + t * p;
    }

    e = (int)n;
    if (e < -1022)
        return 0.0f;
    if (e > 1023)
        return 3.4028234663852886e+38f;

    scale.u = (unsigned long long)(e + 1023) << 52; /* 2^e as a double */
    scale.d *= p;

    return (float)scale.d;
}
