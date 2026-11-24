/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_enc_trig.h
 * @brief Cosines and logarithms of the AMBE+2 encoder, precomputed.
 *
 * The reference evaluates these with cosf()/log2f() at run time (and mbelib
 * caches the cosines in a thread-local struct on first use).  Here they are
 * const tables in flash, whose values were produced by evaluating the *same
 * expressions* with the host libm, printed as hex floats: the firmware
 * therefore reproduces the reference bit for bit without linking libm for
 * anything but log2f (see dmr_enc_frame.c).
 *
 *  - `dmr_enc_cos_prba[m][i]`    cos(pi*(m-1)*(i-0.5)/8), the encoder's float
 *                                evaluation.  Index 0 unused.
 *  - `dmr_enc_cos_prba_m[m][i]`  the same matrix as mbelib builds it, in
 *                                double (the model's Ri).
 *  - `dmr_enc_cos_dct[..]`       cos(pi*(k-1)*(j-0.5)/ji), flat, encoder's
 *                                evaluation; `dmr_enc_cos_dct_off[ji]` is the
 *                                offset of block ji, and the entry of (ji, j, k)
 *                                is off[ji] + (j-1)*ji + (k-1).
 *  - `dmr_enc_cos_dct_m[..]`     the same, mbelib's evaluation (model).
 *  - `dmr_enc_log2_b0[b][0..1]`  0.5*log2(num_harms) and
 *                                0.5*log2(num_harms*w0*2*pi) + 2.289 for the
 *                                fundamental index b.
 *  - `dmr_enc_log2_L[L]`         log2f(L).
 */

#ifndef DMR_ENC_TRIG_H
#define DMR_ENC_TRIG_H

#include <stdint.h>

#define DMR_ENC_DCT_ENTRIES 1785 /* sum of ji*ji for ji = 1..17 */

extern const float dmr_enc_cos_prba[9][9];
extern const float dmr_enc_cos_prba_m[9][9];
extern const float dmr_enc_cos_dct[DMR_ENC_DCT_ENTRIES];
extern const float dmr_enc_cos_dct_m[DMR_ENC_DCT_ENTRIES];
extern const uint16_t dmr_enc_cos_dct_off[19];
extern const float dmr_enc_log2_b0[120][2];
extern const float dmr_enc_log2_L[58];

/** cos(pi*(k-1)*(j-0.5)/ji) as the encoder evaluates it (1-based j, k). */
#define DCT_COS(ji, j, k) \
    (dmr_enc_cos_dct[dmr_enc_cos_dct_off[ji] + ((j) - 1) * (ji) + ((k) - 1)])

/** The same as mbelib evaluates it (the model's inverse DCT). */
#define DCT_COS_M(ji, j, k) \
    (dmr_enc_cos_dct_m[dmr_enc_cos_dct_off[ji] + ((j) - 1) * (ji) + ((k) - 1)])

#endif /* DMR_ENC_TRIG_H */
