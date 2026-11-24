/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "rtx/OpMode_Airband.hpp"
#include "rtx/rtx.h"
#include "core/airband.h"
#include "core/utils.h"

/*
 * Squelch. The level the operator sets is the RSSI at which the squelch opens,
 * with the same S-meter scale the FM mode uses; the carrier has to fall 3 dB
 * below it to close again, and the squelch stays open for a moment after the
 * carrier is gone so that a faded syllable does not cut the audio. AM audio
 * follows the carrier level, so there is nothing else to detect: the noise a
 * noise squelch listens to is exactly what the AGC will bring up.
 */
static constexpr rssi_t   SQL_OPEN_MARGIN  = 1;   /* dB above the level */
static constexpr rssi_t   SQL_CLOSE_MARGIN = 2;   /* dB below the level */
static constexpr uint32_t SQL_HANG_MS      = 400; /* ms of hang time    */

/*
 * Transmit confirmation. The first press of the PTT on a frequency of the band
 * puts the warning on screen and keys nothing; the mode arms itself only on a
 * second press, which is a deliberate act with the warning in view. The warning
 * goes away on its own after this long, and the mode is locked again, so that a
 * radio left with the prompt on screen does not transmit for whoever picks it
 * up next.
 */
#ifdef CONFIG_C62_AIR_TX
static constexpr uint32_t TX_PROMPT_MS = 8000;
#endif

OpMode_Airband::OpMode_Airband()
    : carrierOpen(false), sqlOpen(false), enterRx(true), hangUntil(0),
      pttLatch(false), promptSince(0), txAudioPath(0)
{
}

OpMode_Airband::~OpMode_Airband()
{
}

void OpMode_Airband::enable()
{
    carrierOpen = false;
    sqlOpen     = false;
    enterRx     = true;
    hangUntil   = 0;
    pttLatch    = false;
    promptSince = 0;
    txAudioPath = 0;
}

void OpMode_Airband::disable()
{
    airband_txSetState(AIRBAND_TX_LOCKED);
    promptSince = 0;

    if (txAudioPath > 0) {
        audioPath_release(txAudioPath);
        txAudioPath = 0;
    }

    audioPath_release(rxAudioPath);
    radio_disableRtx();

    carrierOpen = false;
    sqlOpen     = false;
    enterRx     = false;
}

#ifdef CONFIG_C62_AIR_TX
/**
 * Stop a transmission: close the microphone path and turn the radio around.
 * The driver ramps the carrier down before it drops the PA, so this is not a
 * cut.
 *
 * @param status: RTX status, set back to RX.
 */
void OpMode_Airband::stopTx(rtxStatus_t *const status)
{
    if (txAudioPath > 0) {
        audioPath_release(txAudioPath);
        txAudioPath = 0;
    }

    radio_disableRtx();

    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_PTT);

    /*
     * OFF with enterRx, as the FM mode does: the next update starts the
     * receiver again, and nothing touches the radio in between. Setting RX
     * here would leave the receiver powered down and the squelch waiting.
     */
    status->opStatus = OFF;
    enterRx = true;
    sqlOpen = false;
    carrierOpen = false;
}

/**
 * Ask the operator, or key the transmitter.
 *
 * @param status: RTX status.
 */
void OpMode_Airband::requestTx(rtxStatus_t *const status)
{
    const uint32_t now = (uint32_t)getTick();

    /*
     * The mode only transmits inside the band: outside it the AM profile means
     * nothing, the PA has no calibration there and there is no reason to key.
     * It is a simplex mode, so when the transmit frequency is not in the band
     * -- a channel stored as receive only, or a VFO that the UI never filled a
     * transmit field for -- the receiver's frequency is the one to use, and it
     * is written back so that the driver and the screen agree.
     */
    if (!airband_inBand(status->txFrequency)) {
        if (!airband_inBand(status->rxFrequency))
            return;

        status->txFrequency = status->rxFrequency;
    }

    switch (airband_txState()) {
        case AIRBAND_TX_ARMED:
            break;

        case AIRBAND_TX_PROMPT:
            /* Second press, with the warning on screen: that is the consent */
            airband_txSetState(AIRBAND_TX_ARMED);
            promptSince = 0;
            break;

        case AIRBAND_TX_LOCKED:
        default:
            airband_txSetState(AIRBAND_TX_PROMPT);
            promptSince = now;
            return;
    }

    if (status->txDisable != 0)
        return;

    audioPath_release(rxAudioPath);
    sqlOpen = false;

    radio_disableRtx();

    txAudioPath = audioPath_request(SOURCE_MIC, SINK_RTX, PRIO_TX);
    radio_enableTx();

    if (radio_getStatus() != TX) {
        /* The driver refused: no transmitter in this build, or no APC line */
        if (txAudioPath > 0) {
            audioPath_release(txAudioPath);
            txAudioPath = 0;
        }
        status->opStatus = RX;
        enterRx = true;
        return;
    }

    /*
     * The transmit supervisor: from here on a PTT that never comes back, or a
     * transmission that outlives TXG_MAX_TX_MS, is cut from the main thread.
     * The streams are reported open at once because this mode has no stream to
     * wait for: the modulator produces the envelope from the first block the
     * codec delivers.
     */
    struct txGuard *tx = rtx_getTxGuard();
    txg_enter(tx, now);
    txg_streamsOpen(tx, now);
    txg_firstBlock(tx, now);

    status->opStatus = TX;
}
#endif /* CONFIG_C62_AIR_TX */

