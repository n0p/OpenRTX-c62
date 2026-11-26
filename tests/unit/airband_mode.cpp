/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The airband mode (AM), walked through the firmware's own door: rtx_configure()
 * plus rtx_task(), the receiver's RSSI from the emulator and the real
 * OpMode_Airband.
 *
 * This binary is built with CONFIG_C62_AIR_TX, the option the shipped image
 * carries, so the transmitter half of the mode is compiled in and tested here.
 * It is the one mode whose keying is a dialogue with the operator: the first
 * press only puts a warning on screen, and the second, with the warning up, is
 * the consent. Everything else about AM on this radio is either arithmetic
 * (the band plan, tests/unit/airband.cpp), the modulator (tests/unit/am_tx.cpp)
 * or the air itself.
 *
 *  1. The squelch. AM audio follows the carrier, so the squelch is a level and
 *     not a noise detector: it opens 1 dB above the setting, holds for 400 ms
 *     of hang time after the carrier drops below the setting, and closes 2 dB
 *     under it. The hang is what keeps a fading syllable from being chopped,
 *     and it is the part of this mode an operator notices first.
 *  2. Keying. Locked -> first press arms the warning and keys nothing ->
 *     second press keys -> release unkeys. A transmission that starts cannot
 *     outlive the armed state, and being outside the band keys nothing at all.
 *  3. The audio routes: the receiver's to the speaker while the squelch is
 *     open, the microphone's to the radio while transmitting, neither left
 *     behind.
 *  4. A change of mode never keys, whatever the PTT line says.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <pthread.h>

extern "C" {
#include "emulator/emulator.h"
#include "interfaces/radio.h"
#include "interfaces/delays.h"
#include "core/utils.h"
#include "core/airband.h"
#include "core/audio_path.h"
#include "rtx/rtx.h"
}

unsigned radio_linux_keyCount(void);
unsigned radio_linux_refusedCount(void);
void radio_linux_clearCounters(void);

namespace
{

pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;

/* S8 is -79 dBm, the same scale the FM mode's squelch uses. */
constexpr uint8_t SQ_LEVEL = 8;
constexpr rssi_t  SQ_RSSI  = -79;

/* The mode's own numbers (platform/targets/c62/... via docs/TX_AIRBAND.md) */
constexpr uint32_t HANG_MS = 400;

unsigned routeCount(uint8_t src, uint8_t dst)
{
    unsigned n = 0;

    for (pathId id = 1; id < 256; id++) {
        pathInfo_t info = audioPath_getInfo(id);
        if ((info.status == PATH_OPEN) && (info.source == src)
            && (info.sink == dst))
            n++;
    }
    return n;
}

void drainRoutes()
{
    for (pathId id = 1; id < 256; id++) {
        pathInfo_t info = audioPath_getInfo(id);
        if (info.status != PATH_CLOSED)
            audioPath_release(id);
    }
}

void ticks(unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        rtx_task();
}

void sleepMs(unsigned ms)
{
    delayMs(ms);
}

/// Enter the airband mode receiving, squelch at S8, nothing at the antenna.
void enterAm()
{
    drainRoutes();

    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    emulator_state.RSSI = -120.0f;
    radio_linux_clearCounters();

    /* Whatever the previous case left armed, this one starts locked */
    airband_txSetState(AIRBAND_TX_LOCKED);

    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.opMode      = OPMODE_AIRBAND;
    cfg.rxFrequency = 118000000;
    cfg.txFrequency = 118000000;
    cfg.sqlLevel    = SQ_LEVEL;
    cfg.txPower     = 500;
    cfg.rxToneEn    = 0;
    cfg.txToneEn    = 0;
    rtx_configure(&cfg);
    rtx_task();

    /*
     * Past the key gate's guard: a mode change vetoes keying for 150 ms after
     * it ends (rtx/KeyGate.h), and an operator reaching for the PTT takes far
     * longer than that. Without the wait every press here would be refused by
     * the guard and the test would be measuring the guard, not the mode.
     */
    sleepMs(200);
    ticks(2);
}

void signal(rssi_t dbm)
{
    emulator_state.RSSI = (float)dbm;
    ticks(8);
}

/*
 * The PTT edge the mode's own latch looks for.
 *
 * The press has to last: the key gate wants 25 ms of held line before it lets a
 * mode key, and this mode's update() does not sleep, so the ticks alone are
 * microseconds apart. Holding for 40 ms is what an operator's finger does and
 * what the gate is written for.
 */
void pressPtt()
{
    emulator_state.PTTstatus = true;

    for (unsigned i = 0; i < 4; i++) {
        rtx_task();
        sleepMs(12);
    }
}

void releasePtt()
{
    emulator_state.PTTstatus = false;
    ticks(3);
}

} /* namespace */

