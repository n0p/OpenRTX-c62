/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_math_redirect.h
 * @brief Send the OpenDMR sources to the deterministic math of dmr_math.c.
 *
 * Force-included (-include) when compiling lib/opendmr/decoder/*.c for this
 * library, so that libm's cosf/sinf/cos/sin/logf/log2f/exp2f - and the
 * __builtin_sincosf that mbe_math.h would otherwise use - are replaced by the
 * implementations in dmr_math.c, which give the same result on the firmware and
 * on the host used to validate it.
 *
 * <math.h> is included first, on purpose: the real declarations must be seen
 * before the macros are defined, and its include guard keeps the sources'
 * own #include <math.h> from being mangled by them.
 */

#ifndef DMR_MATH_REDIRECT_H
#define DMR_MATH_REDIRECT_H

#include <math.h>

#include "dmr_alloc.h"
#include "dmr_math.h"

#define cosf dmr_cosf
#define sinf dmr_sinf
#define cos dmr_cos
#define sin dmr_sin
#define exp2f dmr_exp2f
#define expf dmr_expf
#define logf dmr_logf
#define log2f dmr_log2f
/* mbe_math.h: replace __builtin_sincosf with the same implementation */
#define MBE_SINCOSF dmr_sincosf

#endif /* DMR_MATH_REDIRECT_H */
