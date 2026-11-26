/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <pthread.h>

#include "rtx/rtx.h"
#include "rtx/ModeArena.hpp"

extern "C" {
#include "emulator/emulator.h"
#include "interfaces/radio.h"
#include "interfaces/delays.h"
#include "core/utils.h"
}

/* Counters of the emulator's transmitter (platform/drivers/baseband/radio_linux.cpp) */
unsigned radio_linux_keyCount(void);
unsigned radio_linux_refusedCount(void);
unsigned radio_linux_barrierCount(void);
void radio_linux_clearCounters(void);

/*
 * The mode switch, through the firmware's own entry points.
 *
 * tests/unit/mode_arena.cpp checks the block itself with stub modes. This one
 * drives the real thing: rtx_init() binds the arena, rtx_configure() hands a
 * new status to the RTX thread and rtx_task() performs the change, exactly as
 * the radio does at run time. The modes are the ones this build has (the Linux
 * emulator has FM, AM airband, broadcast FM and M17), and the test walks every
 * ordered pair of them, twice, so a mode is left and re-entered.
 *
 * What it can see from outside is what the UI sees: the status the RTX thread
 * publishes and the arena's own report. The switch is judged on that -- the
 * requested mode is the one active afterwards, the block is the same memory
 * every time, and nothing is left held between modes.
 */

namespace
{

constexpr uint8_t hostModes[] = {
    OPMODE_FM,
    OPMODE_AIRBAND,
    OPMODE_WFM_BC,
    OPMODE_M17,
};

constexpr size_t hostModeCount = sizeof(hostModes) / sizeof(hostModes[0]);

pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;

/// Ask the RTX thread for a mode and let it act once.
void selectMode(uint8_t mode)
{
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.opMode = static_cast<opmode>(mode);
    /*
     * A power to key with: the gate refuses a transmission of 0 mW, because a
     * PA with nothing behind it is the failure docs/TX_SAFETY.md ends.  The
     * radio takes this from the channel or from settings_t.tx_power; this test
     * has neither, so it says so here.
     */
    cfg.txPower = 500;
    rtx_configure(&cfg);
    rtx_task();
}

} /* namespace */

TEST_CASE("the firmware switches modes through one block", "[arena][rtx]")
{
    rtx_init(&cfgMutex);

    ModeArena *arena = modeArena_bound();
    REQUIRE(arena != nullptr);
    REQUIRE(!arena->held());
    REQUIRE(arena->size() > 0);

    /* FM from NONE: the first mode is constructed in the block */
    selectMode(OPMODE_FM);
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(arena->held());
    REQUIRE(arena->owner() == OPMODE_FM);
    void *first = arena->block();

    /* Every ordered pair, twice: out of one mode and into the next */
    for (int round = 0; round < 2; round++) {
        for (size_t from = 0; from < hostModeCount; from++) {
            for (size_t to = 0; to < hostModeCount; to++) {
                selectMode(hostModes[from]);
                REQUIRE(rtx_getCurrentStatus().opMode == hostModes[from]);
                REQUIRE(arena->owner() == hostModes[from]);

                selectMode(hostModes[to]);
                REQUIRE(rtx_getCurrentStatus().opMode == hostModes[to]);
                REQUIRE(arena->owner() == hostModes[to]);

                /* Same memory, and no stale write since the last release */
                REQUIRE(arena->block() == first);
                REQUIRE(arena->staleDetections() == 0);
            }
        }
    }

    /* Back to none: the object dies, the block goes back poisoned */
    selectMode(OPMODE_NONE);
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_NONE);
    REQUIRE(!arena->held());
    REQUIRE(arena->poisonIntact());

    /* And the mode that was active before is enterable again */
    selectMode(OPMODE_M17);
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_M17);
    REQUIRE(arena->block() == first);
    selectMode(OPMODE_NONE);

    rtx_terminate();
}

TEST_CASE("a mode this build lacks leaves the radio idle", "[arena][rtx]")
{
    rtx_init(&cfgMutex);
    ModeArena *arena = modeArena_bound();
    REQUIRE(arena != nullptr);

    /*
     * OPMODE_DMR is in the enum but no DMR mode is compiled into this build:
     * the registry has no entry, so there is no block to construct it in. The
     * radio stays on NONE, idle, and does not keep retrying on every update.
     */
    selectMode(OPMODE_DMR);
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_NONE);
    REQUIRE(!arena->held());
    REQUIRE(arena->poisonIntact());

    /* Nothing changed behind our back on the next update either */
    rtx_task();
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_NONE);
    REQUIRE(!arena->held());

    /* A mode that does exist still enters afterwards */
    selectMode(OPMODE_FM);
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(arena->owner() == OPMODE_FM);

    selectMode(OPMODE_NONE);
    rtx_terminate();
}

