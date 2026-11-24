/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DMR/DmrFec.h"
#include "protocols/DMR/DmrPhy.h"
#include "protocols/DMR/DmrSpeech.h"
#include "protocols/DMR/DmrStatus.h"
#include "rtx/OpMode_DMR.hpp"

#ifdef CONFIG_DMR_TX
#include "protocols/DMR/DmrTx.h"
#include "protocols/DMR/DmrVocoder.h"
#endif

/* 30 ms of baseband at 48 kHz: one burst arrives every 60 ms per timeslot */
#define HALF_SAMPLES 1440

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

#ifdef CONFIG_DMR_TX
/* 8 kHz PCM samples the vocoder eats per frame */
#define VOCODER_PCM_SAMPLES 160

/* Frames the queue is allowed to take per slot: 1,5 is the average rate, two
 * keeps up and still leaves time for the modulator inside 30 ms */
#define VOCODER_FRAMES_PER_SLOT 2
#endif

static int16_t baseband[2 * HALF_SAMPLES];
#ifdef CONFIG_DMR_TX
static int16_t micPcm[VOCODER_PCM_SAMPLES];
#endif

/**
 * Hand a decoded burst to the status block and, for voice, to the playback
 * thread.  Runs in the rtx thread: it must not decode anything here.
 */
static void onBurst(const struct dmrBurst *burst, void *user)
{
    (void)user;

    dmrStatus_burst(burst);
    if (burst->voice && (burst->frameCount > 0))
        dmrSpeech_push(burst->frame, burst->frameCount);
}

OpMode_DMR::OpMode_DMR()
    : startRx(false)
    , dataErrors(0)
    , settle(SETTLE_SAMPLES)
    , basebandPath(-1)
    , basebandId(-1)
#ifdef CONFIG_DMR_TX
    , txRunning(false)
    , micPath(-1)
    , micId(-1)
    , txPath(-1)
    , txId(-1)
    , frameHead(0)
    , frameCount(0)
    , burstSlot(true)
    , txStage(TX_LEAD)
#endif
{
#ifdef CONFIG_DMR_TX
    memset(&tx, 0, sizeof(tx));
    memset(frameQueue, 0, sizeof(frameQueue));
#endif
}

OpMode_DMR::~OpMode_DMR()
{
    disable();
}

void OpMode_DMR::reset()
{
    settle = SETTLE_SAMPLES;
    dataErrors = 0;
    dmrPhy_reset();
    dmrStatus_reset();
}

void OpMode_DMR::enable()
{
    dmrPhy_init();
    reset();
    dmrSpeech_start();
    startRx = true;
}

void OpMode_DMR::disable()
{
    startRx = false;
#ifdef CONFIG_DMR_TX
    stopTx();
#endif
    stopSampling();
    dmrSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    dmrStatus_clear();
}

void OpMode_DMR::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   48000, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_DMR::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

bool OpMode_DMR::rxSquelchOpen()
{
    return dmrPhy_synced();
}

#ifdef CONFIG_DMR_TX
/**
 * Take up to three frames out of the queue, zero-filling the rest: a burst
 * always carries three, and the vocoder is slower than the slot clock when it
 * is the first burst of a call.
 */
unsigned OpMode_DMR::dequeueFrames(uint8_t frames[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES])
{
    unsigned taken = 0;

    memset(frames, 0, DMR_FRAMES_PER_BURST * DMR_AMBE_BYTES);

    while ((taken < DMR_FRAMES_PER_BURST) && (frameCount > 0)) {
        memcpy(frames[taken], frameQueue[frameHead], DMR_AMBE_BYTES);
        frameHead = (frameHead + 1) % DMR_TX_QUEUE_FRAMES;
        frameCount--;
        taken++;
    }

    return taken;
}

bool OpMode_DMR::startTx(const rtxStatus_t *status)
{
    if (!dmrVocoder_available())
        return false;

    struct dmrTxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.colourCode = status->dmrColourCode;
    cfg.sourceId = status->dmrSourceId;
    cfg.destId = status->dmrTalkgroup;
    cfg.group = true;
    cfg.timeslot = (status->dmrTimeslot == 2) ? 1 : 0;
    cfg.layout = DMR_TX_LAYOUT_MS;
    cfg.ramp = true;
    cfg.innerLevel = DMR_TX_INNER_LEVEL;
    dmrTx_init(&tx, &cfg);

    micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if (micPath < 0)
        return false;

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0) {
        audioPath_release(micPath);
        micPath = -1;
        return false;
    }

    micId = audioStream_start(micPath, micPcm, VOCODER_PCM_SAMPLES, 8000,
                              STREAM_INPUT | BUF_CIRC_DOUBLE);
    txId = audioStream_start(txPath, txSamples, DMR_TX_SLOT_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if ((micId < 0) || (txId < 0)) {
        stopTx();
        return false;
    }

    frameHead = 0;
    frameCount = 0;
    burstSlot = true;
    txStage = TX_LEAD;
    txRunning = true;

    /* Last: the PA, the ramps of the first slot need it settled */
    radio_enableTx();
    return true;
}

void OpMode_DMR::stopTx()
{
    if (!txRunning)
        return;

    txRunning = false;

    if (txId >= 0)
        audioStream_stop(txId);
    if (micId >= 0)
        audioStream_terminate(micId);
    if (txPath >= 0)
        audioPath_release(txPath);
    if (micPath >= 0)
        audioPath_release(micPath);

    txId = micId = -1;
    txPath = micPath = -1;
}

