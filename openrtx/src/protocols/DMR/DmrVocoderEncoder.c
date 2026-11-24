/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrVocoderEncoder.c
 * @brief The AMBE+2 encoder behind dmrVocoder_encode(), when it is built in.
 *
 * DmrVocoder.c defines the hook weakly: without an encoder the DMR transmit
 * mode refuses to key the PA.  This file overrides it with the real thing,
 * lib/dmr_encoder, and is only compiled with CONFIG_DMR_TX_ENC.
 *
 * Keeping the override in its own file is deliberate: the transmit chain
 * (CONFIG_DMR_TX) and the encoder (CONFIG_DMR_TX_ENC, 63 KiB of flash, 22 % of
 * the M33 when it runs) are separate decisions.  With the encoder off, the
 * chain can still be built and measured, and a platform that gets its frames
 * from the HiFi4 DSP (docs/DMR_TX.md) can provide its own hook.
 */

#include "protocols/DMR/DmrVocoder.h"

#include "dmr_encoder.h"

int dmrVocoder_encode(const int16_t pcm[160], uint8_t frame[9])
{
    return dmr_encoder_encode(pcm, frame);
}
