/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The FM squelch, and the two analogue audio routes it controls.
 *
 * The squelch is the part of the analogue mode an operator judges a whole radio
 * by, and until now only the S-level scale under it had a test
 * (tests/unit/slevel.cpp). What is checked here is the mode's own rule, run
 * through the firmware's door -- rtx_configure() + rtx_task(), the pair the RTX
 * thread calls -- with the receiver's RSSI driven from the emulator:
 *
 *  - the squelch opens strictly above the S-level the operator set and closes
 *    strictly below it, with 1 dB of hysteresis on each side, so a carrier
 *    sitting on the threshold does not chatter the audio in and out;
 *  - the level is the setting, not a constant: moving the squelch level while
 *    the carrier stays put opens and closes the audio;
 *  - an RX CTCSS tone decides instead of the carrier when tone squelch is on:
 *    a full-scale carrier with no tone stays shut, and the tone opens the audio
 *    on a signal the RF squelch would not;
 *  - the audio route is a *route*: one SOURCE_RTX -> SINK_SPK path while the
 *    squelch is open and none when it is not, and a transmission swaps it for
 *    SOURCE_MIC -> SINK_RTX and leaves neither behind (the "stuck route" that
 *    silences the radio after a press).
 *
 * The investigation of a route is audioPath_getInfo() over the id range, which
 * is the table's own read-back and needs no cooperation from the mode.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <pthread.h>

extern "C" {
#include "emulator/emulator.h"
#include "interfaces/radio.h"
#include "interfaces/delays.h"
#include "core/utils.h"
#include "core/audio_path.h"
#include "rtx/rtx.h"
}

/* The stub driver's hooks (platform/drivers/baseband/radio_linux.cpp) */
void radio_linux_setDigitalSquelch(bool present);
unsigned radio_linux_keyCount(void);
void radio_linux_clearCounters(void);

namespace
{

pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;

/* The S-level used for the squelch tests: S8 is -79 dBm. */
constexpr uint8_t SQ_LEVEL = 8;
constexpr rssi_t  SQ_RSSI  = -79;

/**
 * How many open routes go from @p src to @p dst right now?
 *
 * The path ids are handed out by a counter that never repeats one, so walking
 * a generous range finds every route the table holds. The count matters as much
 * as the presence: a route opened twice is the leak that ends with no audio.
 */
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

bool rxRoute() { return routeCount(SOURCE_RTX, SINK_SPK) > 0; }
bool txRoute() { return routeCount(SOURCE_MIC, SINK_RTX) > 0; }

/// Release every route the table still holds, whatever opened it.
void drainRoutes()
{
    for (pathId id = 1; id < 256; id++) {
        pathInfo_t info = audioPath_getInfo(id);
        if (info.status != PATH_CLOSED)
            audioPath_release(id);
    }
}

/// Enter FM receiving, with the squelch at S8 and the receiver quiet.
void enterFm()
{
    /*
     * Each case starts from a table with no route in it: a path left open by
     * the case before would hold the receiver's slot at the same priority, and
     * the mode's next request would be refused -- which is a leftover of the
     * test, not a property of the radio.
     */
    drainRoutes();

    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    emulator_state.RSSI = -120.0f;
    radio_linux_clearCounters();
    radio_linux_setDigitalSquelch(false);

    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.opMode     = OPMODE_FM;
    cfg.rxFrequency = 145500000;
    cfg.txFrequency = 145500000;
    cfg.sqlLevel   = SQ_LEVEL;
    cfg.rxToneEn   = 0;
    cfg.txToneEn   = 0;
    cfg.txPower    = 500;   /* the gate refuses to key 0 mW */
    rtx_configure(&cfg);
    rtx_task();
}

/// Let the RTX thread run, and with it the RSSI filter, until it settles.
void ticks(unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        rtx_task();
}

/// Put a carrier of @p dbm at the antenna and let the filter settle.
void signal(rssi_t dbm)
{
    emulator_state.RSSI = (float)dbm;
    ticks(8);
}

} /* namespace */

