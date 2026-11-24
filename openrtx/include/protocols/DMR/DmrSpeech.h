/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_SPEECH_H
#define OPENRTX_DMR_SPEECH_H

/**
 * @file DmrSpeech.h
 * @brief DMR voice playback: AMBE+2 frames queued by the DMR mode, decoded by
 * dmr_vocoder_decode() on a playback thread and played at 8 kHz.
 *
 * Decoding costs about 3 ms of the Cortex-M33 per 20 ms frame, so it does not
 * run in the rtx thread: the mode queues the three 9-byte frames of each voice
 * burst and returns to the demodulator within a few tens of microseconds.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/DMR/DmrDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the playback thread.  Costs a stack allocation, do it when the mode is
 * enabled, not on every burst.
 */
void dmrSpeech_start(void);

/**
 * Stop the playback thread and release the speaker.
 */
void dmrSpeech_stop(void);

/**
 * Queue the voice frames of one burst, in the order they were received.
 *
 * @param frames: frameCount 9-byte AMBE+2 frames, canonical A|B|C order.
 * @param count: number of frames, 0 to DMR_FRAMES_PER_BURST.
 */
void dmrSpeech_push(const uint8_t frames[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES],
                    unsigned count);

/**
 * Playback statistics since the mode was enabled, for the diagnostics.
 *
 * @param queued: frames queued by the demodulator.
 * @param played: frames decoded and played.
 * @param dropped: frames discarded because the queue was full.
 * @param underruns: frames played as silence for want of a queued frame.
 * @param bursts: playback bursts begun (one per transmission).
 */
void dmrSpeech_stats(uint32_t *queued, uint32_t *played, uint32_t *dropped,
                     uint32_t *underruns, uint32_t *bursts);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_SPEECH_H */
