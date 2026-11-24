/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_ENC_H
#define DMR_ENC_H

/**
 * @file dmr_enc.h
 * @brief The AMBE+2 encoder the DMR transmitter feeds from the microphone.
 *
 * One hook, one implementation in the build:
 *
 *  - default: lib/dmr_encoder, ours (1 427 272 instructions per 20 ms frame,
 *    28 - 38 % of the M33 at 300 MHz, CPI 1.2 - 1.6).  Integer analysis,
 *    bit-exact with OpenDMR's encoder.  See docs/DMR_PKG.md, "The vocoders".
 *  - CONFIG_DMR_ENC_OPENDMR (CMake DMR_ENC=ON, off by default) and OpenDMR
 *    present (scripts/c62/fetch_opendmr.sh): opendmr_encode() instead, the
 *    reference at 4 097 626 instructions per frame = 16.4 - 21.9 ms of a
 *    20 ms budget (82 - 109 % of the M33): it does not fit in real time
 *    while the radio also receives.
 *
 * The backend's 9 bytes are canonical (DVSI A|B|C); this hook hands out the
 * 72 bits in the order the burst carries them (ETSI 102 361-1 four-block
 * interleave), which is what dmr_tx_voice() places.  See dmr_enc.c.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @return true when an encoder is linked in. */
bool dmrEnc_available(void);

/** Forget the analysis state: a new call starts here. */
void dmrEnc_reset(void);

/**
 * Encode one 20 ms frame.
 *
 * @param pcm: 160 samples at 8 kHz.
 * @param ambe: the 72 channel bits of the frame, 9 bytes, first transmitted
 *              bit in the MSB of ambe[0] (DMR interleaved order).
 * @return false if there is no encoder or it failed.
 */
bool dmrEnc_encode(const int16_t pcm[160], uint8_t ambe[9]);

#ifdef __cplusplus
}
#endif

#endif /* DMR_ENC_H */
