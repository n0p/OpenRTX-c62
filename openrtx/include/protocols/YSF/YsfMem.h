/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_YSF_MEM_H
#define OPENRTX_YSF_MEM_H

/**
 * @file YsfMem.h
 * @brief Where the System Fusion receive buffers live.
 *
 * The C62 has 256 KB of SRAM and 1.75 MB of PSRAM, and this mode is the
 * largest of the digital ones: a 100 ms baseband frame per stream half
 * (19.2 KB), the 4800-sample ring of the demodulator (14.4 KB), the frame
 * layer state and the voice queue. All of it is written and read at audio or
 * symbol rate -- never inside a per-sample loop -- so it goes to PSRAM, the
 * same move lib/dmr_vocoder's port shim and the codec endpoints already make.
 * See docs/RX_FUSION.md, section "RAM".
 *
 * .psram_section is NOLOAD: the linker does not zero it and does not copy it
 * at boot. Everything placed there is cleared by its own init or reset
 * function before it is first read -- ysfPhy_init(), ysfFrame_reset() and
 * ysfSpeech_start() -- and the frame layer resets itself on first use, so a
 * screen drawn before the mode is enabled cannot read uninitialised memory.
 */

#ifdef PLATFORM_C62
#define YSF_EXT_RAM __attribute__((section(".psram_section")))
#else
#define YSF_EXT_RAM
#endif

#endif /* OPENRTX_YSF_MEM_H */
