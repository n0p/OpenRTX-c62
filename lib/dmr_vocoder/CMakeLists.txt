# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# DMR AMBE+2 vocoder, decode only.  Builds the decoder half of OpenDMR
# (lib/opendmr, a git submodule) as a plain static library for the target MCU,
# plus the C layer that replaces OpenDMR's C++ wrapper:
#
#  - the sources stay in the submodule and are not relicensed; the three hooks
#    this build relies on are a separate commit in the submodule itself, see
#    README.md and docs/DMR_VOCODER.md;
#  - the encoder (lib/opendmr/encoder) is not compiled: it does not fit in the
#    CSK6011B's Cortex-M33;
#  - decoder/fftpack.c is not compiled: it is dead code (mbe_unvoiced_fft.c uses
#    pffft) and would pull in double-precision transcendentals.
#
# The library is target-agnostic C99 with no dependency beyond the compiler
# runtime: the transcendental functions come from dmr_math.c, not from libm.

cmake_minimum_required(VERSION 3.20.0)

set(DMR_VOCODER_OPENDMR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../opendmr)
set(DMR_VOCODER_DECODER_DIR ${DMR_VOCODER_OPENDMR_DIR}/decoder)

# Decoder sources from the OpenDMR submodule, unmodified except for the build
# hooks documented in README.md.
set(DMR_VOCODER_OPENDMR_SOURCES
    ${DMR_VOCODER_DECODER_DIR}/mbelib.c
    ${DMR_VOCODER_DECODER_DIR}/mbe_adaptive.c
    ${DMR_VOCODER_DECODER_DIR}/mbe_unvoiced_fft.c
    ${DMR_VOCODER_DECODER_DIR}/ambe3600x2450.c
    ${DMR_VOCODER_DECODER_DIR}/ambe_common.c
    ${DMR_VOCODER_DECODER_DIR}/ecc.c
    ${DMR_VOCODER_DECODER_DIR}/ecc_const.c
    ${DMR_VOCODER_DECODER_DIR}/pffft.c)

# The reference firmware build uses dmr_math.c instead of libm so that the
# firmware and the host reference produce the same PCM.  OFF compiles the
# OpenDMR sources against the C library exactly as upstream does.
option(DMR_VOCODER_DETERMINISTIC_MATH
       "Use dmr_math.c instead of the C library transcendentals" ON)

set(DMR_VOCODER_OWN_SOURCES
    dmr_vocoder.c
    dmr_golay.c
    dmr_golay_table.c)
if(DMR_VOCODER_DETERMINISTIC_MATH)
    list(APPEND DMR_VOCODER_OWN_SOURCES dmr_math.c)
endif()

add_library(dmr_vocoder STATIC
    ${DMR_VOCODER_OWN_SOURCES}
    ${DMR_VOCODER_OPENDMR_SOURCES})

target_include_directories(dmr_vocoder
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${DMR_VOCODER_DECODER_DIR})

target_compile_definitions(dmr_vocoder PRIVATE
    # math constants: -std=c99 hides them in newlib, and the vocoder pins them
    # so that firmware, host and QEMU builds do the same arithmetic
    M_PI=3.14159265358979323846
    M_SQRT1_2=0.70710678118654752440
    M_SQRT2=1.41421356237309504880
    # pffft.c asserts would pull printf/assert into the decode path
    NDEBUG
    # single static instance instead of thread-local state (no emutls on ARM)
    MBE_THREAD_LOCAL=
    # every one-off allocation goes to the library's static arena, not the heap
    PFFFT_MALLOC=dmr_vocoder_alloc
    PFFFT_FREE=dmr_vocoder_free)

# The OpenDMR sources call libm; redirect them to the deterministic
# implementations in dmr_math.c so that the firmware and the host reference
# produce the same PCM.  Applied per source: dmr_math.c itself must keep the
# real libm names free.
if(DMR_VOCODER_DETERMINISTIC_MATH)
    set_source_files_properties(${DMR_VOCODER_OPENDMR_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/dmr_math_redirect.h")
endif()

# pffft.c does not know this layer either: force-include the declarations of the
# allocation hooks it is compiled against (PFFFT_MALLOC/PFFFT_FREE).
target_compile_options(dmr_vocoder PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/dmr_alloc.h)

# The vocoder is the hot path of DMR RX: keep -O2 instead of the firmware-wide
# -Os, and disable FP contraction so the float pipeline is bit-identical to the
# host reference used to validate it (FMA fusion does not happen on x86-64
# baseline, and VFMA on the M33 would change the rounding of every MAC).
# -fno-math-errno turns sqrtf into a single VSQRT.F32 on the M33, leaving the
# library with no libm dependency at all (errno is irrelevant here).
target_compile_options(dmr_vocoder PRIVATE -O2 -ffp-contract=off -fno-math-errno)
