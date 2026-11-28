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
#include "rtx/TxTrace.h"

#ifdef CONFIG_P25_TX
#include "protocols/P25/P25Tx.h"
#include "rtx/TxGuard.h"
#endif
#include "rtx/OpMode_P25.hpp"

/** 20 ms of discriminator at 48 kHz */
#define BLOCK_SAMPLES 960

#ifdef CONFIG_P25_TX
/** Samples of modulating waveform per transmit slot: 30 ms at 48 kHz, which
 *  divides a 180 ms frame exactly (six slots per frame). */
#define TX_SLOT_SAMPLES 1440

/*
 * Deviation scale handed to the FM exciter: the outer C4FM level at 1800 Hz.
 * The number comes from the DMR transmitter's scale (1944 Hz of deviation for
 * the same symbol level) and is still a bench item, because what it really
 * means is the REG_40 calibration of the transceiver.  docs/TX_P25.md says how
 * to calibrate it.
 */
#define P25_TX_LEVEL (23000.0f * 0.3165f * 1800.0f / 2400.0f)

/** Network access code of a conventional channel, until the UI carries one. */
#define P25_DEFAULT_NAC 0x293u
#endif
/** Samples dropped after starting or retuning: 0.5 s */
#define SETTLE_SAMPLES 24000

/*
 * There is only one P25 mode, so the buffers are static.  The discriminator
 * block goes to PSRAM: it is filled by the codec worker and read once per
 * block, not per sample, and the 256 KiB of SRAM cannot hold one buffer per
 * mode (DMR RX + APRS + NXDN already sit above 80 %).
 */
#ifdef __ZEPHYR__
static int16_t baseband[2 * BLOCK_SAMPLES]
    __attribute__((section(".psram_section")));
#else
static int16_t baseband[2 * BLOCK_SAMPLES];
#endif

#ifdef CONFIG_P25_TX
/**
 * The stream's own buffer: two halves of one 30 ms slot.
 *
 * audioStream_start() is given the length of the whole buffer and a
 * double-buffered stream plays one half while the mode renders into the other
 * (outputStream_getIdleBuffer()).  This used to be declared as a single slot
 * while the stream was told it held two, so the sink read 2.9 kB of PSRAM past
 * the end of the array on every slot.
 */
#ifdef __ZEPHYR__
static stream_sample_t txBaseband[2 * TX_SLOT_SAMPLES]
    __attribute__((section(".psram_section")));
#else
static stream_sample_t txBaseband[2 * TX_SLOT_SAMPLES];
#endif
#endif

/**
 * A frame arrived: show it, and hand the voice to the playback thread.
 */
static void onFrame(const struct p25Frame *frame, void *user)
{
    (void)user;

    p25Status_frame(frame);

    if ((frame->duid == P25_DUID_LDU1 || frame->duid == P25_DUID_LDU2)
        && frame->voiceCount > 0)
        p25Speech_push(frame->voice, frame->voiceValid);
}

OpMode_P25::OpMode_P25()
    : startRx(false)
    , dataErrors(0)
    , settle(SETTLE_SAMPLES)
    , basebandPath(-1)
    , basebandId(-1)
#ifdef CONFIG_P25_TX
    , txPath(-1)
    , txId(-1)
    , txBlocked(false)
    , txEnded(false)
    , txFirstBlock(false)
#endif
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
#ifdef CONFIG_P25_TX
    stopTx();
#endif
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

#ifdef CONFIG_P25_TX

/**
 * Start a transmission.
 *
 * Without an IMBE encoder there is no voice to send: p25Tx_begin() then keys
 * the carrier with a header, null-information TSBK frames and a terminator, so
 * the call is a valid P25 transmission that announces itself and carries no
 * speech, rather than a fake vocoder.
 */
