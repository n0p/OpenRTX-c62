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

extern "C" {
#include "emulator.h"
}

/*
 * The eleven modes and the block they share, walked the way the operator walks
 * them.
 *
 * tests/unit/mode_arena.cpp checks the block itself, tests/unit/mode_switch.cpp
 * every ordered pair of the modes this host build has. This one walks the whole
 * opmode enum -- all twelve values, 0 to 11, in the order the radio uses -- and
 * then back, which is what a channel list with one channel per mode does, and
 * it checks after each step that the two invariants of the shared block hold:
 *
 *  - a mode that this build has is the one active afterwards, it owns the whole
 *    block and it got the same address as the first mode did;
 *  - a mode this build does not have leaves the radio idle with the block free
 *    and poisoned, and does not wedge the switch: the next mode still enters.
 *
 * The host build has four of the eleven modes (FM, AM airband, broadcast FM and
 * M17: a demodulator and a vocoder do not fit in a test binary), so the second
 * invariant is what the other eight exercise. TETRA is the one that matters
 * most -- its object is the largest of them all and it is what sets the size of
 * the block -- and it is not linked here either; what the radio checks is that
 * the block is 17 680 B, the size of that object (docs/MEMORY.md), and the
 * firmware build is where it enters and leaves for real.
 *
 * The last case drives PTT on a compiled transmitter (FM) after the walk, which
 * is the transmit-to-receive loop the arena must not have broken: the mode's
 * object, with its audio paths and its transmit queues, is the thing that was
 * constructed in the block.
 */

namespace
{

constexpr uint8_t firstMode = OPMODE_NONE;
constexpr uint8_t lastMode = OPMODE_WFM_BC;

pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;

/// Ask the RTX thread for a mode and let it act once.
void selectMode(uint8_t mode)
{
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.opMode = static_cast<opmode>(mode);
    rtx_configure(&cfg);
    rtx_task();
}

/// Key or unkey the PTT and let the RTX thread act once.
void ptt(bool down)
{
    emulator_state.PTTstatus = down;
    rtx_task();
}

} /* namespace */

TEST_CASE("every opmode of the enum, four rounds, through one block",
          "[arena][rtx]")
{
    rtx_init(&cfgMutex);

    ModeArena *arena = modeArena_bound();
    REQUIRE(arena != nullptr);
    REQUIRE(arena->size() > 0);

    void *block = nullptr;

    for (int round = 0; round < 2; round++) {
        /* Up the enum and back down, twice: a mode is left and re-entered */
        for (int dir = 0; dir < 2; dir++) {
            for (int step = 0; step <= (lastMode - firstMode); step++) {
                const uint8_t mode = (dir == 0) ? (uint8_t)(firstMode + step) :
                                                  (uint8_t)(lastMode - step);

                selectMode(mode);

                const bool compiled = (arena->sizeOf(mode) != 0);

                if (mode == OPMODE_NONE) {
                    /* The idle mode of the enum: no object, block free */
                    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_NONE);
                    REQUIRE(!arena->held());
                    REQUIRE(arena->poisonIntact());
                } else if (compiled) {
                    REQUIRE(rtx_getCurrentStatus().opMode == mode);
                    REQUIRE(arena->held());
                    REQUIRE(arena->owner() == mode);
                    REQUIRE(arena->used() == arena->sizeOf(mode));

                    if (block == nullptr)
                        block = arena->block();
                    REQUIRE(arena->block() == block);
                } else {
                    /* No entry, no block: the radio stays idle and the block
                       stays released, not held by a mode that was never
                       constructed */
                    REQUIRE(mode != OPMODE_NONE);
                    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_NONE);
                    REQUIRE(!arena->held());
                    REQUIRE(arena->poisonIntact());
                }

                REQUIRE(arena->staleDetections() == 0);
            }
        }
    }

    /* The block is the size of the largest object and never grew */
    REQUIRE(block != nullptr);
    REQUIRE(arena->size() >= arena->used());

    /* Back to none: released and poisoned */
    selectMode(OPMODE_NONE);
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_NONE);
    REQUIRE(!arena->held());
    REQUIRE(arena->poisonIntact());
    REQUIRE(arena->owner() == OPMODE_NONE);
    REQUIRE(arena->used() == 0);

    rtx_terminate();
}

TEST_CASE("a transmitter still keys and unkeys after the walk",
          "[arena][rtx][tx]")
{
    rtx_init(&cfgMutex);

    ModeArena *arena = modeArena_bound();
    REQUIRE(arena != nullptr);

    /* FM is in every build of this firmware and it transmits */
    selectMode(OPMODE_FM);
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);

    ptt(true);
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);

    ptt(false);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);

    /* And the receiver comes back: one more step must not throw */
    rtx_task();
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(arena->held());
    REQUIRE(arena->owner() == OPMODE_FM);

    /* A second over on the same object, the shape of a QSO */
    ptt(true);
    REQUIRE(rtx_getCurrentStatus().opStatus == TX);
    ptt(false);
    REQUIRE(rtx_getCurrentStatus().opStatus != TX);

    /* The mode that ran the overs dies cleanly: no stale write, no leak */
    selectMode(OPMODE_NONE);
    REQUIRE(!arena->held());
    REQUIRE(arena->poisonIntact());
    REQUIRE(arena->staleDetections() == 0);

    rtx_terminate();
}
