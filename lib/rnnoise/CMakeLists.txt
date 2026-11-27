# SPDX-FileCopyrightText: Copyright 2017-2021 Mozilla, Xiph.Org, Amazon, Octasic
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: BSD-3-Clause
#
# RNNoise v0.1.1 (xiph/rnnoise, tag v0.1.1, commit 6cbfd53), vendored unchanged
# except for two local patches: the per-frame working set moved out of the C
# stack and the double arithmetic of the frame path turned into float (see
# PATCHES.md).  BSD-3-Clause, see COPYING; attribution in README.
#
#   denoise.c    the denoiser: features, gains, synthesis (BSD-3)
#   rnn.c        GRU/dense inference on int8 weights (BSD-3)
#   rnn_data.c   the 87 503 int8 weights of the v0.1.1 model (BSD-3)
#   pitch.c      pitch analysis (BSD-3)
#   celt_lpc.c   LPC on top of the FFT (BSD-3)
#   kiss_fft.c   1920-point complex FFT (BSD-3)

cmake_minimum_required(VERSION 3.20.0)

add_library(rnnoise STATIC
    src/denoise.c
    src/rnn.c
    src/rnn_data.c
    src/pitch.c
    src/celt_lpc.c
    src/kiss_fft.c)

target_include_directories(rnnoise PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(rnnoise PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

# _GNU_SOURCE and M_PI: denoise.c uses M_PI, which a strict -std=c99 newlib
# build hides (Zephyr compiles with -std=c99).
#
# -O2, set per source: the flags inherited from zephyr_interface (-Os) are
# appended after a target's own options and would win.  Measured on the M33,
# 10 ms frames of 48 kHz speech: 2 192 946 instructions at -Os against
# 1 907 028 at -O2, and 1 599 912 with -O3 plus -fsingle-precision-constant
# (see docs/NOISE.md).  -fsingle-precision-constant keeps the double-promotion
# out of the literal-heavy filter code; it does not change the output on the
# test vectors beyond the last bit, which the evaluation measured.
set_source_files_properties(
    src/denoise.c src/rnn.c src/rnn_data.c src/pitch.c src/celt_lpc.c src/kiss_fft.c
    PROPERTIES COMPILE_OPTIONS "-O2;-fsingle-precision-constant;-fno-math-errno"
               COMPILE_DEFINITIONS "_GNU_SOURCE;M_PI=3.14159265358979323846")