void OpMode_Airband::update(rtxStatus_t *const status, const bool newCfg)
{
    (void)newCfg;

    const uint32_t now = (uint32_t)getTick();

#ifndef CONFIG_C62_AIR_TX
    (void)now;
    /*
     * Receive only. A held PTT is not refused with silence: the status goes
     * back to RX and the mode keeps receiving, which is what the operator sees
     * on the screen as well.
     */
    if (status->opStatus == TX)
        status->opStatus = RX;
#else
    /* The warning is not a screen: it expires and locks the mode again */
    if ((airband_txState() == AIRBAND_TX_PROMPT)
        && ((now - promptSince) > TX_PROMPT_MS)) {
        airband_txSetState(AIRBAND_TX_LOCKED);
        promptSince = 0;
    }

    /* Leaving the armed state from the quick menu stops a transmission */
    if ((status->opStatus == TX) && (airband_txState() != AIRBAND_TX_ARMED))
        stopTx(status);
#endif /* CONFIG_C62_AIR_TX */

    /*
     * Entering the mode, and coming back from a transmission: the receiver is
     * started here, once, and only when the radio is off.
     */
    if ((status->opStatus == OFF) && enterRx) {
        radio_disableRtx();
        radio_enableRx();
        status->opStatus = RX;
        enterRx = false;
    }

#ifdef CONFIG_C62_AIR_TX
    /*
     * The PTT is read here rather than in the UI: the mode owns the keying
     * policy, and the confirmation is part of it. Edge detection keeps the
     * press that opened the warning from being the one that confirms it.
     */
    const bool ptt = platform_getPttStatus();

    if (ptt && (!pttLatch)) {
        pttLatch = true;

        if (status->opStatus != TX)
            requestTx(status);
    } else if (!ptt) {
        pttLatch = false;

        if (status->opStatus == TX)
            stopTx(status);
    }
#endif

    if (status->opStatus != RX)
        return;

    const rssi_t   level = sLevelToRssi(status->sqlLevel);
    const rssi_t   rssi  = rtx_getRssi();

    if ((carrierOpen == false) && (rssi > (level + SQL_OPEN_MARGIN))) {
        carrierOpen = true;
        hangUntil   = now + SQL_HANG_MS;
    }

    if (carrierOpen) {
        if (rssi > (level - SQL_CLOSE_MARGIN)) {
            hangUntil = now + SQL_HANG_MS;
        } else if ((int32_t)(now - hangUntil) >= 0) {
            carrierOpen = false;
        }
    }

    if ((sqlOpen == false) && carrierOpen) {
        rxAudioPath = audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX);
        if (rxAudioPath > 0)
            sqlOpen = true;
    } else if ((sqlOpen == true) && (carrierOpen == false)) {
        audioPath_release(rxAudioPath);
        sqlOpen = false;
    }
}

#ifdef CONFIG_C62_AIR_TX
void OpMode_Airband::abortTx()
{
    /*
     * The supervisor unkeyed the radio from another thread: drop the
     * microphone path and make sure the receiver comes back, because the
     * status the supervisor leaves is OFF and this mode would otherwise wait
     * for a PTT that is not coming.
     */
    if (txAudioPath > 0) {
        audioPath_release(txAudioPath);
        txAudioPath = 0;
    }

    /* No new over until the PTT has been released: the abort was a fault */
    pttLatch = true;
    enterRx = true;
    sqlOpen = false;
    carrierOpen = false;
}
#endif

bool OpMode_Airband::rxSquelchOpen()
{
    return sqlOpen;
}