TEST_CASE("the squelch opens above its level and closes below it",
          "[fm][squelch][rtx]")
{
    enterFm();
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(radio_getStatus() == RX);

    /* Quiet: shut, and no audio reaching the speaker */
    signal(-120);
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE_FALSE(rxRoute());

    /* Two dB over the level: open, and one route to the speaker */
    signal(SQ_RSSI + 2);
    REQUIRE(rtx_rxSquelchOpen());
    REQUIRE(rxRoute());

    /* Two dB under: shut again, and the route is gone */
    signal(SQ_RSSI - 2);
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE_FALSE(rxRoute());

    rtx_terminate();
}

TEST_CASE("the squelch has 1 dB of hysteresis on both sides",
          "[fm][squelch][rtx]")
{
    enterFm();

    /*
     * The band between level-1 and level+1 is the hysteresis: inside it the
     * squelch keeps the state it had, which is what stops a signal sitting on
     * the threshold from chopping the audio on every RSSI sample.
     */
    const rssi_t band[] = { SQ_RSSI - 1, SQ_RSSI, SQ_RSSI + 1 };

    /* Coming from below: the whole band stays shut, level+1 included */
    signal(SQ_RSSI - 5);
    REQUIRE_FALSE(rtx_rxSquelchOpen());

    for (rssi_t rssi : band) {
        signal(rssi);
        INFO("rssi " << rssi << " with the squelch shut");
        REQUIRE_FALSE(rtx_rxSquelchOpen());
        REQUIRE_FALSE(rxRoute());
    }

    /* Coming from above: the whole band stays open, level-1 included */
    signal(SQ_RSSI + 5);
    REQUIRE(rtx_rxSquelchOpen());

    for (rssi_t rssi : band) {
        signal(rssi);
        INFO("rssi " << rssi << " with the squelch open");
        REQUIRE(rtx_rxSquelchOpen());
        REQUIRE(rxRoute());
    }

    /* And the edges are edges: one dB further closes it */
    signal(SQ_RSSI - 2);
    REQUIRE_FALSE(rtx_rxSquelchOpen());

    /* One dB further above, from shut, opens it */
    signal(SQ_RSSI + 2);
    REQUIRE(rtx_rxSquelchOpen());

    rtx_terminate();
}

TEST_CASE("the squelch level is the operator's, live", "[fm][squelch][rtx]")
{
    enterFm();

    /* A carrier the S8 squelch does not hear */
    signal(-100);
    REQUIRE_FALSE(rtx_rxSquelchOpen());

    /* The operator lowers the squelch to S2 (-115 dBm): the same carrier now
     * opens it, which is the setting being the threshold and not a table
     * baked into the mode */
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.sqlLevel = 2;
    rtx_configure(&cfg);
    rtx_task();
    ticks(4);
    REQUIRE(rtx_rxSquelchOpen());
    REQUIRE(rxRoute());

    /* Raised above the carrier, it shuts again */
    cfg.sqlLevel = SLEVEL_MAX;   /* S9 + 20 dB = -53 dBm */
    rtx_configure(&cfg);
    rtx_task();
    ticks(4);
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE_FALSE(rxRoute());

    rtx_terminate();
}

