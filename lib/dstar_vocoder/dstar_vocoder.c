/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dstar_vocoder.c
 * @brief The firmware's one AMBE 3600x2400 decoder instance, see
 * dstar_vocoder.h.
 *
 * The frame path is protocol/DSTAR/DStarVocoder.c's front end (air bits ->
 * Golay/PN -> 49 parameter bits and the two error counts) followed by the
 * decoder core, which is what mbelib-neo's process path does in one call.
 */

#include "dstar_vocoder.h"

#include <string.h>

#include "dstar_decoder.h"
#include "protocols/DSTAR/DStarDefs.h"
#include "protocols/DSTAR/DStarVocoder.h"

static dstar_decoder_t decoder DSTAR_VOCODER_STATE_ATTR;

int dstarVocoder_decode(const uint8_t air[DSTAR_VOCODER_FRAME_BYTES],
                        int16_t pcm[DSTAR_VOCODER_PCM_SAMPLES])
{
    if (air == NULL || pcm == NULL)
        return -1;

    uint8_t params[DSTAR_AMBE_BITS];
    int c0Errors = 0, protectedErrors = 0;

    if (dstarVocoder_frame(air, params, &c0Errors, &protectedErrors) != 0)
        return -1;

    dstar_decoder_frame(&decoder, params, c0Errors, protectedErrors, pcm);
    return 0;
}

void dstarVocoder_reset(void)
{
    dstar_decoder_init(&decoder);
}

bool dstarVocoder_available(void)
{
    return true;
}

void dstarVocoder_stats(uint16_t *frames, uint16_t *bad, uint16_t *erasures,
                        uint16_t *tones, uint16_t *muted)
{
    if (frames != NULL)
        *frames = decoder.frames;
    if (bad != NULL)
        *bad = decoder.badFrames;
    if (erasures != NULL)
        *erasures = decoder.erasures;
    if (tones != NULL)
        *tones = decoder.toneFrames;
    if (muted != NULL)
        *muted = decoder.mutedFrames;
}

size_t dstarVocoder_state_bytes(void) { return dstar_decoder_state_size(); }
