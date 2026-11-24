/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/YSF/YsfFrame.h"
#include "protocols/YSF/YsfPhy.h"
#include "protocols/YSF/YsfSpeech.h"
#include "rtx/OpMode_YSF.hpp"
#include "core/state.h"

/*
 * 100 ms of baseband at 48 kHz: one System Fusion frame exactly.  The PHY
 * takes any block size, but the stream half is one frame on purpose: the frame
 * clock then lands on a buffer boundary and the decoder never has to catch up
 * with a burst of frames read from a ring that has already wrapped.
 */
#define BLOCK_SAMPLES 4800

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

static int16_t baseband[2 * BLOCK_SAMPLES];

void OpMode_YSF::frameTrampoline(const struct ysfFrame *frame, void *user)
{
    static_cast<OpMode_YSF *>(user)->onFrame(frame);
}

/**
 * One decoded frame.  Runs in the rtx thread, which must not decode audio
 * here: the voice slots go to the playback thread.
 */
void OpMode_YSF::onFrame(const struct ysfFrame *frame)
{
    const struct ysfDecoded *decoded = ysfFrame_decode(frame);

    if (decoded == NULL)
        return;

    if (decoded->voiceCount > 0)
        ysfSpeech_push(decoded->voice, decoded->voiceCount);

    /* A transmission that ends must not leave its last 400 ms playing over the
     * next one; the playback thread closes the speaker on its own. */
    if (decoded->fichOk && (ysfFrame_call()->fi == 2))
        ysfSpeech_flush();
}

OpMode_YSF::OpMode_YSF()
{
    basebandPath = -1;
    basebandId = -1;
    startRx = true;
    settle = SETTLE_SAMPLES;
    dataErrors = 0;
}

OpMode_YSF::~OpMode_YSF()
{
    disable();
}

void OpMode_YSF::reset()
{
    settle = SETTLE_SAMPLES;
    dataErrors = 0;
    ysfPhy_reset();
    ysfFrame_reset();
    ysfSpeech_reset();
}

void OpMode_YSF::enable()
{
    ysfPhy_init();
    reset();
    ysfSpeech_start();
    startRx = true;
}

void OpMode_YSF::disable()
{
    startRx = false;
    stopSampling();
    ysfSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
}

void OpMode_YSF::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * BLOCK_SAMPLES,
                                   48000, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_YSF::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

bool OpMode_YSF::rxSquelchOpen()
{
    return ysfPhy_synced();
}

void OpMode_YSF::update(rtxStatus_t *const rtx, const bool newCfg)
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
            /* Receive only: PTT is refused here */
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

    if (startRx) {
        startSampling();
        radio_enableRx();
        startRx = false;
    }

    /* A new frequency is a new channel: start from scratch */
    if (newCfg)
        reset();

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * A failed read means samples were lost: the symbol stream jumped, so the
     * demodulator starts over instead of coasting on a broken cadence.
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0) {
            ysfPhy_reset();
            ysfFrame_reset();
        } else {
            sleepFor(0, 10);
        }
        return;
    }
    dataErrors = 0;

    if (settle > 0) {
        settle -= (block.len < settle) ? block.len : settle;
        return;
    }

    ysfPhy_push(block.data, block.len, frameTrampoline, this);

    if (ysfPhy_synced())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
