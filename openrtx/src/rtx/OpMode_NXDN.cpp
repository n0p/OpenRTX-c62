/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/utils.h"
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/NXDN/NxdnPhy.h"
#include "protocols/NXDN/NxdnSpeech.h"
#include "protocols/NXDN/NxdnStatus.h"
#include "rtx/OpMode_NXDN.hpp"

#ifdef CONFIG_NXDN_TX
#include "protocols/DMR/dmr_enc.h"
#include "protocols/Fsk4Tx.h"
#include "protocols/NXDN/NxdnTx.h"
#include "rtx/TxGuard.h"
#endif
#include "rtx/TxTrace.h"

/* 20 ms of baseband at 48 kHz.  The demodulator is streaming and keeps its own
 * history, so the block only has to be small enough that the rtx thread turns
 * around often: 20 ms is the AMBE frame period and a quarter of a NXDN frame. */
#define HALF_SAMPLES 960

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

static int16_t baseband[2 * HALF_SAMPLES];

#ifdef CONFIG_NXDN_TX
/*
 * Transmit level: the modulator wants the deviation of a run of +1 symbols.
 * The DMR chain's figure is 23000 * 0.3165 * 1944 / 2400 for +-1944 Hz of
 * outer deviation; NXDN48 puts its outer symbols at +-1050 Hz on a 12.5 kHz
 * channel and at +-350 Hz on a 6.25 kHz one (docs/RX_NXDN.md), so the same
 * arithmetic gives these two.  The absolute deviation is the radio's REG_40
 * calibration, shared with DMR: see docs/TX_NXDN.md.
 */
#define NXDN_TX_LEVEL_12K5 (23000.0f * 0.3165f * 1050.0f / 2400.0f)
#define NXDN_TX_LEVEL_6K25 (23000.0f * 0.3165f * 350.0f / 2400.0f)
/* The C62 transmitter inverts, as for M17 and DMR */
#define NXDN_TX_INVERT true
/* PCM one AMBE+2 frame eats, and frames per 40 ms half (two) */
#define VOCODER_PCM_SAMPLES 160
#define VOCODER_FRAMES_PER_HALF 2
/* Preamble halves before the first frame: 40 ms of 4.4.3's pattern */
#define NXDN_TX_PREAMBLE_HALVES 1
/* Silence halves after the release before the PA goes off */
#define NXDN_TX_TAIL_HALVES 2
#endif

void OpMode_NXDN::frameTrampoline(const struct nxdnFrame *frame, void *user)
{
    static_cast<OpMode_NXDN *>(user)->onFrame(frame);
}

