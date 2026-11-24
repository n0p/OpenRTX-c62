/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file OpMode_DSTAR.cpp
 * @brief D-STAR DV mode handler, see OpMode_DSTAR.hpp.
 *
 * The mode moves buffers: the demodulator and the transmit chain live in
 * protocols/DSTAR (C, host-tested), and the audio paths are the same ones the
 * DMR mode uses -- the flat discriminator on receive, the same baseband buffer
 * on transmit, because the BK4819 is half duplex and both modes key it the same
 * way.
 */

#include <stdio.h>
#include <string.h>

#include "core/settings.h"
#include "core/state.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DSTAR/DStarPhy.h"
#include "protocols/DSTAR/DStarSpeech.h"
#include "protocols/DSTAR/DStarStatus.h"
#ifdef CONFIG_DSTAR_TX
#include "protocols/DSTAR/DStarTx.h"
#endif
#include "rtx/OpMode_DSTAR.hpp"

#ifdef CONFIG_DSTAR_MESSAGING
#include "core/messaging.h"
#endif

/** Half of the receive buffer: 30 ms of discriminator at 48 kHz. */
#define HALF_SAMPLES 1440

/** Samples dropped after starting or retuning: 100 ms. */
#define SETTLE_SAMPLES 4800

/* Deviation scale handed to the FM exciter.  The BK4819 maps the baseband to
 * frequency with the same REG_40 calibration the DMR transmitter uses, and that
 * number is still a bench item: docs/DSTAR_TX.md says how to calibrate it and
 * what the specification asks for (h = 0.5, so 1200 Hz of peak deviation at
 * 4800 bit/s, inside a 6.25 kHz channel). */
#define DSTAR_TX_AMPLITUDE 24000

OpMode_DSTAR::OpMode_DSTAR() :
    basebandPath(-1),
    basebandId(-1),
    txPath(-1),
    txId(-1),
    startRx(false),
    txBlocked(false),
    txEnded(false),
    txFrames(0),
    audioBuffer()
{
    memset(audioBuffer, 0, sizeof(audioBuffer));
}

OpMode_DSTAR::~OpMode_DSTAR()
{
}

/* ------------------------------------------------------------- callbacks --- */

void OpMode_DSTAR::headerTrampoline(const struct dstarHeader *hdr, int syncErrors,
                                    void *user)
{
    static_cast<OpMode_DSTAR *>(user)->onHeader(hdr, syncErrors);
}

void OpMode_DSTAR::frameTrampoline(const struct dstarPhyFrame *frame, void *user)
{
    static_cast<OpMode_DSTAR *>(user)->onFrame(frame);
}

void OpMode_DSTAR::eotTrampoline(void *user)
{
    static_cast<OpMode_DSTAR *>(user)->onEot();
}

void OpMode_DSTAR::onHeader(const struct dstarHeader *hdr, int syncErrors)
{
    dstarStatus_header(hdr, syncErrors, (uint32_t)getTick());
}

void OpMode_DSTAR::onFrame(const struct dstarPhyFrame *frame)
{
    if (!frame->sync)
        dstarSpeech_push(frame->voice);
    dstarStatus_frame(frame, (uint32_t)getTick());
}

void OpMode_DSTAR::onEot()
{
    /* The transmission ended: drop the vocoder state so the next call does not
     * inherit this talker's pitch and phase. */
}

/* ---------------------------------------------------------------- audio --- */

void OpMode_DSTAR::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, audioBuffer, 2 * HALF_SAMPLES, 48000,
                                   STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_DSTAR::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

/* ------------------------------------------------------------------- tx --- */

#ifdef CONFIG_DSTAR_TX

bool OpMode_DSTAR::startTx(const rtxStatus_t *status)
{
    struct dstarTxConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Flag 1: voice, and "via repeater" only when a repeater is configured. */
    cfg.hdr.flags[0] = 0;
    snprintf(cfg.hdr.my, sizeof(cfg.hdr.my), "%.8s", state.settings.callsign);
    cfg.hdr.sfx[0] = '\0';

    if ((status->rxFrequency != status->txFrequency) && (status->txFrequency != 0)) {
        /* Repeater operation: the channel's shift says which repeater this is,
         * but the firmware has no repeater database, so the operator's own
         * callsign stands in for it and the call is marked as going through a
         * repeater.  Editing RPT1/RPT2 is a settings/UI task: docs/RX_DSTAR.md,
         * "enganches de UI". */
        cfg.hdr.flags[0] |= DSTAR_FLAG1_REPEATER;
        snprintf(cfg.hdr.rpt1, sizeof(cfg.hdr.rpt1), "%.8s", state.settings.callsign);
        snprintf(cfg.hdr.rpt2, sizeof(cfg.hdr.rpt2), "%.8s", state.settings.callsign);
    } else {
        snprintf(cfg.hdr.rpt1, sizeof(cfg.hdr.rpt1), "DIRECT");
        snprintf(cfg.hdr.rpt2, sizeof(cfg.hdr.rpt2), "DIRECT");
    }
    snprintf(cfg.hdr.ur, sizeof(cfg.hdr.ur), "CQCQCQ");
    cfg.repeatHeader = true;
    cfg.useNullVoice = true;

#ifdef CONFIG_DSTAR_MESSAGING
    /* A queued message becomes the slow-data text of this transmission, which is
     * what a D-STAR radio does with its TX message. */
    const struct message *msg = messaging_head(MSG_TRANSPORT_DSTAR);
    if (msg != NULL)
        cfg.text = msg->text;
#endif

    dstarTx_begin(&cfg);
    txEnded = false;
    txFrames = 0;

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0)
        return false;
    txId = audioStream_start(txPath, audioBuffer, 2 * HALF_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (txId < 0) {
        audioPath_release(txPath);
        txPath = -1;
        return false;
    }
    return true;
}

