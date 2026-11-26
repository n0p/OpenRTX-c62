/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_NXDN_SPEECH_H
#define OPENRTX_NXDN_SPEECH_H

/**
 * @file NxdnSpeech.h
 * @brief NXDN voice playback: the AMBE+2 frames of a decoded frame queued by
 * the NXDN mode, decoded by dmr_vocoder_decode() on a playback thread and
 * played at 8 kHz.
 *
 * A NXDN frame carries up to four voice channels, i.e. 80 ms of audio, and the
 * vocoder costs about 3 ms of the Cortex-M33 per 20 ms frame, so the decode
 * does not run in the rtx thread.
 *
 * The vocoder is the DMR one (lib/dmr_vocoder) and that is not a shortcut: a
 * NXDN voice channel is one AMBE+2 3600x2450 frame, the same 49 information
 * bits in the same 72-bit layout DMR transmits, so there is nothing to reseg-
 * ment.  See docs/RX_NXDN.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/NXDN/NxdnDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the playback thread. */
void nxdnSpeech_start(void);

/** Stop the playback thread and release the speaker. */
void nxdnSpeech_stop(void);

/**
 * @brief Queue the voice frames of one decoded frame, in order.
 *
 * @param frames frameCount 9-byte AMBE+2 frames, canonical A|B|C order.
 * @param count  number of frames, 0 to NXDN_VCH_PER_FRAME.
 */
void nxdnSpeech_push(const uint8_t frames[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES],
                     unsigned count);

/** Playback statistics since the mode was enabled, for the diagnostics. */
void nxdnSpeech_stats(uint32_t *queued, uint32_t *played, uint32_t *dropped,
                      uint32_t *underruns, uint32_t *bursts);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_NXDN_SPEECH_H */
