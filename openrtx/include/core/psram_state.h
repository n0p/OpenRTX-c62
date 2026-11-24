/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PSRAM_STATE_H
#define PSRAM_STATE_H

/**
 * @file psram_state.h
 * @brief Mode state that lives in the C62's PSRAM.
 *
 * `.psram_section` is a NOLOAD output section in the PSRAMAP region
 * (csk6_psram.ld) and it is shared with the CSK6 system heap, so nothing
 * zeroes it at boot and nothing copies it from flash: a buffer placed here has
 * to be written before it is read.  The rules for using it are therefore:
 *
 *  - the buffer is reset by its own module when the mode opens
 *    (nxdnPhy_reset(), dstarPhy_reset(), ...), or
 *  - its "already initialised" flag stays in ordinary .bss, which the kernel
 *    does zero (dmr_vocoder.c keeps `dmr_ready` there and initialises the
 *    instance on first use).
 *
 * Only state that is read and written once per frame or per burst belongs
 * here: the PSRAM is slower than the 256 KB of SRAM and the DSP paths that run
 * per sample stay in SRAM.  Buffers handed to audioStream_start() are DMA and
 * can never move.
 *
 * The DMR package has a section of its own, `.psram_dmr`
 * (platform/targets/c62/psram_dmr.ld), which platform_init_csk6() zeroes at
 * boot; this header is for the state of the other modes.  They are two
 * different sections on purpose: `.psram_section` also holds the heap, so it
 * cannot be zeroed wholesale.
 */

#if defined(PLATFORM_C62)
#define PSRAM_STATE __attribute__((section(".psram_section")))
#else
#define PSRAM_STATE
#endif

#endif /* PSRAM_STATE_H */
