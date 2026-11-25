/* imbe_vocoder.h -- IMBE 7200x4400 decoder for P25 Phase 1, one instance.
 *
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Turns the 88 vocoder parameters of a 20 ms P25 frame into 160 samples of
 * 8 kHz PCM.  One static instance, no allocation, no locks: call it from the
 * playback thread that owns the state.
 *
 *   imbe_vocoder_reset();
 *   imbe_vocoder_decode(params11, pcm160);   // per frame, in order
 *
 * The core (imbe_decoder.c) is a C port of mbelib-neo's IMBE decoder; see
 * docs/IMBE.md for the verification, the cost and the licence.
 */
#ifndef IMBE_VOCODER_H
#define IMBE_VOCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Packed parameter bytes of one frame (88 bits, MSB first). */
#define IMBE_VOCODER_PARAM_BYTES 11
/** Parameter bits of one frame. */
#define IMBE_VOCODER_PARAM_BITS 88
/** PCM samples of one frame (20 ms at 8 kHz). */
#define IMBE_VOCODER_PCM_SAMPLES 160
/** Output sample rate. */
#define IMBE_VOCODER_SAMPLE_RATE 8000

/** Clear the decoder state: call when a new transmission starts. */
void imbe_vocoder_reset(void);

/**
 * Decode one frame.
 *
 * @param params: 88 parameter bits packed MSB first in 11 bytes, as the P25
 *        LDU carries them after the Golay/Hamming layer.
 * @param pcm: destination, IMBE_VOCODER_PCM_SAMPLES samples at 8 kHz.
 * @return 0 on a voice frame, 1 if the frame was rejected (bad fundamental,
 *         repeated, or muted), -1 on bad arguments.
 */
int imbe_vocoder_decode(const uint8_t params[IMBE_VOCODER_PARAM_BYTES],
                        int16_t pcm[IMBE_VOCODER_PCM_SAMPLES]);

/**
 * Decode one frame whose FEC reported nerr corrected bits.  The count drives
 * the frame-repeat and muting decisions of TIA-102.BABA 7.7-7.8; pass 0 when
 * the channel decoder does not report one.
 */
int imbe_vocoder_decode_errs(const uint8_t params[IMBE_VOCODER_PARAM_BYTES], int nerr,
                             int16_t pcm[IMBE_VOCODER_PCM_SAMPLES]);

/** Corrected-bit count the last decoded frame was given. */
unsigned imbe_vocoder_last_errors(void);

/** Bytes of the decoder's state. */
size_t imbe_vocoder_state_bytes(void);

#endif /* IMBE_VOCODER_H */
