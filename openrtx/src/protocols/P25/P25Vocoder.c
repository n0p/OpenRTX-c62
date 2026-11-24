/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file P25Vocoder.c
 * @brief IMBE decoder hook, see P25Vocoder.h.
 *
 * Built with CONFIG_P25_VOCODER (CMake option C62_P25_VOCODER), which needs an
 * IMBE backend linked in. The adapter below is written for mbelib-neo
 * (mbe_processImbe4400Data, the decoder the host prototype in work/p25-rx
 * validates bit-exactly against dsd-neo); porting it to op25's imbe_vocoder is
 * the same three calls with array_of_bits/imbe_decode_parameters_4400. Which
 * one to link is the user's call: see docs/RX_P25.md for the licences and the
 * patent family.
 */

#include "protocols/P25/P25Vocoder.h"

#ifdef CONFIG_P25_VOCODER

#include <string.h>

#include <mbelib.h>

/* The decoder is stateful and not reentrant: called from the playback thread
 * only. 4.6 KiB of state, in PSRAM with the rest of the non-per-sample data. */
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
    if (!initialised)
        p25Vocoder_reset();

    /* mbelib wants the 88 bits one per char, in transmission order */
    char bits[88];
    for (int i = 0; i < 88; i++)
        bits[i] = (char)((params[i >> 3] >> (7 - (i & 7))) & 1);

    mbe_process_result result;
    mbe_initProcessResult(&result);
    mbe_processImbe4400Data(pcm, &result, bits, &curParms, &prevParms,
                            &prevEnhanced);

    return (result.errs == 0) ? 0 : -1;
}

#else /* CONFIG_P25_VOCODER */

void p25Vocoder_reset(void)
{
}

bool p25Vocoder_available(void)
{
    return false;
}

int p25Vocoder_decode(const uint8_t params[11],
                      int16_t pcm[P25_VOCODER_PCM_SAMPLES])
{
    (void)params;
    (void)pcm;
    return -1;
}

#endif /* CONFIG_P25_VOCODER */
