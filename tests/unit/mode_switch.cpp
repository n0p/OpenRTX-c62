/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <pthread.h>

#include "rtx/rtx.h"
#include "rtx/ModeArena.hpp"

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
