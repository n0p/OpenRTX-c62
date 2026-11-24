/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file OpMode_DPMR.cpp
 * @brief dPMR receive mode, see OpMode_DPMR.hpp.
 */

#include <string.h>

#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DPMR/DpmrData.h"
#include "protocols/DPMR/DpmrPhy.h"
#include "protocols/DPMR/DpmrSpeech.h"
#include "rtx/OpMode_DPMR.hpp"
#include "core/utils.h"


/* 30 ms of baseband at 48 kHz, the same half the other flat RX modes take */
#define HALF_SAMPLES 1440

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

static int16_t baseband[2 * HALF_SAMPLES];

void OpMode_DPMR::frameTrampoline(const struct dpmrFrame *frame, void *user)
{
    static_cast<OpMode_DPMR *>(user)->onFrame(frame);
}

void OpMode_DPMR::onFrame(const struct dpmrFrame *frame)
{
    if (frame->cchOk)
        framesOk++;

    if (frame->headerOk) {
        headersOk++;
        calledAddr = frame->header.calledId;
        ownAddr = frame->header.ownId;
        modeCode = frame->header.mode;
        htCode = frame->header.ht;
    }

    if ((frame->sync == DPMR_SYNC_CC) && (frame->channelCode != 0))
        code = frame->channelCode;

    if (frame->endOk) {
        endsOk++;
        /*
         * Table 5.12: only an END frame with ET = 01 carries a status message
         * (clause 5.15); with ET = 00 the five bits are dummy data and saying
         * "status 0" would be a lie.
         */
        if (frame->end.et == 1)
            lastStatus = frame->end.status;
        if (frame->end.wait != 0)
            lastWait = frame->end.wait;
    }

    /*
     * Voice: the TCH of every frame that decoded as voice is four AMBE+2
     * frames, 80 ms of audio, handed to the playback thread.  A frame that
     * did not close leaves a hole: the queue simply gets nothing for it.
     */
    if (frame->voice)
        dpmrSpeech_push(frame->ambe, DPMR_AMBE_FRAMES);

    if (!frame->cchOk)
        return;

    /*
     * Short data.  Mode 001 carries two user bytes per frame behind a
     * continuation flag (clause 5.9.1); mode 101 uses the same 18 bits as the
     * header of a type 2 data message (clause 5.9.2) whose bytes travel in the
     * TCH.  The bytes of 001 are reassembled; the header of 101 is decoded so
     * that the mode can say a data message is coming, but its payload is the
     * data superframe path, which this receiver does not implement.
     */
    if (frame->cch.mode == DPMR_MODE_VOICE_SLD) {
        struct dpmrSlowData d;

        dpmrData_slowDecode(frame->cch.sld, &d);
        dpmrSld_push(&sldMsg, &d);

        if (sldMsg.complete) {
            sldMessages++;
            memcpy(sldText, sldMsg.text, sldMsg.len);
            sldLen = sldMsg.len;
            sldTruncated = sldMsg.overflow;
            dpmrSld_reset(&sldMsg);
        }
    } else if (frame->cch.mode == DPMR_MODE_VOICE_DATA) {
        struct dpmrSld hdr;

        dpmrData_sldDecode(frame->cch.sld, &hdr);
        if (hdr.dp == DPMR_DP_DATA) {
            sldFormat = hdr.format;
            sldLength = hdr.length;
            dataFrames++;
        }
    }
}

OpMode_DPMR::OpMode_DPMR()
{
}

OpMode_DPMR::~OpMode_DPMR()
{
}

void OpMode_DPMR::reset()
{
    settle = SETTLE_SAMPLES;
    framesOk = 0;
    framesLost = 0;
    headersOk = 0;
    endsOk = 0;
    calledAddr = 0;
    ownAddr = 0;
    modeCode = 0xFF;
    htCode = 0xFF;
    code = 0;
    dataFrames = 0;
    sldLen = 0;
    sldTruncated = false;
    sldMessages = 0;
    sldFormat = 0;
    sldLength = 0;
    lastStatus = 0xFF;
    lastWait = 0;
    dpmrSld_reset(&sldMsg);
    dpmrPhy_reset();
}

void OpMode_DPMR::enable()
{
    dpmrPhy_init();
    reset();
    dpmrSpeech_start();
    startRx = true;
}

void OpMode_DPMR::disable()
{
    startRx = false;
    stopSampling();
    dpmrSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
}

void OpMode_DPMR::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   48000, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_DPMR::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

bool OpMode_DPMR::rxSquelchOpen()
{
    return dpmrPhy_synced();
}

void OpMode_DPMR::update(rtxStatus_t *const rtx, const bool newCfg)
{
    if (newCfg || (rtx->opStatus == OFF)) {
        reset();
        if (startRx) {
            rtx->opStatus = RX;
            return;
        }
        sleepFor(0, 30);
        return;
    }

    /* dPMR is receive only: the PTT does nothing here */
    if (rtx->opStatus == TX) {
        rtx->opStatus = RX;
        return;
    }

    if (!startRx)
        return;

    if (basebandId < 0)
        startSampling();

    if (basebandId < 0) {
        sleepFor(0, 20);
        return;
    }

    dataBlock_t block = inputStream_getData(basebandId);
    if ((block.data == NULL) || (block.len < HALF_SAMPLES)) {
        sleepFor(0, 5);
        return;
    }

    if (settle > 0) {
        settle = (settle > HALF_SAMPLES) ? (settle - HALF_SAMPLES) : 0;
        return;
    }

    bool synced = dpmrPhy_synced();

    dpmrPhy_push(block.data, HALF_SAMPLES, frameTrampoline, this);

    /* A transmission that ended: the grid timed out, count the call as lost */
    if (synced && !dpmrPhy_synced()) {
        framesLost++;
        platform_ledOff(GREEN);
    } else if (dpmrPhy_synced()) {
        platform_ledOn(GREEN);
    }
}