TEST_CASE("with an RX tone the tone opens the audio, not the carrier",
          "[fm][squelch][ctcss][rtx]")
{
    enterFm();

    /* Tone squelch on, no tone: a full-scale carrier is still not audio */
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.rxToneEn = 1;
    cfg.rxTone   = 885;      /* 88.5 Hz, tenths of a hertz */
    rtx_configure(&cfg);
    rtx_task();

    signal(-53);             /* S9 + 20 dB, the strongest the meter has */
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE_FALSE(rxRoute());

    /* The chip hears the tone: audio, on the same carrier */
    radio_linux_setDigitalSquelch(true);
    ticks(2);
    REQUIRE(rtx_rxSquelchOpen());
    REQUIRE(rxRoute());

    /* The tone goes away: the audio closes even with the carrier still there */
    radio_linux_setDigitalSquelch(false);
    ticks(2);
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE_FALSE(rxRoute());

    /*
     * And it works the other way round: a signal too weak for the RF squelch
     * (S2) still opens on the tone, because with tone squelch on it is the
     * tone that decides.
     */
    signal(-110);
    REQUIRE_FALSE(rtx_rxSquelchOpen());

    radio_linux_setDigitalSquelch(true);
    ticks(2);
    REQUIRE(rtx_rxSquelchOpen());

    /* Tone squelch off: the tone alone no longer opens anything there */
    radio_linux_setDigitalSquelch(false);
    ticks(2);
    cfg = rtx_getCurrentStatus();
    cfg.rxToneEn = 0;
    rtx_configure(&cfg);
    rtx_task();
    radio_linux_setDigitalSquelch(true);
    ticks(4);
    REQUIRE_FALSE(rtx_rxSquelchOpen());
    REQUIRE_FALSE(rxRoute());

    radio_linux_setDigitalSquelch(false);
    rtx_terminate();
}

TEST_CASE("a transmission swaps the routes and leaves none behind",
          "[fm][squelch][audio][rtx][tx]")
{
    enterFm();

    /* Receiving a carrier: the speaker route is the one open, and only one */
    signal(SQ_RSSI + 6);
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 1);
    REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 0);
    REQUIRE(radio_linux_keyCount() == 0);

    /* The carrier goes: the audio route closes with it */
    signal(-120);
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 0);

    /* Key it the way the operator does: press and hold */
    emulator_state.PTTstatus = true;
    ticks(6);
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);
    REQUIRE(radio_getStatus() == TX);
    REQUIRE(radio_linux_keyCount() == 1);

    /* The microphone is on the air and the speaker is not */
    INFO("during the transmission");
    REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 1);
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 0);

    /* Release: the transmitter's route goes, and nothing replaces it while
     * there is no carrier to listen to */
    emulator_state.PTTstatus = false;
    ticks(4);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_getStatus() == RX);
    INFO("after the transmission");
    REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 0);
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 0);

    /* The carrier comes back: the squelch reopens the audio by itself, once,
     * which is what proves the route was released and not merely lost */
    signal(SQ_RSSI + 6);
    REQUIRE(rtx_rxSquelchOpen());
    REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 1);

    /* Three presses in a row: one route each time, none accumulating */
    for (int i = 0; i < 3; i++) {
        emulator_state.PTTstatus = true;
        ticks(6);
        REQUIRE(rtx_getCurrentStatus().opStatus == TX);
        REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 1);
        REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 0);

        emulator_state.PTTstatus = false;
        ticks(4);
        REQUIRE(rtx_getCurrentStatus().opStatus != TX);
        REQUIRE(routeCount(SOURCE_MIC, SINK_RTX) == 0);
        REQUIRE(routeCount(SOURCE_RTX, SINK_SPK) == 1);
    }

    REQUIRE(radio_linux_keyCount() == 4);
    REQUIRE(radio_txPathOff());

    rtx_terminate();
}

TEST_CASE("a press is refused when the mode has no power to key with",
          "[fm][squelch][tx][rtx]")
{
    enterFm();

    /* 0 mW: the gate refuses, and the mode must not claim to be transmitting */
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.txPower = 0;
    rtx_configure(&cfg);
    rtx_task();
    ticks(4);

    emulator_state.PTTstatus = true;
    ticks(6);

    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_linux_keyCount() == 0);
    REQUIRE_FALSE(txRoute());
    REQUIRE(radio_txPathOff());

    emulator_state.PTTstatus = false;
    ticks(2);
    rtx_terminate();
}