TEST_CASE("the AM squelch opens on the carrier and hangs before it shuts",
          "[airband][am][squelch]")
{
    enterAm();
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_AIRBAND);
    REQUIRE(radio_getStatus() == RX);

    /* Quiet band: shut, no audio */
    signal(-120);
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 0);

    /* A carrier two dB over the setting: open, and the route with it */
    signal(SQ_RSSI + 2);
    REQUIRE(rtx_rxSquelchOpen());
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 1);

    /*
     * The carrier drops below the closing margin, but the hang time has not
     * run out yet: the audio stays, which is what makes a faded syllable
     * intelligible on AM.
     */
    emulator_state.RSSI = (float)(SQ_RSSI - 20);
    ticks(6);
    REQUIRE(rtx_rxSquelchOpen());

    /* Past the hang time, it closes */
    sleepMs(HANG_MS + 100);
    ticks(2);
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 0);

    rtx_terminate();
}

TEST_CASE("the AM squelch has 1 dB of opening and 2 dB of closing margin",
          "[airband][am][squelch]")
{
    enterAm();

    /* At the setting and one dB over it, still shut */
    signal(SQ_RSSI);
    REQUIRE_FALSE(rtx_rxSquelchOpen());

    signal(SQ_RSSI + 1);
    REQUIRE_FALSE(rtx_rxSquelchOpen());

    /* Two over: open */
    signal(SQ_RSSI + 2);
    REQUIRE(rtx_rxSquelchOpen());

    /*
     * Open, it takes a drop below setting-2 to start the hang; setting-1 is
     * still inside the closing margin and refreshes the hang instead.
     */
    signal(SQ_RSSI - 1);
    REQUIRE(rtx_rxSquelchOpen());
    sleepMs(HANG_MS + 100);
    ticks(2);
    REQUIRE(rtx_rxSquelchOpen());

    /* Setting-3 starts the hang, and the hang then runs out */
    signal(SQ_RSSI - 3);
    sleepMs(HANG_MS + 100);
    ticks(2);
    REQUIRE_FALSE(rtx_rxSquelchOpen());

    rtx_terminate();
}

TEST_CASE("the operator's squelch setting is the threshold", "[airband][am]")
{
    enterAm();

    /* A weak but usable carrier: S8 does not hear it */
    signal(-100);
    REQUIRE_FALSE(rtx_rxSquelchOpen());

    /* The operator opens the squelch down to S2 (-115 dBm) */
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.sqlLevel = 2;
    rtx_configure(&cfg);
    rtx_task();
    ticks(4);
    REQUIRE(rtx_rxSquelchOpen());

    /* And up to S9 + 20 dB, above the carrier: the hang runs out and it shuts */
    cfg.sqlLevel = SLEVEL_MAX;
    rtx_configure(&cfg);
    rtx_task();
    ticks(4);
    sleepMs(HANG_MS + 100);
    ticks(2);
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 0);

    rtx_terminate();
}

