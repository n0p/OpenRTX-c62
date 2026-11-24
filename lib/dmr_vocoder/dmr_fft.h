/* dmr_fft.h -- fixed-size (256 point) real FFT for AMBE+2 unvoiced synthesis.
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Replaces pffft (generic, runtime-planned, 11 KB of setup + scratch on the
 * heap, SIMD-oriented) with the only transform the codec ever needs: a
 * 256-point real transform implemented as a 128-point complex FFT plus the
 * usual even/odd untangling.  Twiddles and the bit-reversal permutation are
 * ROM tables (dmr_tables.h), so init is free and there is no allocation.
 */
#ifndef DMR_FFT_H
#define DMR_FFT_H

#define DMR_FFT_N 256
#define DMR_FFT_HALF 128

/* Forward transform of 256 real samples into bins 0..128 (bins 129..255 are
 * the conjugate mirror and are not stored).  Unnormalised, i.e.
 * re[k]+j*im[k] = sum_n x[n] * exp(-2*pi*j*k*n/256). */
void dmr_rfft256(const float *x, float *re, float *im);

/* Inverse of dmr_rfft256: 256 real samples, scaled by 256 (dmr_irfft256 of
 * dmr_rfft256 gives 256*x, matching pffft's backward transform). */
void dmr_irfft256(const float *re, const float *im, float *x);

#endif /* DMR_FFT_H */