/**
 * One 30 ms timeslot of transmit: encode whatever the microphone gave us and
 * render the slot into the baseband stream.  The blocking write to the output
 * stream is what keeps the slot cadence: 1440 samples at 48 kHz are exactly
 * 30 ms, and the stream has two buffers.
 */
bool OpMode_DMR::txSlot(const rtxStatus_t *status)
{
    uint8_t frames[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES];
    enum dmrTxBurst kind = DMR_TXB_VOICE;
    bool finished = false;

    (void)status;

    /* Keep the queue topped up: the microphone delivers a block every 20 ms and
     * the slot clock runs at 30 ms, so a slot has to take one frame or two.
     * Two at most, so the modulator still has time inside the slot; the queue
     * absorbs the jitter and a starved burst carries zero frames instead of
     * slipping the slot.
     */
    for (unsigned i = 0; (i < VOCODER_FRAMES_PER_SLOT)
                         && (frameCount < DMR_TX_QUEUE_FRAMES); i++) {
        dataBlock_t block = inputStream_getData(micId);

        if ((block.data == NULL) || (block.len < VOCODER_PCM_SAMPLES))
            break;

        uint8_t *dst = frameQueue[(frameHead + frameCount) % DMR_TX_QUEUE_FRAMES];

        if (dmrVocoder_encode((const int16_t *)block.data, dst) != 0)
            break;
        frameCount++;
    }

    switch (txStage) {
        case TX_LEAD:
            kind = DMR_TXB_EMPTY;
            /*
             * The PA is up but the call does not start until there is voice to
             * send: the LC header announces a call and the first voice burst
             * follows it one slot later, so keying the header with an empty
             * queue would put a header and then silence on the air.  The
             * encoder has already produced the samples by the time the
             * microphone block is read, so this costs one slot at most.
             */
            if (platform_getPttStatus() && (frameCount > 0))
                txStage = TX_LC_HEADER;
            break;

        case TX_LC_HEADER:
            kind = DMR_TXB_VOICE_LC;
            txStage = TX_VOICE;
            break;

        case TX_VOICE:
            /*
             * One burst every other slot.  A DMR call occupies one of the two
             * timeslots of the carrier and a voice burst carries 60 ms of
             * speech, which is exactly what the vocoder produces in those
             * 60 ms; transmitting a burst in every slot would send the speech
             * at twice its speed.  The slot in between is silence, not a
             * shorter burst: a handheld that keys the other slot tramples the
             * station that owns it.
             */
            if (burstSlot) {
                kind = DMR_TXB_VOICE;
                burstSlot = false;
            } else {
                kind = DMR_TXB_EMPTY;
                burstSlot = true;
            }
            if (!platform_getPttStatus())
                txStage = TX_TERMINATOR;
            break;

        case TX_TERMINATOR:
            kind = DMR_TXB_TERMINATOR;
            txStage = TX_TAIL;
            break;

        case TX_TAIL:
            /*
             * One empty slot after the terminator: it carries the tail of the
             * last pulse and lets the PA ramp down inside the guard.
             */
            kind = DMR_TXB_EMPTY;
            finished = true;
            break;
    }

    memset(frames, 0, sizeof(frames));
    if (kind == DMR_TXB_VOICE)
        dequeueFrames(frames);
    dmrTx_slot(&tx, kind, (const uint8_t (*)[DMR_AMBE_BYTES])frames, txSamples);

    stream_sample_t *idle = outputStream_getIdleBuffer(txId);

    if (idle != NULL) {
        memcpy(idle, txSamples, sizeof(txSamples));
        outputStream_sync(txId, true);
    }

    return finished;
}
#endif /* CONFIG_DMR_TX */

void OpMode_DMR::update(rtxStatus_t *const rtx, const bool newCfg)
{
    switch (rtx->opStatus) {
        case OFF:
#ifdef CONFIG_DMR_TX
            if (txRunning)
                stopTx();
#endif
            radio_disableRtx();
            if (startRx) {
                rtx->opStatus = RX;
                return;
            }
            sleepFor(0, 30);
            return;

#ifdef CONFIG_DMR_TX
        case TX:
            /*
             * The stage machine sends the terminator and one tail slot after
             * the PTT is released; when those are done the radio goes off.
             */
            if (txSlot(rtx)) {
                stopTx();
                rtx->opStatus = OFF;
                startRx = true;
            }
            return;
#endif

        case RX:
            break;

        default:
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

#ifdef CONFIG_DMR_TX
    /*
     * PTT keys the transmitter, unless the operator disabled TX or there is no
     * AMBE+2 encoder linked in.  The receiver has to be stopped first: the
     * BK4819 is half duplex and both use the same audio path.
     */
    if (platform_getPttStatus() && (rtx->txDisable == 0)) {
        /*
         * Key the transmitter even if a station is being received: with PTT
         * pressed the operator wants to talk, and the receiver is stopped
         * first because the BK4819 is half duplex.
         */
        stopSampling();
        radio_disableRtx();
        if (startTx(rtx)) {
            rtx->opStatus = TX;
            return;
        }
        startRx = true;
    }
#endif

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
     * Wait for the next half buffer of samples.  A failed read means samples
     * were lost: the symbol stream jumped, so the demodulator has to start
     * over, and there is no point in sleeping on top of the delay.
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0) {
            dmrPhy_reset();
            dmrStatus_reset();
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

    dmrPhy_push(block.data, block.len, onBurst, NULL);

    if (dmrPhy_synced())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
