/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_vocoder.c
 * @brief Firmware-facing API of the DMR AMBE+2 vocoder (single instance).
 *
 * The decoder lives in dmr_decoder.c, which keeps its state in a caller-owned
 * object so that it can be instantiated more than once (the host tests decode
 * two streams side by side).  The firmware only ever needs one: this file owns
 * it and implements dmr_vocoder.h on top, so the DMR mode never sees
 * dmr_decoder.h.
 */

#include <stddef.h>

#include "dmr_vocoder.h"
#include "dmr_decoder.h"

/** The one decoder instance of the firmware: ~8.5 KiB of .bss, no initialisers. */
static dmr_vocoder_t dmr_instance;

static unsigned int dmr_last_errors;
static int dmr_ready;

int dmr_vocoder_decode(const uint8_t frame[DMR_VOCODER_FRAME_BYTES],
                       int16_t pcm[DMR_VOCODER_PCM_SAMPLES])
{
    int errs = 0;
    int rc;

    if (frame == NULL || pcm == NULL)
        return -1;

    if (dmr_ready == 0) {
        dmr_vocoder_init(&dmr_instance);
        dmr_ready = 1;
    }

    rc = dmr_vocoder_decode_frame(&dmr_instance, frame, pcm, &errs);
    if (rc == 0)
        dmr_last_errors = (unsigned int)errs;
    return rc;
}

void dmr_vocoder_reset(void)
{
    dmr_vocoder_init(&dmr_instance);
    dmr_ready = 1;
    dmr_last_errors = 0;
}

unsigned int dmr_vocoder_last_errors(void)
{
    return dmr_last_errors;
}

int dmr_vocoder_decode_ambe(const uint8_t d[49], int16_t pcm[DMR_VOCODER_PCM_SAMPLES])
{
    int errs = 0;
    int rc;

    if (d == NULL || pcm == NULL)
        return -1;

    if (dmr_ready == 0) {
        dmr_vocoder_init(&dmr_instance);
        dmr_ready = 1;
    }

    /* The caller's channel already carried out the FEC: no bits corrected
     * here, and the counter keeps what the last protected frame reported. */
    rc = dmr_vocoder_decode_ambe_params(&dmr_instance, d, pcm, &errs);
    return rc;
}

size_t dmr_vocoder_state_bytes(void)
{
    return dmr_vocoder_state_size();
}

size_t dmr_vocoder_arena_used(void)
{
    return 0;
}
