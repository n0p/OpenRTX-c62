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
#include "protocols/DPMR/DpmrPort.h"
#include "protocols/DPMR/DpmrSpeech.h"
#include "protocols/DPMR/DpmrStatus.h"
#include "rtx/OpMode_DPMR.hpp"
#include "core/utils.h"

#ifdef CONFIG_DPMR_TX
#include "core/state.h"
#include "interfaces/delays.h"
#include "protocols/DMR/dmr_enc.h"
#include "rtx/TxGuard.h"

/*
 * Transmit level.  dPMR asks for +-1050 Hz on its outer symbols (clause 12.2,
 * h = 0.29 on a 12.5 kHz channel); the DMR chain's figure, 23000 * 0.3165 *
 * 1944 / 2400, is the same audio amplitude for +-1944 Hz, so scaling it by
 * 1050/1944 gives the level this modulator wants.  The absolute deviation is
 * the radio's shared REG_40 calibration: docs/TX_DPMR.md.
 */
#define DPMR_TX_LEVEL (23000.0f * 0.3165f * 1050.0f / 2400.0f)
/* The C62 transmitter inverts, as for M17 and DMR */
#define DPMR_TX_INVERT true
/* PCM one AMBE+2 frame eats, and two per 40 ms half */
#define VOCODER_PCM_SAMPLES 160
#define VOCODER_FRAMES_PER_HALF 2
/*
 * Preamble frames before the header: two, 160 ms.  The standard only asks for
 * 72 bits, but a receiver that has to find FS1 by correlation needs enough of
 * it to ride out the acquisitions it makes on the preamble itself: with one
 * frame of it this receiver decodes the header, with none it never does.
 */
#define DPMR_TX_PREAMBLE_FRAMES 2
/* Silence halves after the END frame: the pulse tail and the PA ramp-down */
#define DPMR_TX_TAIL_HALVES 2
#endif


/* 30 ms of baseband at 48 kHz, the same half the other flat RX modes take */
#define HALF_SAMPLES 1440

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

/*
 * The baseband block the audio stream fills, 30 ms of discriminator.  It is
 * written by the codec worker and handed over whole to the rtx thread, so on
 * the C62 it goes to PSRAM (DpmrPort.h) with the rest of the cold buffers.
 * The section is NOLOAD: startSampling() clears it before the stream exists.
 */
static EXT_RAM_BSS_ATTR int16_t baseband[2 * HALF_SAMPLES];

void OpMode_DPMR::frameTrampoline(const struct dpmrFrame *frame, void *user)
{
    static_cast<OpMode_DPMR *>(user)->onFrame(frame);
}

void OpMode_DPMR::onFrame(const struct dpmrFrame *frame)
{
    /*
     * Everything the burst gives is published before anything else reads it:
     * the UI's state block (ui_mode_state.c) and the operator's screen are
     * written from DpmrStatus.c, not from the accessors this class keeps for
     * the diagnostics (docs/RX_DPMR.md, "What the screen can show").
     */
    dpmrStatus_frame(frame);

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
#ifdef CONFIG_DPMR_TX
    : frameHead(0), frameCount(0), txStage(0), txHalf(0), txTail(0),
      txFrames(0), txRunning(false), txFirstBlock(false),
      txPath(-1), txId(-1), micPath(-1), micId(-1)
#endif
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
    dpmrStatus_reset();
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
#ifdef CONFIG_DPMR_TX
    stopTx();
#endif
    stopSampling();
    dpmrSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    dpmrStatus_clear();
}

void OpMode_DPMR::startSampling()
{
    memset(baseband, 0, sizeof(baseband));
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
    /* No samples, no frame grid: the state block must not claim "RX" */
    dpmrStatus_receiving(false);
}

bool OpMode_DPMR::rxSquelchOpen()
{
    return dpmrPhy_synced();
}


#ifdef CONFIG_DPMR_TX
/* ------------------------------------------------------------------ */
/* Transmit                                                           */
/* ------------------------------------------------------------------ */

/**
 * Configure the chain for this channel and open the streams.
 *
 * The addresses are the 24-bit ones the operator edits for DMR
 * (rtxStatus_t::dmrSourceId/dmrDestId), which is exactly dPMR's address size.
 * The channel code is the one clause 6.1.5 puts on the frequency, group A:
 * the same table the receiver reads back, so a round trip agrees.
 */
