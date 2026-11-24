/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_VOCODER_H
#define OPENRTX_DMR_VOCODER_H

/**
 * @file DmrVocoder.h
 * @brief The AMBE+2 encoder the DMR transmitter needs, as a link-time hook.
 *
 * The decoder is part of the firmware (lib/dmr_vocoder, 112 k instructions per
 * frame).  The encoder is not: the only complete implementation available
 * (OpenDMR's, derived from OP25) costs 4,19 M instructions per 20 ms frame on
 * the M33, i.e. 84-112 % of the CPU, and 2,08 M with cross-unit inlining
 * (-O3 -flto), 42-55 %.  Neither leaves room for a radio that also receives.
 *
 * So the transmit mode calls this hook, and the platform decides who fills it:
 *
 *  - the HiFi4 service (see docs/DMR_TX.md, phase 3): the frame arrives over
 *    LSF/URPC and this function just hands it back;
 *  - or an optimised port of the encoder;
 *  - or nothing at all (the weak default), in which case OpMode_DMR refuses to
 *    key the transmitter and says why, instead of transmitting garbage.
 *
 * The contract is the vocoder's: 160 samples of 8 kHz PCM in, one 72-bit
 * AMBE+2 frame out in the canonical A|B|C order the decoder uses.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encode one AMBE+2 frame.
 *
 * @param pcm: 160 samples at 8 kHz, signed 16 bit.
 * @param frame: destination, 9 bytes (72 bits), canonical A|B|C order.
 * @return 0 on success, a negative error code if no encoder is available.
 */
int dmrVocoder_encode(const int16_t pcm[160], uint8_t frame[9]);

/**
 * @return true if an encoder is linked in and ready.  The transmit mode asks
 *         this before keying the PA.
 */
bool dmrVocoder_available(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_VOCODER_H */