bool OpMode_P25::startTx(const rtxStatus_t *status)
{
    struct txGuard *guard = rtx_getTxGuard();
    struct p25TxConfig cfg;

    /*
     * The transmit supervisor covers this over from here on: a PTT that never
     * comes back, a stream that stops taking slots or an over that outlives the
     * cap is cut from the main thread, and the driver's own 95 s barrier has a
     * guard to ask.  Until this existed the mode put TX on the screen, built
     * the waveform into an audio path nobody had keyed and left the radio in
     * receive.
     */
    txg_enter(guard, (uint32_t)getTick());

    txTrace_reset("P25");

    /*
     * The NAC, the talkgroup and the subscriber address have no field in the
     * channel or the rtx status of this firmware yet (that is a settings/UI
     * job): the NAC is the conventional default, and the group and the source
     * reuse what the DMR side already holds, which is the same 24-bit space.
     * docs/TX_P25.md lists what the UI has to grow.
     */
    memset(&cfg, 0, sizeof(cfg));
    cfg.nac = P25_DEFAULT_NAC;
    cfg.group = (status->dmrDestType == DMR_DEST_GROUP) ?
                    (uint16_t)(status->dmrDestId & 0xFFFFu) :
                    1u;
    cfg.source = status->dmrSourceId;
    cfg.service = 0x00;

    p25Tx_begin(&cfg);
    txEnded = false;
    txFirstBlock = false;

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    txTrace_step("audioPath_request(SOURCE_MCU, SINK_RTX)", txPath);
    if (txPath < 0)
        return false;

    /*
     * The buffer is NOLOAD PSRAM and the sink starts playing its first half as
     * soon as the stream is attached, so it is zeroed here: the alternative is
     * that the first 30 ms of a transmission is whatever was left there.
     */
    memset(txBaseband, 0, sizeof(txBaseband));

    txId = audioStream_start(txPath, txBaseband, 2 * TX_SLOT_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    txTrace_step("audioStream_start(OUT, 48 kHz, baseband)", txId);
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
        txTrace_block("radio_enableTx(): the PA did not key");
        /* Nothing was keyed and nothing played: close without waiting for the
         * sink (stopTx(false)). */
        stopTx(false);
        return false;
    }
    txg_note(guard, TXG_EV_KEYED, (uint32_t)getTick());
    txTrace_keyed();
    return true;
}

void OpMode_P25::stopTx(bool drain)
{
    p25Tx_end();
    if (txId >= 0) {
        /*
         * drain: let the sink finish the slot it has (audioStream_stop()),
         * which is what puts the terminator on the air.  Otherwise cancel at
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

/**
 * One 30 ms slot of modulating waveform.
 *
 * @return true when the terminator has left the shaper and the carrier may
 *         drop.
 */
bool OpMode_P25::txSlot()
{
    struct txGuard *guard = rtx_getTxGuard();
    stream_sample_t *idle = outputStream_getIdleBuffer(txId);

    /*
     * The half the sink is *not* playing is the one to write: with a double
     * buffered stream, writing the same half every time and syncing with
     * "nothing changed" (what this did) overwrites what is on the air and
     * leaves the other half playing stale memory for ever.
     */
    if (idle == NULL) {
        txBlocked = true;
        return true;
    }

    p25Tx_samples(idle, TX_SLOT_SAMPLES, P25_TX_LEVEL);
    if (!outputStream_sync(txId, true)) {
        /* The sink is not consuming: this is the end of the over, not a slot
         * to retry.  The PTT stays blocked until it is released. */
        txBlocked = true;
        return true;
    }

    /*
     * One block reached the sink: the first one starts the supervisor's
     * cadence clock, the rest keep it and flag a slot that arrived late.
     */
    if (!txFirstBlock) {
        txFirstBlock = true;
        txg_firstBlock(guard, (uint32_t)getTick());
    } else {
        txg_block(guard, (uint32_t)getTick());
    }

    if (p25Tx_done() && !txEnded) {
        txEnded = true;
        return true;
    }
    return false;
}

#endif /* CONFIG_P25_TX */

void OpMode_P25::abortTx()
{
#ifdef CONFIG_P25_TX
    /*
     * The supervisor unkeyed from another thread and left the status at OFF.
     * The streams are cancelled without waiting for the sink (this runs in the
     * main thread), the guard is closed so it stops asking for an abort that
     * has already happened, and startRx is what asks for the receiver on the
     * next pass.  txBlocked keeps a PTT that is still held from keying again
     * the moment the abort lands.
     */
    stopTx(false);

    txBlocked = true;
    startRx = true;
#endif
}

void OpMode_P25::update(rtxStatus_t *const rtx, const bool newCfg)
{
#ifdef CONFIG_P25_TX
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

#ifdef CONFIG_P25_TX
        case TX:
            /* PTT released: the terminator goes out, then the carrier drops */
            if ((platform_getPttStatus() == false) && (txEnded == false))
                p25Tx_end();

            if (txSlot()) {
                stopTx();
                rtx->opStatus = OFF;
                startRx = true;
            }
            return;
#endif

        default:
#ifdef CONFIG_P25_TX
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

#ifdef CONFIG_P25_TX
    /* PTT keys the transmitter: the receiver is stopped first, the BK4819 is
     * half duplex and both directions use the same audio path */
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
