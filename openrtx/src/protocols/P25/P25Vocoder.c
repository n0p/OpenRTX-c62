/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file P25Vocoder.c
 * @brief IMBE decoder hook, see P25Vocoder.h.
 *
 * Three builds:
 *   - CONFIG_P25_VOCODER (default): lib/imbe_vocoder, this tree's own decoder
 *     (docs/IMBE.md);
 *   - CONFIG_P25_MBELIB: mbelib-neo's mbe_processImbe4400Data(), its DVSI
 *     10 ms subframe rendering, kept as an option to compare on the radio;
 *   - neither: the hook reports unavailable and the speech path plays silence.
 */

#include "protocols/P25/P25Vocoder.h"

#if defined(CONFIG_P25_VOCODER)

#include <stddef.h>

#include "imbe_vocoder.h"

void p25Vocoder_reset(void)
{
    imbe_vocoder_reset();
}

bool p25Vocoder_available(void)
{
    return true;
}

int p25Vocoder_decode(const uint8_t params[11], int16_t pcm[P25_VOCODER_PCM_SAMPLES])
{
    return p25Vocoder_decode_errs(params, 0, pcm);
}

int p25Vocoder_decode_errs(const uint8_t params[11], int nerr,
                           int16_t pcm[P25_VOCODER_PCM_SAMPLES])
{
    return imbe_vocoder_decode_errs(params, nerr, pcm);
}

#elif defined(CONFIG_P25_MBELIB)

#include <mbelib.h>

/* The reference decoder is stateful and not reentrant: called from the
 * playback thread only. Its three parameter sets are about 4.6 KiB, in PSRAM
 * with the rest of the mode's non-per-sample data. */
#ifdef __ZEPHYR__
static mbe_parms curParms __attribute__((section(".psram_section")));
static mbe_parms prevParms __attribute__((section(".psram_section")));
static mbe_parms prevEnhanced __attribute__((section(".psram_section")));
#else
static mbe_parms curParms;
static mbe_parms prevParms;
static mbe_parms prevEnhanced;
#endif
static bool initialised;

void p25Vocoder_reset(void)
{
    mbe_initMbeParms(&curParms, &prevParms, &prevEnhanced);
    initialised = true;
}

bool p25Vocoder_available(void)
{
    return true;
}

int p25Vocoder_decode(const uint8_t params[11], int16_t pcm[P25_VOCODER_PCM_SAMPLES])
{
    return p25Vocoder_decode_errs(params, 0, pcm);
}

int p25Vocoder_decode_errs(const uint8_t params[11], int nerr,
                           int16_t pcm[P25_VOCODER_PCM_SAMPLES])
{
    char bits[88];

    if (!initialised)
        p25Vocoder_reset();

    /* mbelib wants the 88 bits one per char, in transmission order */
    for (int i = 0; i < 88; i++)
        bits[i] = (char)((params[i >> 3] >> (7 - (i & 7))) & 1);

    mbe_process_result result;
    mbe_initProcessResult(&result);
    /* The parameter-bits entry point has no C0 count, so the error total is
     * what the repeat and mute decisions see (imbe4400_repeat_required). */
    result.total_errors = (nerr > 0) ? nerr : 0;
    mbe_processImbe4400Data(pcm, &result, bits, &curParms, &prevParms, &prevEnhanced);

    return (result.total_errors > 5) ? 1 : 0;
}

#else /* no backend */

void p25Vocoder_reset(void)
{
}

bool p25Vocoder_available(void)
{
    return false;
}

int p25Vocoder_decode(const uint8_t params[11], int16_t pcm[P25_VOCODER_PCM_SAMPLES])
{
    (void)params;
    (void)pcm;
    return -1;
}

int p25Vocoder_decode_errs(const uint8_t params[11], int nerr,
                           int16_t pcm[P25_VOCODER_PCM_SAMPLES])
{
    (void)params;
    (void)nerr;
    (void)pcm;
    return -1;
}

#endif
