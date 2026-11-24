/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_YSF_SPEECH_H
#define OPENRTX_YSF_SPEECH_H

/**
 * @file YsfSpeech.h
 * @brief System Fusion voice playback: AMBE+2 frames in, speaker out.
 *
 * Same shape as the DMR and TETRA speech paths: the RTX thread pushes voice
 * slots into a small queue, and a playback thread owns the decoder and the
 * audio output, because the vocoder is stateful and not reentrant.
 *
 * Both System Fusion voice formats go through the one AMBE+2 decoder of
 * lib/dmr_vocoder: V/D mode 2 hands it the 49 parameter bits, V/D mode 1 the
 * 72-bit channel frame it already unstuffs for DMR.
 */

#include <stdbool.h>

#include "protocols/YSF/YsfFrame.h"

/** Start the playback thread.  Idempotent. */
void ysfSpeech_start(void);

/** Stop the playback thread and close the speaker. */
void ysfSpeech_stop(void);

/** Drop everything queued, keeping the decoder warm (a gap, not a new call). */
void ysfSpeech_flush(void);

/** Reset the decoder as well: the next transmission starts from scratch. */
void ysfSpeech_reset(void);

/**
 * Queue decoded voice slots for playback.  Called from the RTX thread.
 *
 * @param slots: array of slots, in transmission order.
 * @param count: how many.
 */
void ysfSpeech_push(const struct ysfVoiceSlot *slots, unsigned count);

/** Frames played since start, and frames dropped for want of room. */
void ysfSpeech_stats(unsigned *played, unsigned *dropped);

#endif /* OPENRTX_YSF_SPEECH_H */
