/* imbe_fft.c -- 256-point real FFT: a 128-point complex FFT plus even/odd
 * untangling.  See dmr_fft.h.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "imbe_fft.h"
#include "dmr_tables.h"  /* same 256-point transform, same twiddles */

/* In-place 128-point complex FFT, decimation in time: 7 radix-2 stages over
 * bit-reversed input.  Pointers are walked instead of re-indexing so the
 * address arithmetic stays off the inner loop.
 */
static void fft128(float *zr, float *zi)
{
    for (int i = 0; i < IMBE_FFT_HALF; i++) {
        int j = dmr_bitrev128[i];
        if (j > i) {
            float t = zr[i];
            zr[i] = zr[j];
            zr[j] = t;
            t = zi[i];
            zi[i] = zi[j];
            zi[j] = t;
        }
    }
    for (int half = 1; half < IMBE_FFT_HALF; half <<= 1) {
        const int span = half << 1;
        const int step = IMBE_FFT_HALF / span;
        for (int k = 0; k < half; k++) {
            const float wr = dmr_tw128[k * step];
            const float wi = dmr_tw128i[k * step];
            float *pr = zr + k, *pi = zi + k;
            float *qr = pr + half, *qi = pi + half;
            const float *end = zr + IMBE_FFT_HALF;
            while (pr < end) {
                const float ar = *qr, ai = *qi;
                const float tr = wr * ar - wi * ai;
                const float ti = wr * ai + wi * ar;
                *qr = *pr - tr;
                *qi = *pi - ti;
                *pr += tr;
                *pi += ti;
                pr += span;
                pi += span;
                qr += span;
                qi += span;
            }
        }
    }
}

void imbe_rfft256(const float *x, float *re, float *im)
{
    float zr[IMBE_FFT_HALF], zi[IMBE_FFT_HALF];

    for (int k = 0; k < IMBE_FFT_HALF; k++) {
        zr[k] = x[2 * k];
        zi[k] = x[2 * k + 1];
    }
    fft128(zr, zi);

    /* X[k] = E[k] + W256^k * O[k], with
     *   E[k] = 0.5*(Z[k]   + conj(Z[128-k]))
     *   O[k] = -0.5j*(Z[k] - conj(Z[128-k]))
     * valid for every k in 0..128 (indices of Z taken modulo 128). */
    for (int k = 0; k <= IMBE_FFT_HALF; k++) {
        int m = (IMBE_FFT_HALF - k) & (IMBE_FFT_HALF - 1);
        int j = k & (IMBE_FFT_HALF - 1);
        float ar = zr[j], ai = zi[j];
        float br = zr[m], bi = -zi[m];
        float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        float orr = di, oi = -dr; /* O = -j*D */
        float wr = dmr_tw256[k], wi = dmr_tw256i[k];
        re[k] = er + (wr * orr - wi * oi);
        im[k] = ei + (wr * oi + wi * orr);
    }
    im[0] = 0.0f;
    im[IMBE_FFT_HALF] = 0.0f;
}

void imbe_irfft256(const float *re, const float *im, float *x)
{
    float zr[IMBE_FFT_HALF], zi[IMBE_FFT_HALF];

    /* Recover Z[k] = E[k] + j*O[k]:
     *   E[k]     = 0.5*(X[k] + conj(X[128-k]))
     *   O[k]     = 0.5*(X[k] - conj(X[128-k])) * conj(W256^k)
     * and Z[128-k] = conj(Z[k]) for the upper half. */
    for (int k = 0; k <= IMBE_FFT_HALF / 2; k++) {
        int m = IMBE_FFT_HALF - k;
        float ar = re[k], ai = im[k];
        float br = re[m], bi = -im[m];
        float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        float wr = dmr_tw256[k], wi = dmr_tw256i[k];
        float orr = dr * wr + di * wi; /* D * conj(W) */
        float oi = di * wr - dr * wi;
        zr[k] = er - oi; /* E + j*O */
        zi[k] = ei + orr;
        int j = IMBE_FFT_HALF - k;
        if (j != IMBE_FFT_HALF && j != k) { /* skip k = 0 and k = 64 */
            zr[j] = er + oi;
            zi[j] = orr - ei;
        }
    }

    /* 128-point inverse transform, unnormalised: 128*IDFT */
    for (int k = 0; k < IMBE_FFT_HALF; k++)
        zi[k] = -zi[k];
    fft128(zr, zi);
    /* now zr[j] = 128*Re(IDFT(Z)[j]) and zi[j] = -128*Im(IDFT(Z)[j]).
     * x[2m] = Re(IDFT(Z)[m]), x[2m+1] = Im(IDFT(Z)[m]); scaled to the
     * pffft-backward convention (256 * IDFT_256) => factor 2. */
    const float s = 2.0f;
    for (int k = 0; k < IMBE_FFT_HALF; k++) {
        x[2 * k] = zr[k] * s;
        x[2 * k + 1] = -zi[k] * s;
    }
}
