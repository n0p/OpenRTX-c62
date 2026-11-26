/* imbe_fft.h -- fixed-size (256 point) real FFT for IMBE unvoiced synthesis.
 *
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The same transform the AMBE+2 decoder uses (lib/dmr_vocoder/dmr_fft.c): a
 * 128-point complex FFT plus the even/odd untangling, twiddles and the
 * bit-reversal permutation in ROM, no allocation, no plan.  The IMBE unvoiced
 * synthesis needs a 256-point transform too (TIA-102.BABA eq. 117-126), so the
 * decoder shares the implementation and its tables instead of carrying a
 * second copy.
 */
#ifndef IMBE_FFT_H
#define IMBE_FFT_H

#define IMBE_FFT_N 256
#define IMBE_FFT_HALF 128

/* Forward transform of 256 real samples into bins 0..128 (bins 129..255 are
 * the conjugate mirror and are not stored).  Unnormalised, i.e.
 * re[k]+j*im[k] = sum_n x[n] * exp(-2*pi*j*k*n/256). */
void imbe_rfft256(const float *x, float *re, float *im);

/* Inverse of imbe_rfft256: 256 real samples, scaled by 256 (imbe_irfft256 of
 * imbe_rfft256 gives 256*x, matching pffft's backward transform, which is what
 * the reference divides by). */
void imbe_irfft256(const float *re, const float *im, float *x);

#endif /* IMBE_FFT_H */
