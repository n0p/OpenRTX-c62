/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file OpMode_P25.cpp
 * @brief P25 Phase 1 receive mode, see OpMode_P25.hpp.
 *
 * The rtx thread only moves samples: it reads 20 ms blocks of discriminator
 * audio and hands them to the PHY, whose frame callback updates the status,
 * queues the IMBE frames for the playback thread and drives the LED. Everything
 * that decodes happens in P25Phy.c, so the same code runs on the host
 * (tests/host/p25_phy_host) and in the firmware.
 */

#include <string.h>

#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/P25/P25Defs.h"
#include "protocols/P25/P25Phy.h"
#include "protocols/P25/P25Speech.h"
#include "protocols/P25/P25Status.h"
#include "protocols/P25/P25Vocoder.h"
#include "rtx/OpMode_P25.hpp"

/** 20 ms of discriminator at 48 kHz */
#define BLOCK_SAMPLES 960
/** Samples dropped after starting or retuning: 0.5 s */
#define SETTLE_SAMPLES 24000

/* There is only one P25 mode, so the buffers are static */
static int16_t baseband[2 * BLOCK_SAMPLES];

/**
 * A frame arrived: show it, and hand the voice to the playback thread.
 */
static void onFrame(const struct p25Frame *frame, void *user)
{
    (void)user;

    p25Status_frame(frame);

    if ((frame->duid == P25_DUID_LDU1 || frame->duid == P25_DUID_LDU2) &&
        frame->voiceCount > 0)
        p25Speech_push(frame->voice, frame->voiceValid);
}

OpMode_P25::OpMode_P25()
    : startRx(false)
    , dataErrors(0)
    , settle(SETTLE_SAMPLES)
    , basebandPath(-1)
    , basebandId(-1)
{
}

OpMode_P25::~OpMode_P25()
{
    disable();
}

void OpMode_P25::reset()
{
    settle = SETTLE_SAMPLES;
    p25Phy_reset();
    p25Status_reset();
}

void OpMode_P25::enable()
{
    reset();
    p25Speech_start();
    startRx = true;
}

void OpMode_P25::disable()
{
    startRx = false;
    stopSampling();
    p25Speech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    p25Status_clear();
}

void OpMode_P25::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * BLOCK_SAMPLES,
                                   48000, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_P25::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

void OpMode_P25::update(rtxStatus_t *const rtx, const bool newCfg)
{
    switch (rtx->opStatus) {
        case OFF:
            radio_disableRtx();
            if (startRx) {
                rtx->opStatus = RX;
                return;
            }
            sleepFor(0, 30);
            return;

        case RX:
            break;

        default:
            /* Receive only */
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

    if (startRx) {
        startSampling();
        radio_enableRx();
        startRx = false;
    }

    /* A new frequency is a new site: start from scratch */
    if (newCfg) {
        reset();
        radio_updateConfiguration();
    }

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * Wait for the next half buffer. A failed read means samples were lost:
     * the symbol stream jumped, so restart the search rather than decode on a
     * stale grid.
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0)
            p25Phy_reset();
        else
            sleepFor(0, 10);
        return;
    }
    dataErrors = 0;

    /* Drop what follows a start or a retune: the receiver settles and the
     * demodulator would take its transient for a symbol stream */
    if (settle > 0) {
        settle -= (block.len < settle) ? block.len : settle;
        return;
    }

    p25Phy_push(block.data, block.len, onFrame, NULL);

    if (p25Status_sync() || p25Phy_synced())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
