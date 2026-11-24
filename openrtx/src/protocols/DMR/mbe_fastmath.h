/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Fast cosf() and rand() for mbelib, force-included (-include) into the mbelib sources
 * only (see CMakeLists.txt) - mbelib itself is not modified.
 *
 * Why: with uvquality 3 mbelib's speech synthesis calls cosf() ~21000 times
 * per 20 ms frame (~1 M calls/s). On the ESP32-P4 newlib's cosf is a software
 * routine of roughly 100-200 cycles, i.e. 30-55 % of a core for cosines
 * alone - enough to starve the DMR task and make the voice choppy.
 *
 * This version: Cody-Waite reduction to [-pi, pi], fold to [0, pi/2] by symmetry,
 * then a Taylor polynomial to x^12 (remainder < 1e-8 on that interval).
 * About 15 FPU operations. Host test: decoded speech vs. libm cosf -> see
 * test/host_dmr (identical AMBE frames, audio difference far below audibility).
 */
#ifndef MBE_FASTMATH_H
#define MBE_FASTMATH_H

/*
 * The Zephyr build compiles libraries as strict C99 (-std=c99,
 * -D_ANSI_SOURCE), which hides the POSIX constants mbelib uses, and adds -Os
 * after the library's own options: set both here, ahead of mbelib's code.
 */
#pragma GCC optimize("O2")

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

static inline float mbe_fast_cosf(float x)
{
    float k = x * 0.15915494309f; /* 1 / (2 pi) */
    float s = 1.0f, x2;
    k = (float)(int)(k + (k >= 0.0f ? 0.5f : -0.5f));
    /* Cody-Waite reduction: 2*pi = C1 + C2 with C1 = 6.28125 exact in 8 bits,
     * so k*C1 is exact for |k| < 65536 and only the tiny C2 term rounds. */
    x -= k * 6.28125f;
    x -= k * 1.9353071795864769e-3f; /* [-pi, pi] */
    if (x < 0.0f) {
        x = -x;                      /* cos is even */
    }
    if (x > 1.57079632679f) {
        x = 3.14159265359f - x; /* cos(x) = -cos(pi - x) */
        s = -1.0f;
    }
    x2 = x * x;
    /* Taylor series to x^12, evaluated by Horner's rule */
    float p = 2.08767570e-9f;
    p = p * x2 - 2.75573192e-7f;
    p = p * x2 + 2.48015873e-5f;
    p = p * x2 - 1.38888889e-3f;
    p = p * x2 + 4.16666667e-2f;
    p = p * x2 - 0.5f;
    p = p * x2 + 1.0f;
    return s * p;
}

#ifdef MBE_COS_TABLE
/*
 * C62: a table cosine. The synthesis takes ~36000 cosines per frame (160
 * samples, up to 56 harmonics, 4 per unvoiced band): at ~40 cycles each
 * the polynomial above was most of the 7 ms a frame took on the 300 MHz
 * M33. Here: 1024 points per turn and linear interpolation, ~12 cycles,
 * error below 5e-6 (-106 dB). The table, 4 KB, is in RAM, filled once by
 * mbe_cos_init() (dmr_voice.c) before the first frame. Large arguments
 * lose precision as in the polynomial: mbelib computes them in float.
 */
#define MBE_COS_POINTS 1024
extern float mbe_cos_table[MBE_COS_POINTS + 1];

static inline float mbe_table_cosf(float x)
{
    const float t = x * (float)(MBE_COS_POINTS / (2.0 * M_PI));
    int i = (int)t;
    if (t < (float)i)
        i--; /* floor, for negative arguments */
    const float f = t - (float)i;
    const float *c = &mbe_cos_table[i & (MBE_COS_POINTS - 1)];
    return c[0] + f * (c[1] - c[0]);
}

#define cosf mbe_table_cosf
#else
#define cosf mbe_fast_cosf
#endif

/*
 * mbelib calls the double precision exp() and log() on float values. The
 * M33's FPU is single precision only: double is software, thousands of
 * cycles per call, once per harmonic per frame. Single precision is plenty
 * for these spectral amplitudes.
 */
#define exp expf
#define log logf

/*
 * Fast rand() for mbelib's unvoiced synthesis (mbe_rand(), up to ~15000 calls
 * per frame with uvquality 3). newlib's rand() goes through __getreent(), which
 * masks interrupts to read the current task: on the P4 that made the vocoder
 * hog its core under the Pluto (task watchdog, IQ skips). The noise only needs
 * to sound random: xorshift32, same 0..RAND_MAX range. One state shared by the
 * single DMR task; a race would only perturb the noise.
 */
#include <stdlib.h> /* declares the real rand() before the macro below */
#include <stdint.h>

static inline int mbe_fast_rand(void)
{
    static uint32_t st = 0x9e3779b9u;
    uint32_t x = st;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    st = x;
    return (int)(x & RAND_MAX);
}

#define rand mbe_fast_rand

/*
 * C62: the speech synthesis and its parameter decoding run from ITCM, the
 * core's zero wait state code memory, instead of the external flash. From
 * flash, through an instruction cache shared with the demodulator, the
 * audio worker and the UI, a frame took 7.5 ms of the vocoder's own CPU time
 * where its arithmetic needs about 1.5. Redeclared here with the section,
 * which GCC applies to the definitions that follow in mbelib's sources.
 * About 6.6 KB of the ITCM's 16.
 */
#ifdef MBE_ITCM_HOT
#include "mbelib.h"
#define MBE_ITCM __attribute__((section(".itcm"), noinline))
void mbe_synthesizeSpeechf(float *aout_buf, mbe_parms *cur_mp,
                           mbe_parms *prev_mp, int uvquality) MBE_ITCM;
void mbe_spectralAmpEnhance(mbe_parms *cur_mp) MBE_ITCM;
int mbe_decodeAmbe2450Parms(char *ambe_d, mbe_parms *cur_mp,
                            mbe_parms *prev_mp) MBE_ITCM;
#endif

#endif /* MBE_FASTMATH_H */
