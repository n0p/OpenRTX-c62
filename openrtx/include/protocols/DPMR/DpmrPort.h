/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTOCOLS_DPMR_DPMRPORT_H
#define PROTOCOLS_DPMR_DPMRPORT_H

/**
 * @file DpmrPort.h
 * @brief Platform shim of the dPMR receiver: where its big buffers live.
 *
 * The dPMR demodulator, its speech path and the mode itself build unchanged in
 * OpenRTX and in a plain host harness, so nothing here may depend on a HAL.
 * The only platform decision is where the large buffers are placed, and it is
 * the C62's:
 *
 *  - SRAM is at 80 % of its 256 KiB and every kilobyte counts, so a buffer
 *    that is big and cold goes to the 1,75 MiB of PSRAM the radio carries;
 *  - what is read or written once per sample or per symbol (the matched
 *    filter's history) stays in SRAM, where there are no wait states.
 *
 * The PSRAM section is NOLOAD: the loader does not zero it, so a buffer placed
 * there holds whatever the previous power-on left.  Every one of them is
 * cleared by its own reset path -- dpmrPhy_reset(), dpmrSpeech_start(),
 * OpMode_DPMR::startSampling() -- before the first byte is read, and that is
 * not an optimisation but a requirement of the section.
 */

#ifdef PLATFORM_C62
/** Large, cold buffer in the C62's PSRAM (.psram_section, NOLOAD). */
#define EXT_RAM_BSS_ATTR __attribute__((section(".psram_section")))
#else
/** Large, cold buffer: ordinary .bss where there is no PSRAM. */
#define EXT_RAM_BSS_ATTR
#endif

#endif /* PROTOCOLS_DPMR_DPMRPORT_H */
