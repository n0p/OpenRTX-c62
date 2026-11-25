/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_YSF_TX_H
#define OPENRTX_YSF_TX_H

/**
 * @file YsfTx.h
 * @brief Yaesu System Fusion transmit chain: FICH, data channel, V/D mode 1
 * voice, and the C4FM waveform.
 *
 * A transmission is a header frame, a stream of communication frames and a
 * terminator, 100 ms each (960 bits: a 40-bit sync, the 200-bit FICH and 720
 * bits of payload).  Everything here is the exact inverse of the receiver in
 * YsfFrame.c, which is verified against off-air captures:
 *
 *   FICH  32 information bits (frame indicator, call mode, block and frame
 *         numbers, data type, DG-ID) + CRC-16, four Golay(24,12) codewords, a
 *         rate-1/2 constraint-length-5 convolutional code with four tail bits,
 *         and the 20 x 5 dibit interleaver
 *   DCH   160 bits of data + CRC-16 (+ four tail bits) through the same
 *         convolutional code and a 20 x 9 interleaver, whitened with PN(9,5)
 *   voice V/D mode 1: five slots of 36 CSD dibits + 36 voice dibits, the voice
 *         being the canonical 72-bit AMBE+2 frame (the layout lib/dmr_encoder
 *         produces and lib/dmr_vocoder consumes), unprotected on the air
 *
 * Voice FR (data type 3, full-rate IMBE) is not transmitted: this build has no
 * IMBE encoder, and a V/D mode 1 call is what a Fusion radio does by default.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/YSF/YsfFrame.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Characters in the text message of one transmission (3 x 20). */
#define YSF_TX_TEXT_MAX 60

/** What the transmission announces. */
struct ysfTxConfig {
    char source[YSF_CALLSIGN_LEN + 1]; ///< Own callsign
    char dest[YSF_CALLSIGN_LEN + 1]; ///< Destination: "CQCQCQ" or a DG-ID text
    char uplink[YSF_CALLSIGN_LEN
                + 1]; ///< Uplink node, "DIRECT" for a simplex call
    char downlink[YSF_CALLSIGN_LEN + 1]; ///< Downlink node
    uint8_t dgid;                        ///< DG-ID, 0..99 (0 = all)
    const char *text;                    ///< Slow-data message, may be NULL
};

/**
 * @brief Voice source: fill one 72-bit AMBE+2 channel frame.
 *
 * @param frame: nine bytes, canonical A|B|C order (dmr_encoder_encode output).
 * @return 0 on success, -1 to send a slot of zeros instead.
 */
typedef int (*ysfVoiceSource)(uint8_t frame[9], void *user);

/** Install a voice source (NULL removes it). */
void ysfTx_setVoiceSource(ysfVoiceSource src, void *user);

/** Start a transmission: the next samples are the header frame. */
void ysfTx_begin(const struct ysfTxConfig *cfg);

/** End it: the next frame is the terminator, then silence. */
void ysfTx_end(void);

/** @return true when the terminator has been pumped out. */
bool ysfTx_done(void);

/**
 * @brief Produce modulating waveform samples.
 *
 * @param out: destination, signed 16 bit at 48 kHz.
 * @param count: samples wanted; any count, the symbol carry is internal.
 * @param gain: full-scale units per symbol level (outer level = 3 * gain).
 * @return samples written (always count, zero once the call is over).
 */
int ysfTx_samples(int16_t *out, int count, float gain);

/** @return frames handed to the modulator since ysfTx_begin(), diagnostics. */
uint32_t ysfTx_frames(void);

/**
 * @brief The 480 dibits of the frame that is being transmitted.
 *
 * Diagnostics and the host test: the frame layer is checked bit for bit by
 * feeding these straight to ysfFrame_decode(), which is what takes the
 * modulator out of the comparison.
 */
const uint8_t *ysfTx_frameDibits(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_YSF_TX_H */
