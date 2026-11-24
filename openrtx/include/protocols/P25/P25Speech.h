/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_P25_SPEECH_H
#define OPENRTX_P25_SPEECH_H

/**
 * @file P25Speech.h
 * @brief P25 voice playback: IMBE parameters to 8 kHz audio on the speaker.
 *
 * Same shape as the DMR and TETRA speech paths: the rtx thread queues the nine
 * IMBE frames of each LDU and a playback thread owns the decoder and the
 * speaker, because the vocoder is stateful and not reentrant. The speaker opens
 * when a full LDU is queued (180 ms) and is released after one second without
 * voice.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** IMBE frames in an LDU1 or LDU2. */
#define P25_FRAMES_PER_LDU 9

/**
 * Start the playback thread.
 */
void p25Speech_start(void);

/**
 * Stop the playback thread and release the speaker.
 */
void p25Speech_stop(void);

/**
 * Queue the voice frames of one LDU. Call from the rtx thread.
 *
 * A frame whose FEC failed is queued as silence, so the audio timeline keeps
 * the 20 ms it occupies.
 *
 * @param params: IMBE parameters, one 11-byte frame per entry.
 * @param valid: which frames decoded.
 */
void p25Speech_push(const uint8_t params[P25_FRAMES_PER_LDU][11],
                    const bool valid[P25_FRAMES_PER_LDU]);

/**
 * Playback statistics since the mode was enabled.
 *
 * @param queued: IMBE frames queued.
 * @param played: frames decoded and played.
 * @param dropped: frames dropped on a full queue.
 * @param underruns: output buffers filled with silence because nothing was
 *                   queued.
 */
void p25Speech_stats(uint32_t *queued, uint32_t *played, uint32_t *dropped,
                     uint32_t *underruns);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_P25_SPEECH_H */