bool OpMode_DPMR::startBaseband(const rtxStatus_t *status)
{
    uint8_t codeBits[12];
    uint16_t code = 0;

    if ((status->dmrSourceId == 0) || (status->dmrDestId == 0))
        return false;

    if (dpmrDefs_channelCodeBits(status->rxFrequency, DPMR_GROUP_A,
                                 codeBits)) {
        for (int i = 0; i < 12; i++)
            code = (uint16_t)((code << 1) | codeBits[i]);
    }

    dpmrTx_setup(&tx, status->dmrSourceId, status->dmrDestId, code);
    dpmrTx_setCallType(&tx, DPMR_MODE_VOICE, 0,
                       (status->dmrDestType == DMR_DEST_USER) ? DPMR_FMT_PEER
                                                              : DPMR_FMT_ALL_CALL);
    fsk4Mod_init(&txMod, DPMR_TX_SPS, DPMR_TX_LEVEL, DPMR_TX_INVERT, false);

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0)
        return false;

    txId = audioStream_start(txPath, txSamples, 2 * DPMR_TX_HALF_SAMPLES,
                             48000, STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    return (txId >= 0);
}

bool OpMode_DPMR::startTx(const rtxStatus_t *status)
{
    struct txGuard *guard = rtx_getTxGuard();

    txg_enter(guard, (uint32_t)getTick());

    if (!dmrEnc_available())
        return false;

    if ((status->txFrequency == 0) || (status->txDisable != 0))
        return false;

    if (!startBaseband(status)) {
        txg_streamsFailed(guard, (uint32_t)getTick());
        stopTx();
        return false;
    }

    micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if (micPath < 0) {
        stopTx();
        return false;
    }

    micId = audioStream_start(micPath, micPcm, 2 * VOCODER_PCM_SAMPLES, 8000,
                              STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (micId < 0) {
        stopTx();
        return false;
    }

    dmrEnc_reset();
    frameHead = 0;
    frameCount = 0;
    txStage = 0;
    txHalf = 0;
    txTail = 0;
    txRunning = true;
    /*
     * A new transmission waits for its own first block: putHalf() reports
     * txg_firstBlock() only when this is false, and a flag left set by the
     * previous over made the guard -- already in WAIT_FIRST -- take txg_block()
     * for this one, which that phase ignores, until TXG_FIRST_MS expired and
     * the supervisor aborted the second press 300 ms in (see OpMode_NXDN).
     */
    txFirstBlock = false;

    txg_streamsOpen(guard, (uint32_t)getTick());
    radio_enableTx();
    txg_note(guard, TXG_EV_KEYED, (uint32_t)getTick());
    return true;
}

void OpMode_DPMR::stopTx()
{
    const bool wasRunning = txRunning;

    txRunning = false;

    if (txId >= 0)
        audioStream_stop(txId);
    if (micId >= 0)
        audioStream_terminate(micId);
    if (txPath >= 0)
        audioPath_release(txPath);
    if (micPath >= 0)
        audioPath_release(micPath);

    txId = micId = txPath = micPath = -1;

    if (wasRunning)
        radio_disableRtx();

    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_MODE);
}

bool OpMode_DPMR::fillMic(unsigned frames)
{
    for (unsigned i = 0; (i < frames) && (frameCount < DPMR_TX_QUEUE_FRAMES);
         i++) {
        dataBlock_t block = inputStream_getData(micId);

        if ((block.data == NULL) || (block.len < VOCODER_PCM_SAMPLES))
            break;

        uint8_t *dst = frameQueue[(frameHead + frameCount)
                                  % DPMR_TX_QUEUE_FRAMES];

        if (!dmrEnc_encode((const int16_t *)block.data, dst))
            break;
        frameCount++;
    }
    return frameCount > 0;
}

bool OpMode_DPMR::putHalf()
{
    struct txGuard *guard = rtx_getTxGuard();
    stream_sample_t *idle = outputStream_getIdleBuffer(txId);

    if (idle == NULL)
        return false;

    memcpy(idle, txSamples, DPMR_TX_HALF_SAMPLES * sizeof(stream_sample_t));
    outputStream_sync(txId, true);

    if (txFirstBlock) {
        txg_block(guard, (uint32_t)getTick());
    } else {
        txFirstBlock = true;
        txg_firstBlock(guard, (uint32_t)getTick());
    }
    return true;
}

