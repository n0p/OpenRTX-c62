/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/YSF/YsfFrame.h"
#include "protocols/YSF/YsfMem.h"
#include "protocols/YSF/YsfPhy.h"
#include "protocols/YSF/YsfSpeech.h"
#include "rtx/OpMode_YSF.hpp"
#include "core/state.h"

#ifdef CONFIG_YSF_TX
#include "protocols/YSF/YsfTx.h"
/*
 * The voice is AMBE+2 2450, the canonical 72-bit frame: lib/dmr_encoder, the
 * same C encoder the DMR transmitter runs.  It is NOT dmrEnc_encode(), which
 * hands the bits out in the DMR burst order -- System Fusion carries them in
 * canonical A|B|C order, which is what this encoder produces and what the
 * receiver's vocoder takes.
 *
 * CONFIG_DMR_TX is the symbol that brings the encoder in: the platform builds
 * lib/dmr_encoder whenever the DMR transmitter is enabled (the OpenDMR
 * reference is an option of that build), and there is no separate Kconfig
 * switch for it.
 */
#ifdef CONFIG_DMR_TX
#include "dmr_encoder.h"
#define YSF_TX_HAS_ENCODER 1
#endif

/*
 * Without the AMBE+2 encoder (a build with CONFIG_YSF_TX but no DMR_TX) the
 * voice slots still go out, as zeros: a valid V/D mode 1 frame with no speech
 * in it, the same choice the P25 transmitter makes without an IMBE encoder.
 * The microphone is still read so the frame cadence is the real one.
 */
#ifdef YSF_TX_HAS_ENCODER
#define YSF_TX_PCM_SAMPLES DMR_ENCODER_PCM_SAMPLES
#define YSF_TX_FRAME_BYTES DMR_ENCODER_FRAME_BYTES
#else
#define YSF_TX_PCM_SAMPLES 160 /* 20 ms of 8 kHz PCM */
#define YSF_TX_FRAME_BYTES 9   /* one 72-bit slot */
#endif
#endif                         /* CONFIG_YSF_TX */

/*
 * 100 ms of baseband at 48 kHz: one System Fusion frame exactly.  The PHY
 * takes any block size, but the stream half is one frame on purpose: the frame
 * clock then lands on a buffer boundary and the decoder never has to catch up
 * with a burst of frames read from a ring that has already wrapped.
 *
 * The buffer is 19.2 KB (double buffered, so two 100 ms halves) and lives in
 * PSRAM: the codec worker copies into it in software, sample by sample, like
 * the DSP endpoints it already keeps there, so nothing here is a DMA target.
 */
#define BLOCK_SAMPLES 4800

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

static int16_t baseband[2 * BLOCK_SAMPLES] YSF_EXT_RAM;

#ifdef CONFIG_YSF_TX
/** 100 ms of modulating waveform: one frame, the length the stream hands out. */
#define TX_FRAME_SAMPLES (YSF_FRAME_SYMBOLS * 10)

/** Slots after the PTT release before the last frame is allowed out. */
#define TX_TAIL_SLOTS 2

/*
 * Half of the full-scale symbol level, i.e. the outer level at 900 Hz of
 * deviation: the raised cosine pulse of a four-level signal peaks at about 1.8
 * times the outer level, and a transmitter whose peak sits at full scale is
 * mis-sliced by both receivers used to check this chain (docs/TX_YSF.md).  The
 * real number is the REG_40 calibration; this is the ratio that was measured.
 */
#define YSF_TX_LEVEL (23000.0f * 0.3165f * 900.0f / 2400.0f)

/** AMBE+2 frames buffered for the transmit path (4 x 20 ms). */
#define TX_FRAME_QUEUE 4

static int16_t txSamples[TX_FRAME_SAMPLES] YSF_EXT_RAM;
static int16_t micPcm[2 * YSF_TX_PCM_SAMPLES] YSF_EXT_RAM;
static uint8_t txFrames[TX_FRAME_QUEUE][YSF_TX_FRAME_BYTES] YSF_EXT_RAM;
static unsigned txFrameHead;
static unsigned txFrameCount;
#endif

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
#ifdef CONFIG_YSF_TX
    txPath = -1;
    txId = -1;
    micPath = -1;
    micId = -1;
    txBlocked = false;
    txEnded = false;
#endif
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
#ifdef CONFIG_YSF_TX
    stopTx();
#endif
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

#ifdef CONFIG_YSF_TX

