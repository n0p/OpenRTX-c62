/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "core/utils.h"
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/NXDN/NxdnPhy.h"
#include "protocols/NXDN/NxdnSpeech.h"
#include "protocols/NXDN/NxdnStatus.h"
#include "rtx/OpMode_NXDN.hpp"

/* 40 ms of baseband at 48 kHz: half a frame, so a frame arrives in two blocks
 * and the demodulator never waits more than one block on the stream */
#define HALF_SAMPLES 1920

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

static int16_t baseband[2 * HALF_SAMPLES];

void OpMode_NXDN::frameTrampoline(const struct nxdnFrame *frame, void *user)
{
    static_cast<OpMode_NXDN *>(user)->onFrame(frame);
}

void OpMode_NXDN::onFrame(const struct nxdnFrame *frame)
{
    nxdnStatus_frame(frame);

    if (!frame->lichOk)
        return;

    /* The SACCH of a superframe: 18 bits of layer 3 per frame, in the position
     * the SR field announces */
    if (frame->sacchOk) {
        int part = 3 - ((frame->sacchSr >> 6) & 0x03);

        memcpy(&sacchBits[part * 18], frame->sacchBits, 18);
        sacchHave[part] = true;

        if (part == 3) {
            bool complete = sacchHave[0] && sacchHave[1] && sacchHave[2]
                         && sacchHave[3];

            if (complete) {
                struct nxdnSacchMessage msg;

                nxdnFecSacchParse(sacchBits, &msg);
                msg.valid = true;
                nxdnStatusSacchSegment(part, true, &msg);
            }
            memset(sacchHave, 0, sizeof(sacchHave));
        }
    }

    if (frame->frameCount > 0)
        nxdnSpeech_push(frame->frame, frame->frameCount);
}

void OpMode_NXDN::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   48000, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_NXDN::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

bool OpMode_NXDN::rxSquelchOpen()
{
    return nxdnPhy_synced();
}

OpMode_NXDN::OpMode_NXDN()
    : basebandPath(-1), basebandId(-1), settle(0), dataErrors(0),
      startRx(false)
{
    memset(sacchBits, 0, sizeof(sacchBits));
    memset(sacchHave, 0, sizeof(sacchHave));
}

void OpMode_NXDN::enable()
{
    nxdnPhy_init();
    nxdnStatus_reset();
    nxdnSpeech_start();
    memset(sacchHave, 0, sizeof(sacchHave));
    settle = SETTLE_SAMPLES;
    dataErrors = 0;
    startRx = true;
}

void OpMode_NXDN::disable()
{
    startRx = false;
    stopSampling();
    nxdnSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    nxdnStatus_clear();
}

void OpMode_NXDN::update(rtxStatus_t *const rtx, const bool newCfg)
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
    if (newCfg) {
        nxdnPhy_reset();
        nxdnStatus_reset();
        memset(sacchHave, 0, sizeof(sacchHave));
        settle = SETTLE_SAMPLES;
    }

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * Wait for the next half buffer of samples.  A failed read means samples
     * were lost: the symbol stream jumped, so the demodulator has to start
     * over, and there is no point in sleeping on top of the delay.
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0) {
            nxdnPhy_reset();
            nxdnStatus_reset();
        } else {
            sleepFor(0, 10);
        }
        return;
    }
    dataErrors = 0;

    /*
     * Drop what follows a start or a retune: the receiver settles and the
     * level estimates would be taken from its transient.
     */
    if (settle > 0) {
        settle -= (block.len < settle) ? block.len : settle;
        return;
    }

    nxdnPhy_push(block.data, block.len, frameTrampoline, this);

    if (nxdnPhy_synced())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