/**
 * One 40 ms half of the transmission.
 *
 * A dPMR transmission is a preamble, a header frame, superframes of four
 * payload frames for as long as the PTT is held, and an END frame.  Frames are
 * 80 ms, so each one is two halves of this function; the END frame is 20 ms
 * and the second half of its half is silence.
 */
bool OpMode_DPMR::txSlot(const rtxStatus_t *status)
{
    const bool ptt = platform_getPttStatus();
    const int halfSymbols = DPMR_TX_HALF_SAMPLES / DPMR_TX_SPS;

    (void)status;

    fillMic(VOCODER_FRAMES_PER_HALF);

    /* PTT released: close the call with the END frame, then the tail */
    if (!ptt && (txStage == 2)) {
        txStage = 3;
        txHalf = 0;
        txg_finishing(rtx_getTxGuard(), (uint32_t)getTick());
    }

    if (txStage == 4) {
        fsk4Mod_levels(&txMod, nullptr, halfSymbols, txSamples);
        txHalf ^= 1;
        if (!putHalf())
            return true;
        return ++txTail >= DPMR_TX_TAIL_HALVES;
    }

    if (txStage == 3) {
        /*
         * The END frame is 48 symbols -- exactly one write -- and the rest of
         * its 20 ms is silence, which is also what carries its pulse out.
         */
        dpmrTx_end(&tx, -1, levelsBuf);
        fsk4Mod_levels(&txMod, levelsBuf, DPMR_TX_END_SYMBOLS, txSamples);
        txStage = 4;
        return putHalf() ? false : true;
    }

    if (txHalf == 0) {
        if (txStage == 0) {
            dpmrTx_preamble(levelsBuf);
        } else if (txStage == 1) {
            dpmrTx_header(&tx, DPMR_HT_START, 0, levelsBuf);
        } else {
            uint8_t ambe[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES];

            memset(ambe, 0, sizeof(ambe));
            for (unsigned i = 0; i < DPMR_AMBE_FRAMES; i++) {
                if (frameCount == 0)
                    break;
                memcpy(ambe[i], frameQueue[frameHead], DPMR_AMBE_BYTES);
                frameHead = (frameHead + 1) % DPMR_TX_QUEUE_FRAMES;
                frameCount--;
            }
            dpmrTx_frame(&tx, (const uint8_t (*)[DPMR_AMBE_BYTES])ambe,
                         levelsBuf);
        }
    }

    fsk4Mod_levels(&txMod, &levelsBuf[txHalf * halfSymbols], halfSymbols,
                   txSamples);

    if (txHalf == (DPMR_TX_FRAME_HALVES - 1)) {
        if (txStage == 0) {
            if (++txFrames >= DPMR_TX_PREAMBLE_FRAMES)
                txStage = 1;
        } else if (txStage == 1) {
            txStage = 2;
        }
    }

    txHalf = (txHalf + 1) & 3;

    return putHalf() ? false : true;
}

#endif /* CONFIG_DPMR_TX */

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

#ifdef CONFIG_DPMR_TX
    if (rtx->opStatus == TX) {
        bool finished;

        if (platform_getPttStatus() == false)
            txg_finishing(rtx_getTxGuard(), (uint32_t)getTick());

        finished = txSlot(rtx);

        if (finished) {
            stopTx();
            rtx->opStatus = RX;
            startRx = true;
            settle = SETTLE_SAMPLES;
        }
        return;
    }

    /* PTT: only with the mode selected, a frequency and a destination */
    if (rtx_keyingAllowed(rtx) && startRx && (rtx->opStatus == RX)) {
        stopSampling();
        radio_disableRtx();
        if (startTx(rtx)) {
            rtx->opStatus = TX;
            return;
        }
    }
#else
    /* dPMR is receive only without CONFIG_DPMR_TX: the PTT does nothing */
    if (rtx->opStatus == TX) {
        rtx->opStatus = RX;
        return;
    }
#endif

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

    /* What the main screen's "RX" marker follows: the grid, not the frames */
    dpmrStatus_receiving(dpmrPhy_synced());

    /* A transmission that ended: the grid timed out, count the call as lost */
    if (synced && !dpmrPhy_synced()) {
        framesLost++;
        platform_ledOff(GREEN);
    } else if (dpmrPhy_synced()) {
        platform_ledOn(GREEN);
    }
}
