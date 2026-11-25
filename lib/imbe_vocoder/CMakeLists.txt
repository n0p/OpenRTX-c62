# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# P25 IMBE 7200x4400 vocoder, decode only.  Target-agnostic C99 with no
# dependency beyond the compiler runtime (memcpy/memset): the transcendental
# functions come from imbe_math.h, the 256-point real FFT from imbe_fft.c and
# every table from imbe_tables.h, in flash.  The FFT shares lib/dmr_vocoder's
# twiddle tables (the same 256-point transform), which is why the include path
# below points at it.
#
#   imbe_vocoder.c  firmware-facing contract (one static instance), GPL-3.0+
#   imbe_decoder.c  IMBE decoder: parameters, enhancement, synthesis, GPL-2.0+
#   imbe_fft.c      fixed 256-point real FFT (from lib/dmr_vocoder), GPL-2.0+

cmake_minimum_required(VERSION 3.20.0)

add_library(imbe_vocoder STATIC
    imbe_vocoder.c
    imbe_decoder.c
    imbe_fft.c)

target_include_directories(imbe_vocoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(imbe_vocoder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../dmr_vocoder)

set_target_properties(imbe_vocoder PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

# The vocoder is the hot path of P25 RX, so it is built with -O2 instead of the
# firmware-wide -Os.  Per source, not on the target: the flags inherited from
# zephyr_interface include -Os and are appended after a target's own options.
#
# -ffp-contract=off on the model path is a fidelity requirement, as in
# lib/dmr_vocoder: the model's phase accumulators reach hundreds of radians,
# where one ulp of a fused multiply-add changes cos().  The FFT never
# accumulates a phase, so it may contract (VFMA).
#
# -fno-math-errno keeps sqrtf a single VSQRT.F32 and the objects free of libm.
set_source_files_properties(imbe_decoder.c imbe_vocoder.c PROPERTIES
    COMPILE_OPTIONS "-O2;-ffp-contract=off;-fno-math-errno")
set_source_files_properties(imbe_fft.c PROPERTIES
    COMPILE_OPTIONS "-O2;-ffp-contract=fast;-fno-math-errno")
