/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_glue.c
 * @brief The radio's DMR settings records, see dmr_glue.h.
 */

#include <stddef.h>
#include "core/datatypes.h"
#include "dmr_glue.h"
#include "dmr_sms.h"

/*
 * Air delay of this audio path, microseconds, computed from its code:
 *
 *  30 000 us  one half of the output stream (2 x 1440 samples at 48 kHz,
 *             BUF_CIRC_DOUBLE): a slot written now starts playing when the
 *             other half ends, one 30 ms slot from now.
 *  10 000 us  one DSP transport frame (csk6_codec.c, CAPTURE_FRAME_MS = 10):
 *             the half is handed to the DSP at the next 480-sample boundary,
 *             0..10 ms.
 * --------
 *  40 000 us  deterministic part, uncertainty one transport frame (-0 / +10 ms).
 *
 * Not in the code: the DSP's own output queue and the DAC, which this driver
 * cannot read back (no output-time call in csk6_codec.h, unlike the tree the
 * package comes from).  Their loopback probe measured 12.4 ms for the same
 * worker and image family, so the real figure is expected around 50 ms; that
 * is what the persistent trim is for.  docs/DMR_PKG.md, "Transmit air delay",
 * has the bench procedure and what to re-measure when the DSP image changes.
 */
#define DMR_AIR_DELAY_US_MEASURED 40000

dmrRadioSettings_t c62_dmr = {
    .id = 0,
    .cc = 0,
    .slot = 1,
    .tg = 0,
    .privateCall = false,
    .txAirDelayUs = DMR_AIR_DELAY_US_MEASURED,
    .txAirDelayAuto = false,
    .txFlip = false,
};

dmrT3Settings_t c62_t3 = {
    .mode = C62_T3_OFF,
    .followAll = false,
    .baseHz = 0,
    .first = 1,
    .sep = 0,
    .txLow = false,
    .splitHz = 0,
    .ctrlHz = { 0, 0, 0 },
};

void dmrGlue_reset(void)
{
    c62_dmr.id = 0;
    c62_dmr.cc = 0;
    c62_dmr.slot = 1;
    c62_dmr.tg = 0;
    c62_dmr.privateCall = false;
    c62_dmr.txAirDelayUs = DMR_AIR_DELAY_US_MEASURED;
    c62_dmr.txAirDelayAuto = false;
    c62_dmr.txFlip = false;

    c62_t3.mode = C62_T3_OFF;
    c62_t3.followAll = false;
    c62_t3.baseHz = 0;
    c62_t3.first = 1;
    c62_t3.sep = 0;
    c62_t3.txLow = false;
    c62_t3.splitHz = 0;
    for (unsigned i = 0; i < C62_T3_CTRL_MORE; i++)
        c62_t3.ctrlHz[i] = 0;
}

int32_t dmrGlue_airDelayUs(void)
{
    return c62_dmr.txAirDelayUs;
}

void dmrGlue_setAirDelayUs(int32_t us)
{
    if (us < 0)
        us = 0;
    if (us > 60000)
        us = 60000;
    c62_dmr.txAirDelayUs = us;
}

/* One TDMA frame: 2 slots of 30 ms at 48 kHz */
#define DMR_FRAME_SAMPLES 2880

uint32_t dmrGlue_preRollSamples(void)
{
    int32_t samples = (int32_t)(((int64_t)c62_dmr.txAirDelayUs * 48) / 1000);

    samples %= DMR_FRAME_SAMPLES;
    if (samples < 0)
        samples += DMR_FRAME_SAMPLES;
    return (uint32_t)samples;
}

uint8_t dmrGlue_smsFormat(uint8_t setting)
{
    switch (setting) {
        case DMR_SMS_FMT_TMS:
            return DMR_SMS_TMS;

        /* The 5016 family: same UDP port, so delivery is the package's IP
         * text whatever flavour the receiver expects */
        case DMR_SMS_FMT_AUTO:
        case DMR_SMS_FMT_IP_5016:
        case DMR_SMS_FMT_IP_HC:
        case DMR_SMS_FMT_ANYTONE:
            return DMR_SMS_ETSI_IP;

        /* Defined short data: the package's ISO 8859-1 replaces the UTF and
         * Hytera payloads of these settings */
        case DMR_SMS_FMT_DD_UTF16BE:
        case DMR_SMS_FMT_DD_UTF8:
        case DMR_SMS_FMT_HYTERA:
        default:
            return DMR_SMS_ETSI_DD;
    }
}