void OpMode_DSTAR::stopTx()
{
    if (txEnded == false) {
        /* One last block so the end pattern and the pulse tail leave the
         * shaper, then close the stream. */
        dstarTx_samples(audioBuffer, 2 * HALF_SAMPLES, DSTAR_TX_AMPLITUDE);
        txEnded = true;
    }
    if (txId >= 0)
        audioStream_terminate(txId);
    if (txPath >= 0)
        audioPath_release(txPath);
    txId = -1;
    txPath = -1;
}

bool OpMode_DSTAR::txSlot()
{
    /* Fill one half buffer (30 ms) of modulating waveform. */
    dstarTx_samples(audioBuffer, HALF_SAMPLES, DSTAR_TX_AMPLITUDE);
    outputStream_sync(txId, false);
    txFrames++;

    if (dstarTx_done() && txEnded == false) {
        txEnded = true;
        return true;
    }
    return false;
}

#else /* !CONFIG_DSTAR_TX: PTT does nothing in this mode */

bool OpMode_DSTAR::startTx(const rtxStatus_t *status)
{
    (void)status;
    return false;
}

void OpMode_DSTAR::stopTx()
{
    if (txId >= 0)
        audioStream_terminate(txId);
    if (txPath >= 0)
        audioPath_release(txPath);
    txId = -1;
    txPath = -1;
}

bool OpMode_DSTAR::txSlot()
{
    return true;
}

#endif /* CONFIG_DSTAR_TX */

/* --------------------------------------------------------------- opmode --- */

void OpMode_DSTAR::enable()
{
    dstarPhy_init();
    dstarStatus_reset();
    dstarSpeech_start();
    startRx = true;
    txBlocked = false;
    txEnded = false;
}

void OpMode_DSTAR::disable()
{
    startRx = false;
    stopTx();
    stopSampling();
    dstarSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    dstarStatus_clear();
}

bool OpMode_DSTAR::rxSquelchOpen()
{
    struct dstarStatus st;
    dstar_getStatus(&st);
    return st.sync;
}

void OpMode_DSTAR::abortTx()
{
    stopTx();
}

void OpMode_DSTAR::update(rtxStatus_t *const rtx, const bool newCfg)
{
    if (platform_getPttStatus() == false)
        txBlocked = false;

    if (newCfg) {
        if (basebandId >= 0) {
            stopSampling();
            startRx = true;
        }
        dstarPhy_reset();
        dstarStatus_reset();
    }

    switch (rtx->opStatus) {
    case OFF:
        stopTx();
        radio_disableRtx();
        if (startRx) {
            rtx->opStatus = RX;
            return;
        }
        sleepFor(0, 30);
        return;

    case TX:
#ifdef CONFIG_DSTAR_TX
        if (platform_getPttStatus() == false && txEnded == false) {
            /* PTT released: send the end pattern and stop */
            dstarTx_end();
        }
        if (txSlot()) {
            stopTx();
            rtx->opStatus = OFF;
            startRx = true;
        }
#else
        stopTx();
        rtx->opStatus = OFF;
        startRx = true;
#endif
        return;

    case RX: {
        if (basebandId < 0)
            startSampling();

        /* Push whatever the stream has: half a double buffer, 30 ms. */
        dataBlock_t block = inputStream_getData(basebandId);
        if (block.data == NULL) {
            sleepFor(0, 10);
            break;
        }
        dstarPhy_push(block.data, block.len, headerTrampoline, frameTrampoline,
                      eotTrampoline, this);
        break;
    }

    default:
        rtx->opStatus = OFF;
        startRx = true;
        return;
    }

    /* PTT keys the transmitter.  The receiver is stopped first: the BK4819 is
     * half duplex and both directions use the same audio path. */
    if (platform_getPttStatus() && (rtx->txDisable == 0) && (txBlocked == false)) {
        stopSampling();
        radio_disableRtx();
        if (startTx(rtx)) {
            rtx->opStatus = TX;
            platform_ledOn(GREEN);
            return;
        }
        txBlocked = true;
        startRx = true;
    }
}
