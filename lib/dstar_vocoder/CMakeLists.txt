# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# D-STAR AMBE 3600x2400 voice decoder, decode only.
#
# This directory is the backend of the D-STAR voice path: the mode pushes the
# 72 voice bits of a DV frame and knows nothing else.  The implementation is a
# C port of mbelib-neo v2.6.2's D-STAR path (GPL-2.0-or-later): no C++, no
# heap, no thread-local storage, no libm, every table in flash, all state in
# one object.
#
# The C62 build does not use this file: platform/targets/c62/CMakeLists.txt
# compiles dstar_vocoder.c and dstar_decoder.c straight into the application
# (so the library overrides the protocol layer's weak hooks) and places the
# state in PSRAM.  This target is for the host tests and for any other target
# that wants the decoder as a library.
#
#   dstar_vocoder.c  firmware-facing contract (one static instance)
#   dstar_decoder.c  AMBE 2400 core: FEC-free parameter decode, synthesis
#
# Host build:
#   gcc -O2 -std=c99 -I../../lib/dmr_vocoder -I../../openrtx/include \
#       dstar_vocoder.c dstar_decoder.c ../../lib/dmr_vocoder/dmr_fft.c \
#       ../../lib/dmr_vocoder/dmr_golay.c ../../lib/dmr_vocoder/dmr_golay_table.c \
#       ../../openrtx/src/protocols/DSTAR/DStarVocoder.c -lm

cmake_minimum_required(VERSION 3.20.0)

add_library(dstar_vocoder STATIC
    dstar_vocoder.c
    dstar_decoder.c
    ../dmr_vocoder/dmr_fft.c
    ../dmr_vocoder/dmr_golay.c
    ../dmr_vocoder/dmr_golay_table.c)

target_include_directories(dstar_vocoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../dmr_vocoder)

set_target_properties(dstar_vocoder PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

# The decoder is the hot path, so -O2 rather than the firmware-wide -Os.  Set
# per source because the flags inherited from zephyr_interface (which include
# -Os) come after a target's own options and would win.
#
# -ffp-contract=off is a fidelity requirement, not a tuning knob: the
# synthesized phase runs to hundreds of radians, where fusing a product into
# FMA changes the reconstructed phase of a harmonic (the reference is built
# without contraction in the model path).
#
# -fno-math-errno keeps sqrtf a single VSQRT and the object free of libm; the
# decoder's transcendentals come from dmr_math.h and dmr_fft.c.
set_source_files_properties(dstar_decoder.c dstar_vocoder.c PROPERTIES
    COMPILE_OPTIONS "-O2;-ffp-contract=off;-fno-math-errno")
