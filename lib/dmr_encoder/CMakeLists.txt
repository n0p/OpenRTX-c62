# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# DMR AMBE+2 encoder: the transmit half of the vocoder, pure C.
#
# Target-agnostic C99 with no dependency beyond the compiler runtime
# (memcpy/memmove/memset) and log2f from libm.  Everything else is integer, and
# every table (the mbelib quantiser tables, the cosines of the two DCTs, the
# per-b logarithms) is const, in flash.
#
#   dmr_encoder.c            firmware-facing contract, GPL-3.0+
#   dmr_enc_core.c           fixed-point analysis (pitch, FFT, V/UV, spectrum),
#                            GPL-3.0+ (port of Pavel Yazev's IMBE encoder)
#   dmr_enc_frame.c          quantiser, predictor model, Golay, frame packing,
#                            GPL-3.0+ (port of OP25's MBE encoder + mbelib)
#   dmr_enc_ambe_tables.c    quantiser tables from mbelib, GPL-2.0+
#   dmr_enc_trig.c           generated cosines/logarithms, GPL-3.0+

cmake_minimum_required(VERSION 3.20.0)

add_library(dmr_encoder STATIC
    dmr_encoder.c
    dmr_enc_core.c
    dmr_enc_frame.c
    dmr_enc_ambe_tables.c
    dmr_enc_trig.c)

target_include_directories(dmr_encoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(dmr_encoder PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

# Same reasoning as lib/dmr_vocoder, and one extra requirement:
#
# - -O2, not the firmware-wide -Os: the encoder is 3x slower at -Os.
# - -ffp-contract=off is a fidelity requirement, not tuning.  The quantiser is
#   single precision and the reference was measured without FMAs; letting the
#   compiler fuse a*b+c changes the last bit of the DCT and the chosen indices
#   drift (the decoder port lost 28 dB of SNR to this, here it costs frames).
# - -fno-math-errno turns sqrtf/fabsf into single instructions and keeps the
#   object's libm surface to log2f.
# - -Wno-array-bounds: the analysis FFT uses the reference's Fortran-style
#   `data = &datam1[-1]` pointer, which the warning flags as out of bounds.
#
# The options are set per source because zephyr_interface's own -Os is appended
# after the target's, and would win.
set_source_files_properties(
    dmr_encoder.c dmr_enc_core.c dmr_enc_frame.c dmr_enc_ambe_tables.c
    dmr_enc_trig.c
    PROPERTIES COMPILE_OPTIONS
    "-O2;-ffp-contract=off;-fno-math-errno;-Wno-array-bounds")
