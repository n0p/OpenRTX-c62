# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# DMR AMBE+2 vocoder, decode only.  Target-agnostic C99 with no dependency
# beyond the compiler runtime (memcpy/memset): the transcendental functions come
# from dmr_math.h, the 256-point real FFT from dmr_fft.c and every table from
# dmr_tables.h, in flash.  Nothing is built from the OpenDMR submodule: it stays
# in the tree as the reference this decoder was verified against.
#
#   dmr_vocoder.c   firmware-facing contract (one static instance), GPL-3.0+
#   dmr_decoder.c   AMBE+2 decoder: FEC, parameters, synthesis, GPL-2.0+
#   dmr_fft.c       fixed 256-point real FFT, GPL-2.0+

cmake_minimum_required(VERSION 3.20.0)

add_library(dmr_vocoder STATIC
    dmr_vocoder.c
    dmr_decoder.c
    dmr_fft.c)

target_include_directories(dmr_vocoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(dmr_vocoder PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

# The vocoder is the hot path of DMR RX, so it is built with -O2 instead of the
# firmware-wide -Os.  The options are set per source, not on the target, because
# the flags inherited from zephyr_interface (which include -Os) are appended
# after the target's own options and would win, being last on the command line.
#
# -ffp-contract=off on the model path is a fidelity requirement, not a tuning
# knob: the AMBE phase accumulator reaches ~1e6 rad, where one ulp of the sum is
# 0.06 rad, so letting the compiler fuse the products into FMA changes the
# reconstructed phase (SNR against the reference falls from 106 dB to 78 dB).
# The FFT never accumulates a phase, so it may contract (VFMA): ~2.5 % faster
# with no effect on the output.  FMA written explicitly in the model
# (__builtin_fmaf, dmr_math.h) is unaffected by -ffp-contract.
#
# -fno-math-errno turns sqrtf into a single VSQRT.F32 and keeps the object free
# of libm (errno is meaningless here).
set_source_files_properties(dmr_decoder.c dmr_vocoder.c PROPERTIES
    COMPILE_OPTIONS "-O2;-ffp-contract=off;-fno-math-errno")
set_source_files_properties(dmr_fft.c PROPERTIES
    COMPILE_OPTIONS "-O2;-ffp-contract=fast;-fno-math-errno")