/**
 * Voice source of the transmit chain: one AMBE+2 frame from the queue.
 *
 * @return 0 with nine bytes of channel frame, or -1 when the queue ran dry (the
 *         slot then goes out as zeros, which a receiver plays as silence).
 */
int OpMode_YSF::voiceTrampoline(uint8_t frame[9], void *user)
{
    OpMode_YSF *self = static_cast<OpMode_YSF *>(user);

    while (txFrameCount == 0) {
        if (!self->fillMic(1))
            return -1;
    }

    memcpy(frame, txFrames[txFrameHead], 9);
    txFrameHead = (txFrameHead + 1) % TX_FRAME_QUEUE;
    txFrameCount--;
    return 0;
}

/** Encode up to `slots` microphone frames into the queue. */
bool OpMode_YSF::fillMic(unsigned slots)
{
    for (unsigned i = 0; i < slots; i++) {
        if (txFrameCount >= TX_FRAME_QUEUE)
            break;

        dataBlock_t block = inputStream_getData(micId);
        if ((block.data == NULL) || (block.len < YSF_TX_PCM_SAMPLES))
            break;

        uint8_t *dst = txFrames[(txFrameHead + txFrameCount) % TX_FRAME_QUEUE];

#if defined(YSF_TX_HAS_ENCODER)
        if (dmr_encoder_encode((const int16_t *)block.data, dst) != 0)
            break;
#else
        memset(dst, 0, 9);
#endif
        txFrameCount++;
    }
    return txFrameCount > 0;
}

/** Start a System Fusion transmission. */
bool OpMode_YSF::startTx(const rtxStatus_t *status)
{
    struct ysfTxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.source, sizeof(cfg.source), "%.10s", state.settings.callsign);
    snprintf(cfg.dest, sizeof(cfg.dest), "CQCQCQ");
    snprintf(cfg.uplink, sizeof(cfg.uplink), "DIRECT");
    snprintf(cfg.downlink, sizeof(cfg.downlink), "DIRECT");
    cfg.dgid = 0; /* DG-ID 0: every station, the default of the mode */
    cfg.text = NULL;

    ysfTx_begin(&cfg);
    ysfTx_setVoiceSource(voiceTrampoline, this);
    txEnded = false;
    txFrameHead = 0;
    txFrameCount = 0;

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0)
        return false;

    txId = audioStream_start(txPath, txSamples, 2 * TX_FRAME_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (txId < 0) {
        audioPath_release(txPath);
        txPath = -1;
        return false;
    }

    micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if (micPath < 0) {
        stopTx();
        return false;
    }
    micId = audioStream_start(micPath, micPcm, 2 * YSF_TX_PCM_SAMPLES, 8000,
                              STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (micId < 0) {
        stopTx();
        return false;
    }

#ifdef YSF_TX_HAS_ENCODER
    dmr_encoder_reset();
#endif
    return true;
}

void OpMode_YSF::stopTx()
{
    ysfTx_setVoiceSource(NULL, NULL);
    ysfTx_end();

    if (txId >= 0)
        audioStream_terminate(txId);
    if (micId >= 0)
        audioStream_terminate(micId);
    if (txPath >= 0)
        audioPath_release(txPath);
    if (micPath >= 0)
        audioPath_release(micPath);
    txId = micId = -1;
    txPath = micPath = -1;
    txFrameCount = 0;
}

/** One 100 ms frame of modulating waveform; true when the call is over. */
bool OpMode_YSF::txSlot()
{
    fillMic(YSF_VOICE_SLOTS);
    ysfTx_samples(txSamples, TX_FRAME_SAMPLES, YSF_TX_LEVEL);
    outputStream_sync(txId, false);

    if (ysfTx_done() && !txEnded) {
        txEnded = true;
        return true;
    }
    return false;
}

#endif /* CONFIG_YSF_TX */

void OpMode_YSF::abortTx()
{
#ifdef CONFIG_YSF_TX
    stopTx();
#endif
}

void OpMode_YSF::update(rtxStatus_t *const rtx, const bool newCfg)
{
#ifdef CONFIG_YSF_TX
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

#ifdef CONFIG_YSF_TX
        case TX:
            if ((platform_getPttStatus() == false) && (txEnded == false))
                ysfTx_end();

            if (txSlot()) {
                stopTx();
                rtx->opStatus = OFF;
                startRx = true;
            }
            return;
#endif

        default:
#ifdef CONFIG_YSF_TX
            stopTx();
#endif
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

#ifdef CONFIG_YSF_TX
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
#endif
}
