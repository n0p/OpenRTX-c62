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
#include "rtx/TxGuard.h"
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
 * number is still a bench item: docs/TX_DSTAR.md says how to calibrate it and
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
    txFirstBlock(false),
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
    struct txGuard *guard = rtx_getTxGuard();
    struct dstarTxConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    /*
     * The transmit supervisor covers this over from here on: a PTT that never
     * comes back, a stream that stops taking blocks or an over that outlives
     * the cap is cut from the main thread, and the driver's own 95 s barrier
     * has a guard to ask.  Until this existed the mode put TX on the screen,
     * built the waveform into an audio path nobody had keyed and left the
     * radio in receive.
     */
    txg_enter(guard, (uint32_t)getTick());

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
    txFirstBlock = false;

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0)
        return false;

    /*
     * One buffer for both directions, and the sink starts playing its first
     * half as soon as the stream is attached: zeroing it here keeps the first
     * 30 ms of the transmission from being the last thing received.
     */
    memset(audioBuffer, 0, sizeof(audioBuffer));

    txId = audioStream_start(txPath, audioBuffer, 2 * HALF_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (txId < 0) {
        audioPath_release(txPath);
        txPath = -1;
        return false;
    }

    /*
     * Everything the modulator needs is open: key the PA through the driver's
     * own gate (rtx_txPermitted() is what the driver checks, and the gate this
     * mode has just passed is what it asks) and treat a refusal as a failed
     * key-up, not as a transmission with no carrier.
     */
    txg_streamsOpen(guard, (uint32_t)getTick());
    radio_enableTx();
    if (radio_getStatus() != TX) {
        /* Nothing was keyed and nothing played: close without waiting for the
         * sink (stopTx(false)). */
        stopTx(false);
        return false;
    }
    txg_note(guard, TXG_EV_KEYED, (uint32_t)getTick());
    return true;
}

void OpMode_DSTAR::stopTx(bool drain)
{
    if ((txEnded == false) && drain) {
        /*
         * The end pattern and the pulse tail: two halves, each one rendered
         * into the half the sink is not playing.  This used to be a single
         * write of the whole buffer followed by audioStream_terminate(), which
         * discards whatever the codec has not played yet, so the pattern never
         * left the radio.  An abort (drain = false) skips it: the supervisor
         * has already dropped the PA and this runs in the main thread.
         */
        for (unsigned i = 0; (i < 2) && (txId >= 0); i++) {
            stream_sample_t *idle = outputStream_getIdleBuffer(txId);

            if (idle == NULL)
                break;
            dstarTx_samples(idle, HALF_SAMPLES, DSTAR_TX_AMPLITUDE);
            if (!outputStream_sync(txId, true))
                break;
        }
    }
    txEnded = true;

    if (txId >= 0) {
        /*
         * drain: let the sink finish the half it has (audioStream_stop()),
         * which is what puts the end pattern on the air.  Otherwise cancel at
         * once (audioStream_terminate()): that is the abort path, which runs in
         * the main thread and must not wait.
         */
        if (drain)
            audioStream_stop(txId);
        else
            audioStream_terminate(txId);
    }
    if (txPath >= 0)
        audioPath_release(txPath);
    txId = -1;
    txPath = -1;
    txFirstBlock = false;

    /* The over is over: close the guard here, or the supervisor keeps asking
     * for an abort that already happened. */
    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_MODE);
}

bool OpMode_DSTAR::txSlot()
{
    struct txGuard *guard = rtx_getTxGuard();
    stream_sample_t *idle = outputStream_getIdleBuffer(txId);

    /*
     * One half buffer (30 ms) of modulating waveform, into the half the sink is
     * *not* playing: with a double buffered stream, writing the same half every
     * time and syncing with "nothing changed" (what this did) overwrites what
     * is on the air and leaves the other half playing stale memory for ever.
     */
    if (idle == NULL) {
        txBlocked = true;
        return true;
    }

    dstarTx_samples(idle, HALF_SAMPLES, DSTAR_TX_AMPLITUDE);
    if (!outputStream_sync(txId, true)) {
        /* The sink is not consuming: this is the end of the over, not a block
         * to retry.  The PTT stays blocked until it is released. */
        txBlocked = true;
        return true;
    }
    txFrames++;

    /*
     * One block reached the sink: the first one starts the supervisor's
     * cadence clock, the rest keep it and flag a block that arrived late.
     */
    if (!txFirstBlock) {
        txFirstBlock = true;
        txg_firstBlock(guard, (uint32_t)getTick());
    } else {
        txg_block(guard, (uint32_t)getTick());
    }

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

void OpMode_DSTAR::stopTx(bool drain)
{
    (void)drain;
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

    /* Age the flags first: a call that stopped must close the squelch without
     * waiting for the next frame to arrive. */
    dstarStatus_age((uint32_t)getTick());
    dstar_getStatus(&st);
    return st.sync;
}

void OpMode_DSTAR::abortTx()
{
    /*
     * The supervisor unkeyed from another thread and left the status at OFF.
     * The streams are cancelled without waiting for the sink (this runs in the
     * main thread), the guard is closed so it stops asking for an abort that
     * has already happened, and startRx is what asks for the receiver on the
     * next pass.  txBlocked keeps a PTT that is still held from keying again
     * the moment the abort lands.
     */
#ifdef CONFIG_DSTAR_TX
    stopTx(false);
#endif
    txBlocked = true;
    startRx = true;
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
            /*
             * Entering the mode, or coming back from a transmission: the
             * receiver is asked for here.  Without this the chip stayed as
             * radio_disableRtx() left it (OFF, LNA off, audio path closed), so
             * the mode never received anything and switchMode() took the radio
             * back to the previous mode after its 300 ms wait
             * (rtx.cpp, modeStarted()).
             */
            radio_enableRx();
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
    if (rtx_keyingAllowed(rtx) && (txBlocked == false)) {
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
