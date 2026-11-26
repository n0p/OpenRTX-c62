/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_encoder.h
 * @brief DMR AMBE+2 encoder, transmit half of lib/dmr_vocoder.
 *
 * 160 samples of 8 kHz PCM in, one 72-bit AMBE+2 frame out (9 bytes, canonical
 * A|B|C order), i.e. exactly what lib/dmr_vocoder decodes and what the DMR
 * transmit chain of the firmware expects from the vocoder hook.
 *
 * This is a C port of the encoder half of OpenDMR (OP25's MBE encoder plus
 * Pavel Yazev's fixed-point IMBE analysis), rearranged for a small MCU:
 *
 *  - one static instance: no heap, no C++, no thread-local storage;
 *  - the analysis is integer only and lives in one translation unit, so the
 *    ETSI basic operators inline (that was half of the reference's cost);
 *  - the Golay codewords are computed, not looked up: 32 KiB of tables fewer;
 *  - cosines and logs are const tables in flash; the only libm call left is
 *    log2f() on the spectral amplitudes (one per harmonic per frame);
 *  - no floating point in the analysis at all, single precision in the
 *    quantiser, `-ffp-contract=off` so that the FMAd and non-FMAd builds
 *    produce the same bits.
 *
 * Verified bit-exact against the reference encoder (libopendmr) on 1000
 * frames of speech, and closed through the firmware's own receiver: see
 * docs/DMR_TX.md and work/dmr-tx/enc-c.
 */

#ifndef DMR_ENCODER_H
#define DMR_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Samples per AMBE+2 frame (20 ms at 8 kHz). */
#define DMR_ENCODER_PCM_SAMPLES 160

/** Bytes per AMBE+2 frame. */
#define DMR_ENCODER_FRAME_BYTES 9

/**
 * @brief Encode one frame.
 *
 * The encoder keeps state between calls (pitch history, predictor, DC blocker),
 * so consecutive frames of a transmission must be encoded in order.  The state
 * is initialised on the first call; dmr_encoder_reset() before a transmission
 * is optional but recommended.
 *
 * @param pcm   DMR_ENCODER_PCM_SAMPLES samples, signed 16 bit.
 * @param frame Output, DMR_ENCODER_FRAME_BYTES bytes.
 * @return 0 on success, -1 on invalid arguments.
 */
int dmr_encoder_encode(const int16_t pcm[DMR_ENCODER_PCM_SAMPLES],
                       uint8_t frame[DMR_ENCODER_FRAME_BYTES]);

/**
 * @brief Reset the encoder state (start of a transmission).
 */
void dmr_encoder_reset(void);

/**
 * @brief Static RAM used by the encoder state (.bss, no initialisers).
 */
size_t dmr_encoder_state_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_ENCODER_H */
