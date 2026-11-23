/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_math.h
 * @brief Deterministic single-precision transcendentals for the DMR vocoder.
 *
 * mbelib-neo calls cosf/sinf/logf/log2f/exp2f a few hundred times per frame,
 * and pffft calls cos/sin in double precision when it builds the transform
 * tables.  Those calls go to whatever libm the firmware is linked with: on the
 * C62 that is newlib, on the machine used to validate the vocoder it is glibc.
 * The two disagree by one unit in the last place for some arguments, which is
 * enough to change a handful of PCM samples by 1 LSB and makes a bit-exact
 * comparison between host and target impossible.
 *
 * These implementations are used instead, for the OpenDMR sources only
 * (dmr_vocoder.c is compiled with -Dcosf=dmr_cosf and friends):
 *
 *  - range reduction in double precision, with the same Cody-Waite split on
 *    every target, so the reduced argument is bit-identical everywhere;
 *  - Taylor polynomials in double precision, truncated far below the
 *    single-precision rounding error of the result;
 *  - only IEEE-754 operations (add, multiply, conversions) - no FMA, no
 *    extended precision, no library call.  With -ffp-contract=off the result is
 *    therefore identical on x86-64 and on the Cortex-M33.
 */

#ifndef DMR_MATH_H
#define DMR_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief cos(x), single precision. */
float dmr_cosf(float x);

/** @brief sin(x), single precision. */
float dmr_sinf(float x);

/** @brief sin(x) and cos(x) together, as __builtin_sincosf. */
void dmr_sincosf(float x, float *s, float *c);

/** @brief Base-2 exponential, single precision. */
float dmr_exp2f(float x);

/** @brief Natural exponential, single precision. */
float dmr_expf(float x);

/** @brief Natural logarithm, single precision. */
float dmr_logf(float x);

/** @brief Base-2 logarithm, single precision. */
float dmr_log2f(float x);

/** @brief cos(x), double precision (used by pffft's table setup). */
double dmr_cos(double x);

/** @brief sin(x), double precision (used by pffft's table setup). */
double dmr_sin(double x);

#ifdef __cplusplus
}
#endif

#endif /* DMR_MATH_H */
