/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DSTAR_VOCODER_H
#define OPENRTX_DSTAR_VOCODER_H

/**
 * @file DStarVocoder.h
 * @brief D-STAR voice: the AMBE 3600x2400 channel frame, and why the DMR
 * AMBE+2 decoder cannot be used for it.
 *
 * A D-STAR DV frame carries 72 voice bits per 20 ms, and those 72 bits are the
 * AMBE-2020 *channel* frame of the full-rate (2400 bit/s) codec: JARL STD-7
 * 4.1.2 b says "a 72-bit voice signal ... in accordance with the AMBE-2020
 * (with FEC) standard".  The FEC is inside the 72 bits, not around them:
 *
 *   bits  0..23  Golay(24,12) codeword: 12 information bits + 11 check bits
 *                + one overall parity bit (the c0 word, the pitch)
 *   bits 24..47  Golay(23,12) codeword of c1, XORed with a pseudo-random
 *                sequence seeded by c0 (LCG 173x + 13849, 16*c0)
 *   bits 48..71  24 unprotected bits: 11 bits c2 + 14 bits c3 (higher DCT
 *                coefficients and the gain word)
 *
 * The DMR path in lib/dmr_vocoder decodes AMBE+2 3600x2450, which is a
 * different codec: same 72-bit geometry (24 + 23 + 25) and the same Golay and
 * LCG primitives, but a 49-bit parameter payload with a different bit
 * allocation and a different set of quantiser tables (AMBE+2 halves the
 * spectral-amplitude rate by predicting from the previous frame).  Feeding a
 * D-STAR frame to it produces intelligible-looking noise, not speech: the
 * segmentation does *not* carry over, only the FEC does.
 *
 * This module owns the part that is D-STAR specific and cheap: the air-bit
 * de-interleave, the Golay/PN correction and the packing into the 49
 * parameter bits of MBE 2400, verified bit for bit against mbelib-neo
 * (tests/host/dstar/dstar_vocoder_test.c).  Decoding those 49 bits to PCM
 * needs the 2400 parameter decoder and its tables, which is the piece that does
 * not fit today -- see docs/RX_DSTAR.md for the measurement.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/DSTAR/DStarDefs.h"
#include "protocols/DSTAR/DStarFec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** AMBE 3600x2400 parameter bits per frame. */
#define DSTAR_AMBE_BITS 49

/** PCM samples per frame, 20 ms at 8 kHz. */
#define DSTAR_PCM_SAMPLES 160

/** Sample rate of the decoded audio, in Hz. */
#define DSTAR_VOCODER_SAMPLE_RATE 8000

/**
 * @brief Unpack one 72-bit D-STAR voice frame into 49 AMBE parameter bits.
 *
 * @param air: the frame, air order (bit 0 of byte 0 is the first on air).
 * @param params: 49 parameter bits, one per byte, MSB first (the layout
 *                mbelib's mbe_decodeAmbe2400Parms() expects).
 * @param errors: optional, receives the number of corrected channel bits.
 * @return 0 on success.
 */
int dstarVocoder_params(const uint8_t air[DSTAR_VOICE_BYTES],
                        uint8_t params[DSTAR_AMBE_BITS], int *errors);

/**
 * @brief Decode one voice frame to PCM.
 *
 * Weak hook, like dmrVocoder_encode(): a platform that has the AMBE 2400 core
 * overrides it.  The default returns -1, which is what the playback thread
 * turns into silence.
 *
 * @return 0 on success, -1 when no decoder is linked.
 */
int dstarVocoder_decode(const uint8_t air[DSTAR_VOICE_BYTES],
                        int16_t pcm[DSTAR_PCM_SAMPLES]);

/** @return true when a 2400 decoder is linked in (i.e. audio will play). */
bool dstarVocoder_available(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DSTAR_VOCODER_H */