/*
 * A change of mode must not key the radio.
 *
 * This is the incident: the radio was flashed with a build whose DMR mode went
 * on the air when it was selected, with nobody pressing the PTT, and the
 * transmitter then stayed keyed. The vectors below are the three ways a mode
 * could still reach the PA, and every one of them has to leave the radio
 * receiving:
 *
 *  1. the PTT reads pressed while the mode is changed -- held across it, a
 *     stale level, a pin sampled while the chip is being reconfigured;
 *  2. a queued message: the DMR data path keys without a PTT by design, so it
 *     must wait for the change to settle;
 *  3. any caller at all: the driver refuses to key unless the RTX layer
 *     permits it, which is what makes a mode that forgets harmless.
 *
 * The host build has four of the eleven modes; the others are in the enum but
 * not in the registry, and asking for one leaves the radio idle, so they are
 * walked too -- the vector is "select it, the PA must stay off".
 */

namespace
{

constexpr uint8_t allModes[] = {
    OPMODE_NONE, OPMODE_FM, OPMODE_DMR, OPMODE_M17, OPMODE_TETRA, OPMODE_NXDN,
    OPMODE_DPMR, OPMODE_P25, OPMODE_YSF, OPMODE_DSTAR, OPMODE_AIRBAND,
    OPMODE_WFM_BC,
};

constexpr size_t allModeCount = sizeof(allModes) / sizeof(allModes[0]);

/// Let the RTX thread run n times, as the radio does every 30 ms.
void ticks(unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        rtx_task();
}

} /* namespace */

TEST_CASE("a mode change with the PTT read as pressed never keys the radio",
          "[arena][rtx][tx]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    radio_linux_clearCounters();

    /* Start from a mode, with the PTT up, so the gate has seen a release */
    selectMode(OPMODE_FM);
    ticks(2);

    for (size_t i = 0; i < allModeCount; i++)
    {
        const uint8_t mode = allModes[i];

        /* The PTT reads pressed *before* the change and stays pressed after */
        emulator_state.PTTstatus = true;
        radio_linux_clearCounters();

        selectMode(mode);
        ticks(8); /* 240 ms: the guard and the settle have both expired */

        INFO("mode " << (int)mode);
        REQUIRE(radio_linux_keyCount() == 0);
        REQUIRE(rtx_getCurrentStatus().opStatus != TX);
        REQUIRE(radio_txPathOff());

        /* Back to a known state for the next vector */
        emulator_state.PTTstatus = false;
        ticks(2);
    }

    selectMode(OPMODE_NONE);
    rtx_terminate();
}

TEST_CASE("a press during the change cannot key the new mode", "[rtx][tx]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    selectMode(OPMODE_WFM_BC);
    ticks(2);
    radio_linux_clearCounters();

    /*
     * The change and the press happen before the RTX thread sees either: the
     * gate is transitioned and the level it finds is a press it never saw
     * released, which is the case that used to key. The target is FM because it
     * is the transmitter the host can run end to end.
     */
    emulator_state.PTTstatus = true;
    selectMode(OPMODE_FM);
    ticks(10);

    REQUIRE(radio_linux_keyCount() == 0);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);

    /* Released and pressed again: now it is the operator, and it keys */
    emulator_state.PTTstatus = false;
    ticks(2);
    emulator_state.PTTstatus = true;
    ticks(4);

    REQUIRE(radio_linux_keyCount() == 1);
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);

    /* Released: the radio goes back to receiving */
    emulator_state.PTTstatus = false;
    ticks(3);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_txPathOff());

    selectMode(OPMODE_NONE);
    rtx_terminate();
}

TEST_CASE("the driver refuses to key outside a permitted transmission",
          "[rtx][tx]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    selectMode(OPMODE_FM);
    ticks(2);
    radio_linux_clearCounters();

    /*
     * The choke point: any path that reaches radio_enableTx() without the gate
     * -- another thread, a mode that forgot, a call in the middle of a change --
     * gets no PA. This is what covers the seven modes the host cannot run.
     */
    radio_enableTx();
    REQUIRE(radio_linux_keyCount() == 0);
    REQUIRE(radio_linux_refusedCount() == 1);
    REQUIRE(radio_txPathOff());

    selectMode(OPMODE_NONE);
    rtx_terminate();
}

TEST_CASE("a mode that cannot be entered leaves the PA off and the radio in RX",
          "[arena][rtx][tx]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = true; /* even with the PTT held */
    radio_linux_clearCounters();

    /* DMR is not in the host build: no block, no mode, no transmitter */
    selectMode(OPMODE_DMR);
    ticks(6);

    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_NONE);
    REQUIRE(radio_linux_keyCount() == 0);
    REQUIRE(radio_txPathOff());
    REQUIRE(radio_getStatus() == RX);

    emulator_state.PTTstatus = false;
    selectMode(OPMODE_NONE);
    rtx_terminate();
}

