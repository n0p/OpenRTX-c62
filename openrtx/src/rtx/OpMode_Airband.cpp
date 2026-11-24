/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "interfaces/radio.h"
#include "rtx/OpMode_Airband.hpp"
#include "rtx/rtx.h"
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

OpMode_Airband::OpMode_Airband()
    : carrierOpen(false), sqlOpen(false), enterRx(true), hangUntil(0)
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
}

void OpMode_Airband::disable()
{
    audioPath_release(rxAudioPath);
    radio_disableRtx();

    carrierOpen = false;
    sqlOpen     = false;
    enterRx     = false;
}

void OpMode_Airband::update(rtxStatus_t *const status, const bool newCfg)
{
    (void)newCfg;

    /*
     * Receive only. A held PTT is not refused with silence: the status goes
     * back to RX and the mode keeps receiving, which is what the operator sees
     * on the screen as well.
     */
    if (status->opStatus == TX)
        status->opStatus = RX;

    if ((status->opStatus == OFF) && enterRx) {
        radio_enableRx();
        status->opStatus = RX;
        enterRx = false;
    }

    if (status->opStatus != RX)
        return;

    const rssi_t   level = sLevelToRssi(status->sqlLevel);
    const rssi_t   rssi  = rtx_getRssi();
    const uint32_t now   = (uint32_t)getTick();

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

bool OpMode_Airband::rxSquelchOpen()
{
    return sqlOpen;
}