void OpMode_NXDN::onFrame(const struct nxdnFrame *frame)
{
    nxdnStatus_frame(frame);

    if (!frame->lichOk)
        return;

    /*
     * The RAN travels in the SR bits of the SACCH, not in the LICH, so the
     * filter can only be applied once a superframe has decoded: for the first
     * ~320 ms after the carrier appears everything passes, and afterwards the
     * frames of any other RAN are dropped before they reach the vocoder or the
     * layer 3 assembler. 0xFF means "any", which is the default and what the
     * mode did before the row existed.
     */
    if (!nxdnRanFrame(state.settings.nxdn_ran, &lastRan, frame))
        return;

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
#ifdef CONFIG_NXDN_TX
    , frameHead(0), frameCount(0), txStage(0), txHalf(0), txPreamble(0),
      txReleaseFrames(0), txTail(0), txRunning(false), txFirstBlock(false),
      txBlocked(false), txPath(-1), txId(-1), micPath(-1), micId(-1)
#endif
{
}

void OpMode_NXDN::enable()
{
    nxdnPhy_init();
    nxdnStatus_reset();
    nxdnSpeech_start();
    settle = SETTLE_SAMPLES;
    dataErrors = 0;
    lastRan = -1;
    startRx = true;
}

void OpMode_NXDN::disable()
{
    startRx = false;
#ifdef CONFIG_NXDN_TX
    stopTx();
#endif
    stopSampling();
    nxdnSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    nxdnStatus_clear();
}


#ifdef CONFIG_NXDN_TX
/* ------------------------------------------------------------------ */
/* Transmit                                                           */
/* ------------------------------------------------------------------ */

/**
 * Configure the chain for this channel and open the baseband and mic streams.
 *
 * The unit and talkgroup IDs are the ones the operator already edits for DMR
 * (rtxStatus_t::dmrSourceId/dmrDestId, 24 bits); NXDN carries 16 on the air,
 * so the low 16 bits are what goes out, which is what a NXDN radio shows.
 * The RAN is the one the receiver filters on (settings nxdn_ran, "any" = 63),
 * and the transmit level follows the channel bandwidth.
 */
bool OpMode_NXDN::startBaseband(const rtxStatus_t *status)
{
    const uint16_t src = (uint16_t)(status->dmrSourceId & 0xFFFFu);
    const uint16_t dst = (uint16_t)(status->dmrDestId & 0xFFFFu);
    const uint8_t ran = nxdnRanTransmit(state.settings.nxdn_ran);
    const float level = (status->bandwidth == BW_6_25) ? NXDN_TX_LEVEL_6K25
                                                       : NXDN_TX_LEVEL_12K5;

    if ((src == 0) || (dst == 0)) {
        txTrace_block("startBaseband(): source or destination ID is 0");
        return false;
    }

    nxdnTx_setup(&tx, src, dst, ran, status->dmrDestType != DMR_DEST_USER);
    fsk4Mod_init(&txMod, NXDN_TX_SPS, level, NXDN_TX_INVERT, true);

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    txTrace_step("audioPath_request(SOURCE_MCU, SINK_RTX)", txPath);
    if (txPath < 0)
        return false;

    txId = audioStream_start(txPath, txSamples, 2 * NXDN_TX_HALF_SAMPLES,
                             48000, STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    txTrace_step("audioStream_start(OUT, 48 kHz, baseband)", txId);
    return (txId >= 0);
}

bool OpMode_NXDN::startTx(const rtxStatus_t *status)
{
    struct txGuard *guard = rtx_getTxGuard();

    txg_enter(guard, (uint32_t)getTick());

    txTrace_reset("NXDN");

    if (!dmrEnc_available()) {
        txTrace_block("no AMBE+2 encoder in this image");
        return false;
    }

    /* A frequency has to be there: the platform tunes from the rtx status */
    if ((status->txFrequency == 0) || (status->txDisable != 0)) {
        txTrace_block("txFrequency == 0 or txDisable");
        return false;
    }

    if (!startBaseband(status)) {
        txTrace_block("startBaseband()");
        txg_streamsFailed(guard, (uint32_t)getTick());
        stopTx();
        return false;
    }

    micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    txTrace_step("audioPath_request(SOURCE_MIC, SINK_MCU)", micPath);
    if (micPath < 0) {
        stopTx();
        return false;
    }

    micId = audioStream_start(micPath, micPcm, 2 * VOCODER_PCM_SAMPLES, 8000,
                              STREAM_INPUT | BUF_CIRC_DOUBLE);
    txTrace_step("audioStream_start(IN, 8 kHz, mic)", micId);
    if (micId < 0) {
        stopTx();
        return false;
    }

    dmrEnc_reset();
    frameHead = 0;
    frameCount = 0;
    txStage = 0;
    txHalf = 0;
    txPreamble = NXDN_TX_PREAMBLE_HALVES;
    txTail = 0;
    txRunning = true;
    /*
     * A new transmission waits for its own first block: putHalf() reports
     * txg_firstBlock() only when this is false, and a flag left set by the
     * previous over made the guard -- already in WAIT_FIRST -- take txg_block()
     * for this one, which that phase ignores, until TXG_FIRST_MS expired and
     * the supervisor aborted the second press 300 ms in.
     */
    txFirstBlock = false;

    txg_streamsOpen(guard, (uint32_t)getTick());
    radio_enableTx();
    txg_note(guard, TXG_EV_KEYED, (uint32_t)getTick());
    txTrace_keyed();
    return true;
}

void OpMode_NXDN::stopTx(bool drain)
{
    const bool wasRunning = txRunning;

    txRunning = false;
    txFirstBlock = false;

    if (txId >= 0) {
        /*
         * drain: let the sink finish the half it has (audioStream_stop()),
         * which is what puts the tail on the air.  Otherwise cancel at once
         * (audioStream_terminate()): that is the abort path, which runs in the
         * main thread and must not wait.
         */
        if (drain)
            audioStream_stop(txId);
        else
            audioStream_terminate(txId);
    }
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

/** Encode what the microphone has, at most `frames` AMBE+2 frames. */
bool OpMode_NXDN::fillMic(unsigned frames)
{
    for (unsigned i = 0; (i < frames) && (frameCount < NXDN_TX_QUEUE_FRAMES);
         i++) {
        dataBlock_t block = inputStream_getData(micId);

        if ((block.data == NULL) || (block.len < VOCODER_PCM_SAMPLES))
            break;

        uint8_t *dst = frameQueue[(frameHead + frameCount)
                                  % NXDN_TX_QUEUE_FRAMES];

        if (!dmrEnc_encode((const int16_t *)block.data, dst))
            break;
        frameCount++;
    }
    return frameCount > 0;
}

bool OpMode_NXDN::putHalf()
{
    struct txGuard *guard = rtx_getTxGuard();
    stream_sample_t *idle = outputStream_getIdleBuffer(txId);

    if (idle == NULL)
        return false;

    memcpy(idle, txSamples, NXDN_TX_HALF_SAMPLES * sizeof(stream_sample_t));
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
 * One 40 ms half: the first or the second half of the frame being sent.
 *
 * The stage machine is the call's: a preamble half, then voice frames -- one
 * frame per two halves, four AMBE+2 frames each -- and, when the operator lets
 * the PTT go, a superframe of TX_REL messages and two halves of silence so the
 * pulse tail and the PA ramp finish on the air.
 */
bool OpMode_NXDN::txSlot(const rtxStatus_t *status)
{
    const bool ptt = platform_getPttStatus();
    const int halfSymbols = NXDN_TX_HALF_SYMBOLS;

    (void)status;

    fillMic(VOCODER_FRAMES_PER_HALF);

    if (!ptt && (txStage == 1)) {
        nxdnTx_release(&tx);
        txStage = 2;
        txHalf = 0;
        txReleaseFrames = 0;
        /*
         * No txg_finishing() here: the release is a superframe of TX_REL
         * messages (four frames, 320 ms) that is still the call, and the drain
         * budget (TXG_DRAIN_MS, 160 ms) would cut the PA in the middle of it on
         * every transmission.  The tail starts when the superframe is out, and
         * that is where the guard is told the transmission is finishing; the
         * absolute cap (TXG_MAX_TX_MS) and the 90 s clock cover the rest.
         */
    }

    if (txStage == 3) {
        /* Silence: the tail of the pulse and the PA ramp-down */
        fsk4Mod_levels(&txMod, nullptr, halfSymbols, txSamples);
        txHalf ^= 1;
        if (!putHalf())
            return true;
        return ++txTail >= NXDN_TX_TAIL_HALVES;
    }

    /* Preamble, voice and release: one frame per four writes */
    if (txHalf == 0) {
        if (txStage == 0) {
            nxdnTx_preamble(levelsBuf);
        } else {
            uint8_t ambe[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES];

            memset(ambe, 0, sizeof(ambe));
            if (txStage == 1) {
                for (unsigned i = 0; i < NXDN_VCH_PER_FRAME; i++) {
                    if (frameCount == 0)
                        break;
                    memcpy(ambe[i], frameQueue[frameHead], NXDN_AMBE_BYTES);
                    frameHead = (frameHead + 1) % NXDN_TX_QUEUE_FRAMES;
                    frameCount--;
                }
            }
            nxdnTx_frame(&tx, (const uint8_t (*)[NXDN_AMBE_BYTES])ambe,
                         levelsBuf);
        }
    }

    fsk4Mod_levels(&txMod, &levelsBuf[txHalf * halfSymbols], halfSymbols,
                   txSamples);

    if (txHalf == (NXDN_TX_FRAME_HALVES - 1)) {
        if (txStage == 0) {
            if (--txPreamble <= 0)
                txStage = 1;
        } else if ((txStage == 2) && (++txReleaseFrames >= NXDN_SUPERFRAME)) {
            txStage = 3;
            /* The release is on the air: from here what is left is the tail,
             * and it has a budget (TXG_DRAIN_MS) it meets. */
            txg_finishing(rtx_getTxGuard(), (uint32_t)getTick());
        }
    }

    txHalf = (txHalf + 1) & 3;

    return putHalf() ? false : true;
}

#endif /* CONFIG_NXDN_TX */

void OpMode_NXDN::abortTx()
{
    /*
     * The supervisor unkeyed from another thread and left the status at OFF.
     * The streams are cancelled without waiting for the sink (this runs in the
     * main thread) and the guard is closed, or it keeps asking for an abort
     * that has already happened; startRx is what asks for the receiver on the
     * next pass, because the OFF branch of update() only re-enters receive with
     * it set and this mode used to sit there -- mute, with the chip receiving,
     * and the abort repeated every 20 ms -- until the operator changed mode.
     * txBlocked keeps a PTT that is still held from keying again.
     */
#ifdef CONFIG_NXDN_TX
    stopTx(false);
    txBlocked = true;
#endif
    startRx = true;
}

void OpMode_NXDN::update(rtxStatus_t *const rtx, const bool newCfg)
{
#ifdef CONFIG_NXDN_TX
    /* A new press retries the key-up even if the last one failed */
    if (platform_getPttStatus() == false)
        txBlocked = false;
#endif

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

#ifdef CONFIG_NXDN_TX
    case TX: {
        bool finished;

        if (platform_getPttStatus() == false)
            txg_finishing(rtx_getTxGuard(), (uint32_t)getTick());

        finished = txSlot(rtx);

        if (finished) {
            stopTx();
            rtx->opStatus = OFF;
            startRx = true;
        }
        return;
    }
#endif

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
        settle = SETTLE_SAMPLES;
    }

#ifdef CONFIG_NXDN_TX
    /*
     * PTT: only with the transmit chain built, the mode selected, a valid
     * destination and a frequency the platform can tune to.  The RAN is the
     * one the receiver filters on, so the two ends of a round trip agree.
     */
    if (rtx_keyingAllowed(rtx) && (txBlocked == false)) {
        stopSampling();
        radio_disableRtx();
        if (startTx(rtx)) {
            rtx->opStatus = TX;
            return;
        }
        txBlocked = true;
        startRx = true;
    }
#endif

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
        settle -= (block.len < (size_t)settle) ? (int16_t)block.len : settle;
        return;
    }

    nxdnPhy_push(block.data, block.len, frameTrampoline, this);

    if (nxdnPhy_synced())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