TEST_CASE("the transmit supervisor unkeys and leaves the radio in RX",
          "[rtx][tx][watchdog]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    selectMode(OPMODE_FM);
    ticks(2);

    /* Key it through the normal path */
    emulator_state.PTTstatus = true;
    ticks(4);
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);
    REQUIRE(!radio_txPathOff());

    /*
     * Simulate the stall the supervisor exists for: a transmission whose streams
     * opened and whose first block never arrived, long past its budget.
     */
    struct txGuard *guard = rtx_getTxGuard();
    guard->phase = TXG_PHASE_WAIT_FIRST;
    guard->streamTime = (uint32_t)getTick() - (TXG_FIRST_MS + 100);

    rtx_supervise();
    REQUIRE(radio_getStatus() != TX);
    REQUIRE(radio_txPathOff());

    /* The RTX thread unwinds: PTT released, the radio is back in RX */
    emulator_state.PTTstatus = false;
    ticks(3);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_getStatus() == RX);

    selectMode(OPMODE_NONE);
    rtx_terminate();
}

TEST_CASE("the transmit clock cuts at 90 s with the RTX thread blocked",
          "[rtx][tx][watchdog]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    selectMode(OPMODE_FM);
    ticks(2);

    /* Key it through the normal path */
    emulator_state.PTTstatus = true;
    ticks(4);
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);

    /*
     * The thread that owns the chip never runs again: the one thing that has
     * to keep going is the supervisor, in the main thread. The clock is
     * advanced the way 90 s of air would have advanced it.
     */
    struct txWatch *watch = rtx_getTxWatch();
    watch->keyed = true;
    watch->airMs = 0;
    watch->keyedAt = (uint32_t)getTick() - (TXW_LIMIT_MS + 100);

    rtx_supervise();

    /* PA off at the first cut, and the clock latched */
    REQUIRE(radio_getStatus() != TX);
    REQUIRE(radio_txPathOff());
    REQUIRE(txw_blocked(watch));
    REQUIRE(watch->cuts == 1);
    REQUIRE(std::string(rtx_txWatchReason()) == "timeout");

    /* Nothing keys again while the bar is spent, whatever the gate says */
    rtxStatus_t cfg = rtx_getCurrentStatus();
    REQUIRE_FALSE(rtx_keyingAllowed(&cfg));
    REQUIRE_FALSE(rtx_txPermitted());
    REQUIRE_FALSE(rtx_unattendedTxAllowed(&cfg));

    /* The RTX thread comes back: the abort is taken and the radio is in RX */
    emulator_state.PTTstatus = false;
    ticks(3);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);
    REQUIRE(radio_getStatus() == RX);

    selectMode(OPMODE_NONE);
    rtx_terminate();
}

TEST_CASE("the driver's own barrier cuts a keying in a loop",
          "[rtx][tx][watchdog]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    selectMode(OPMODE_FM);
    ticks(2);
    radio_linux_clearCounters();

    /* Key it normally, then let the air run past the driver's margin */
    emulator_state.PTTstatus = true;
    ticks(4);
    REQUIRE(radio_getStatus() == TX);
    const unsigned keyedOnce = radio_linux_keyCount();
    REQUIRE(keyedOnce >= 1);

    struct txWatch *barrier = radio_txWatch();
    barrier->keyed = true;
    barrier->airMs = 0;
    barrier->keyedAt = (uint32_t)getTick() - (TXW_DRIVER_LIMIT_MS + 100);

    /*
     * The caller that keeps trying: every one of these is refused and puts the
     * lines back down, which is what the radio's version does before it even
     * looks at the frequency.
     */
    for (unsigned i = 0; i < 8; i++)
    {
        radio_enableTx();
        REQUIRE(radio_getStatus() != TX);
        REQUIRE(radio_txPathOff());
    }

    REQUIRE(radio_linux_barrierCount() == 8);
    REQUIRE(txw_blocked(barrier));

    /* The key count did not move: not one of those attempts keyed anything */
    REQUIRE(radio_linux_keyCount() == keyedOnce);

    emulator_state.PTTstatus = false;
    ticks(3);
    selectMode(OPMODE_NONE);
    rtx_terminate();
}

TEST_CASE("the driver forces the PA off before anything else at init",
          "[rtx][tx][watchdog]")
{
    /* radio_init() is the driver's boot path: nothing may be keyed after it */
    rtx_init(&cfgMutex);
    REQUIRE(radio_txPathOff());
    REQUIRE(radio_getStatus() != TX);
    REQUIRE(txw_airMs(radio_txWatch(), (uint32_t)getTick()) == 0);

    rtx_terminate();
}
