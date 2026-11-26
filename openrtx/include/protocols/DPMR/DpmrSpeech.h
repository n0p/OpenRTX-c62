/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTOCOLS_DPMR_DPMRSPEECH_H
#define PROTOCOLS_DPMR_DPMRSPEECH_H

/**
 * @file DpmrSpeech.h
 * @brief dPMR voice playback: AMBE+2 frames in, speaker out.
 *
 * Same shape as the DMR path (protocols/DMR/DmrSpeech.h): the RTX thread
 * queues the AMBE+2 frames of a TCH field and a playback thread owns the
 * vocoder and the speaker.  The vocoder is stateful and not reentrant, so it is
 * only ever called from the playback thread.
 *
 * The vocoder is the same one DMR uses, and dPMR needs nothing else from it:
 * the TCH of a dPMR frame is four 72 bit AMBE+2 3600x2450 frames, the format
 * lib/dmr_vocoder decodes.  dpmrFec_voiceFrame() undoes dPMR's own 4-way
 * interleave and leaves the canonical A|B|C order, which the unit test checks
 * byte for byte against the DMR de-interleaver.
 */

#include <stdbool.h>
#include "protocols/DPMR/DpmrDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the playback thread. */
void dpmrSpeech_start(void);
/** Stop the playback thread. */
void dpmrSpeech_stop(void);

/**
 * Queue the AMBE+2 frames of one frame's TCH.
 *
 * @param frames array of DPMR_AMBE_FRAMES frames, DPMR_AMBE_BYTES each
 * @param count  frames to queue (at most DPMR_AMBE_FRAMES)
 */
void dpmrSpeech_push(const uint8_t frames[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES],
                     unsigned count);

/** Counters, for the diagnostics screen. */
void dpmrSpeech_stats(uint32_t *queued, uint32_t *played, uint32_t *dropped,
                      uint32_t *underruns);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOLS_DPMR_DPMRSPEECH_H */
