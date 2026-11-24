/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_encoder.c
 * @brief Firmware-facing contract of the AMBE+2 encoder (one instance).
 *
 * The work is split in two halves that do not share state: the fixed-point
 * analysis (dmr_enc_core.c) turns 160 samples into model parameters, and the
 * quantiser (dmr_enc_frame.c) turns those into the 72-bit frame and updates
 * its own predictor.  This file is the only thing that knows both, and it is
 * what the DMR transmit mode calls through the dmrVocoder hook.
 */

#include "dmr_encoder.h"
#include "dmr_enc_int.h"

static int encoderReady;

void dmr_encoder_reset(void)
{
    dmr_enc_analysis_reset();
    dmr_enc_frame_reset();
    encoderReady = 1;
}

int dmr_encoder_encode(const int16_t pcm[DMR_ENCODER_PCM_SAMPLES],
                       uint8_t frame[DMR_ENCODER_FRAME_BYTES])
{
    static imbe_param_t param;

    if (pcm == NULL || frame == NULL)
        return -1;

    if (!encoderReady)
        dmr_encoder_reset();

    dmr_enc_analysis(&param, pcm);
#ifdef DMR_ENC_DEBUG
    dmr_enc_dbg_param = param;
#endif
    dmr_enc_frame(&param, frame);
    return 0;
}

size_t dmr_encoder_state_bytes(void)
{
    return dmr_enc_state_bytes();
}
