/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DSTAR_SPEECH_H
#define OPENRTX_DSTAR_SPEECH_H

/**
 * @file DStarSpeech.h
 * @brief D-STAR voice playback: a small queue filled by the rtx thread and a
 * thread that owns the vocoder and the speaker.
 *
 * Same shape as the DMR path (DmrSpeech.h) and for the same reason: the
 * vocoder is stateful and not reentrant, so it is called from one thread only.
 * When no AMBE 3600x2400 decoder is linked in (dstarVocoder_decode() is the
 * weak default), the thread drains its queue and plays nothing: the mode still
 * decodes callsigns, messages and positions, which is what it can do today.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/DSTAR/DStarDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Queue depth: 32 frames are 640 ms, enough to ride out a lost frame. */
#define DSTAR_SPEECH_QUEUE 32

/** Start the playback thread.  Idempotent. */
void dstarSpeech_start(void);

/** Stop the playback thread and close the speaker. */
void dstarSpeech_stop(void);

/** Queue one 72-bit voice frame (rtx thread, does not decode). */
void dstarSpeech_push(const uint8_t frame[DSTAR_VOICE_BYTES]);

/** Diagnostics: frames queued, played, dropped, and queue underruns. */
void dstarSpeech_stats(uint32_t *queued, uint32_t *played, uint32_t *dropped);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DSTAR_SPEECH_H */