TEST_CASE("the first press arms the transmitter and keys nothing",
          "[airband][am][tx]")
{
    enterAm();
    REQUIRE(airband_txState() == AIRBAND_TX_LOCKED);

    pressPtt();

    /*
     * The warning is up and the radio is still receiving: no PA, no
     * microphone route, no transmission. This is the whole point of the
     * dialogue -- an airband licence is not implied by a press.
     */
    REQUIRE(airband_txState() == AIRBAND_TX_PROMPT);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_getStatus() != TX);
    REQUIRE(radio_linux_keyCount() == 0);
    REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 0);

    /* Released and pressed again, with the warning up: now it keys */
    releasePtt();
    pressPtt();

    REQUIRE(airband_txState() == AIRBAND_TX_ARMED);
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);
    REQUIRE(radio_getStatus() == TX);
    REQUIRE(radio_linux_keyCount() == 1);
    REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 1);
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 0);

    /* Release: unkeyed, back to receiving, no route left behind */
    releasePtt();
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_getStatus() == RX);
    REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 0);
    REQUIRE(radio_txPathOff());

    /*
     * Armed from here on: one press transmits, because the operator has already
     * been told what pressing means in this band.
     */
    pressPtt();
    REQUIRE(airband_txState() == AIRBAND_TX_ARMED);
    REQUIRE(radio_linux_keyCount() == 2);
    releasePtt();

    rtx_terminate();
}

TEST_CASE("a transmission ends when the mode is no longer armed",
          "[airband][am][tx]")
{
    enterAm();

    /* Get to a transmitting state through the dialogue */
    pressPtt();
    releasePtt();
    pressPtt();
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);

    /* Leaving the armed state -- the quick menu, a mode change -- unkeys it */
    airband_txSetState(AIRBAND_TX_LOCKED);
    ticks(2);

    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_getStatus() == RX);
    REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 0);
    REQUIRE(radio_txPathOff());

    /* The PTT is still down and it must not re-key while locked */
    ticks(6);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_txPathOff());

    releasePtt();
    rtx_terminate();
}

TEST_CASE("outside the band nothing keys, whatever the state", "[airband][tx]")
{
    enterAm();

    /* A frequency the band does not have, on both fields */
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.rxFrequency = 145500000;
    cfg.txFrequency = 145500000;
    rtx_configure(&cfg);
    rtx_task();
    airband_txSetState(AIRBAND_TX_ARMED);

    pressPtt();
    ticks(6);

    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_getStatus() != TX);
    REQUIRE(radio_linux_keyCount() == 0);
    REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 0);

    /*
     * Simplex: with only the receive field inside the band, the mode uses it
     * for the transmission, which is what a channel stored as receive only or
     * a VFO with an empty transmit field looks like.
     */
    cfg = rtx_getCurrentStatus();
    cfg.rxFrequency = 121500000;
    cfg.txFrequency = 0;
    rtx_configure(&cfg);
    rtx_task();

    releasePtt();
    pressPtt();
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);
    REQUIRE(rtx_getCurrentStatus().txFrequency == 121500000);

    releasePtt();
    rtx_terminate();
}

TEST_CASE("a change of mode never keys the AM transmitter", "[airband][tx][rtx]")
{
    drainRoutes();
    rtx_init(&cfgMutex);
    radio_linux_clearCounters();

    /* The consent state is the mode's, and the previous case may have armed it:
     * a fresh entry starts locked whatever a test did before it. */
    airband_txSetState(AIRBAND_TX_LOCKED);

    /* FM first, so the change has somewhere to come from */
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.opMode = OPMODE_FM;
    cfg.rxFrequency = 145500000;
    cfg.txFrequency = 145500000;
    cfg.txPower = 500;
    rtx_configure(&cfg);
    rtx_task();
    ticks(2);

    /*
     * The PTT reads pressed before the change and stays pressed across it: the
     * gate must veto the keying and the new mode must not take the level as
     * consent. If it did, the first press of a warning dialogue would be
     * skipped and the radio would transmit AM on the air.
     */
    emulator_state.PTTstatus = true;
    radio_linux_clearCounters();

    cfg = rtx_getCurrentStatus();
    cfg.opMode = OPMODE_AIRBAND;
    cfg.rxFrequency = 118000000;
    cfg.txFrequency = 118000000;
    rtx_configure(&cfg);
    rtx_task();
    ticks(8);

    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_AIRBAND);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_linux_keyCount() == 0);
    REQUIRE(radio_txPathOff());
    REQUIRE(airband_txState() != AIRBAND_TX_ARMED);

    emulator_state.PTTstatus = false;
    ticks(2);
    cfg.opMode = OPMODE_NONE;
    rtx_configure(&cfg);
    rtx_task();
    rtx_terminate();
}
