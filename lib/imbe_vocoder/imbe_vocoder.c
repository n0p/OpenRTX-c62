/* imbe_vocoder.c -- the firmware-facing single instance of the IMBE decoder.
 *
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Same shape as lib/dmr_vocoder/dmr_vocoder.c: one static instance, no
 * allocation, no locks, reset callable between frames from the audio thread.
 */

#include "imbe_vocoder.h"

#include "imbe_decoder.h"

/* The decoder is stateful and not reentrant: the P25 speech path calls it from
 * its playback thread only.  In the firmware its 11.6 KiB go to PSRAM with the
 * rest of the mode's non-per-sample data, the same section the DMR and D-STAR
 * vocoders use: the region is NOLOAD, so imbe_decoder_init() zeroes the whole
 * instance and nothing depends on the section being cleared at boot.  Moving it
 * to SRAM costs no flash and this file nothing but the attribute, and buys the
 * frame's model accesses internal-RAM speed, against 11.6 KiB of the tight
 * region. */
#ifdef __ZEPHYR__
static imbe_decoder_t dec __attribute__((section(".psram_section")));
#else
static imbe_decoder_t dec;
#endif

static unsigned last_errors;

void imbe_vocoder_reset(void)
{
    imbe_decoder_init(&dec);
    last_errors = 0;
}

int imbe_vocoder_decode(const uint8_t params[IMBE_VOCODER_PARAM_BYTES],
                        int16_t pcm[IMBE_VOCODER_PCM_SAMPLES])
{
    return imbe_vocoder_decode_errs(params, 0, pcm);
}

int imbe_vocoder_decode_errs(const uint8_t params[IMBE_VOCODER_PARAM_BYTES], int nerr,
                             int16_t pcm[IMBE_VOCODER_PCM_SAMPLES])
{
    uint8_t bits[IMBE_VOCODER_PARAM_BITS];
    int r;

    if (params == NULL || pcm == NULL)
        return -1;

    imbe_unpack_params(params, bits);
    last_errors = (nerr > 0) ? (unsigned)nerr : 0u;
    r = imbe_decoder_decode(&dec, bits, nerr, pcm);
    return (r < 0) ? -1 : r;
}

unsigned imbe_vocoder_last_errors(void)
{
    return last_errors;
}

size_t imbe_vocoder_state_bytes(void)
{
    return imbe_decoder_state_size();
}
