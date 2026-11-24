/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>

#include "rtx/TxGuard.h"

/*
 * The transmit lifecycle, on the host.
 *
 * A transmission that never ends is a keyed PA, so the rule this test enforces
 * is simple: from every state, with every combination of what does and does not
 * happen next, the guard ends the transmission within a bounded time and says
 * why. The two numbers that matter on the air are asserted as well: the first
 * block cannot take more than TXG_FIRST_MS after the streams are open, and the
 * radio is out of transmit no later than TXG_DRAIN_MS after the PTT is released
 * (the UI and the operator ask for less than 200 ms).
 *
 * Ticks are 5 ms, the period of the main thread that calls the supervisor.
 */

namespace
{

constexpr uint32_t TICK = 5;

/// Run the supervisor until it aborts or the budget runs out.
struct Supervision
{
    uint32_t abortTime = 0;   ///< Time of the first abort request, 0 if none
    unsigned abortTicks = 0;  ///< How many ticks returned abort
    uint32_t elapsed = 0;     ///< Simulated time when the loop stopped
};

Supervision supervise(txGuard &guard, uint32_t start, uint32_t budgetMs,
                      bool ptt)
{
    Supervision out;
    uint32_t now = start;

    while (now - start < budgetMs) {
        now += TICK;
        if (txg_tick(&guard, now, ptt) == TXG_ACTION_ABORT) {
            if (out.abortTime == 0)
                out.abortTime = now;
            out.abortTicks++;
        }
    }
    out.elapsed = now;
    return out;
}

/// A transmission that works: PTT, streams, blocks every slot, release, exit.
void oneHealthyOver(txGuard &guard, uint32_t start, uint32_t holdMs)
{
    txg_enter(&guard, start);
    txg_streamsOpen(&guard, start + 10);
    txg_firstBlock(&guard, start + 40);

    uint32_t now = start + 40;
    while (now - start < holdMs) {
        now += TXG_BLOCK_MS;
        txg_block(&guard, now);
    }

    txg_finishing(&guard, now + 10);
    txg_exit(&guard, now + 80, TXG_REASON_PTT);
}

} // namespace

TEST_CASE("A healthy transmission meets its two deadlines", "[txguard]")
{
    txGuard guard;
    txg_init(&guard, 0);

    oneHealthyOver(guard, 1000, 3000);

    REQUIRE(guard.phase == TXG_PHASE_IDLE);
    REQUIRE(guard.reason == TXG_REASON_PTT);
    REQUIRE(guard.transmissions == 1);
    REQUIRE(guard.aborts == 0);
    /* First block 40 ms after the PTT, out of TX 70 ms after the PTT release */
    REQUIRE(guard.lastLatencyMs == 40);
    REQUIRE(guard.lastTailMs == 70);
    REQUIRE(guard.slips == 0);
    /* 3 s of slots at 30 ms, plus the one of the first block */
    REQUIRE(guard.blocks == 100);
}

TEST_CASE("A sink that never consumes aborts in the first-block budget",
          "[txguard]")
{
    /*
     * This is the failure on the radio: the streams open, the mode waits for
     * the first block that never comes -- because the codec worker is stuck, or
     * the DSP never started the playback -- and the RTX thread sits in the
     * stream while the PA is keyed.
     */
    txGuard guard;
    txg_init(&guard, 0);

    REQUIRE(txg_enter(&guard, 100) == true);
    txg_streamsOpen(&guard, 110);

    Supervision s = supervise(guard, 110, 2000, true);

    REQUIRE(s.abortTime != 0);
    /* Bounded by TXG_FIRST_MS from the stream start, plus at most one tick */
    REQUIRE(s.abortTime - 110 <= TXG_FIRST_MS + TICK);
    REQUIRE(guard.reason == TXG_REASON_TIMEOUT_FIRST);
    REQUIRE(guard.phase == TXG_PHASE_ABORT);
    REQUIRE(guard.aborts == 1);
    /* The request is not forgotten while the mode has not unwound */
    REQUIRE(s.abortTicks > 10);

    /* And it survives the mode's own exit: the reason is still the abort's */
    txg_exit(&guard, 2000, TXG_REASON_MODE);
    REQUIRE(guard.reason == TXG_REASON_TIMEOUT_FIRST);
    REQUIRE(guard.phase == TXG_PHASE_IDLE);
}

TEST_CASE("Streams that never open abort in the start budget", "[txguard]")
{
    txGuard guard;
    txg_init(&guard, 0);

    /* PTT pressed, then blocked inside audioStream_start() or radio_enableTx() */
    REQUIRE(txg_enter(&guard, 500) == true);

    Supervision s = supervise(guard, 500, 1000, true);

    REQUIRE(s.abortTime - 500 <= TXG_START_MS + TICK);
    REQUIRE(guard.reason == TXG_REASON_TIMEOUT_START);
    REQUIRE(guard.aborts == 1);
}

TEST_CASE("The tail cannot outlive the 200 ms the UI asks for", "[txguard]")
{
    /*
     * PTT released, terminator and drain on the air; the mode never comes back
     * because the DSP drain does not complete. The supervisor must have the PA
     * off inside 200 ms of the release.
     */
    txGuard guard;
    txg_init(&guard, 0);
    txg_enter(&guard, 100);
    txg_streamsOpen(&guard, 110);
    txg_firstBlock(&guard, 140);
    txg_block(&guard, 170);

    txg_finishing(&guard, 10000);
    Supervision s = supervise(guard, 10000, 1000, false);

    REQUIRE(s.abortTime != 0);
    REQUIRE(s.abortTime - 10000 <= 200);
    REQUIRE(guard.reason == TXG_REASON_TIMEOUT_DRAIN);
}

TEST_CASE("Finishing before the first block ends the over at once",
          "[txguard]")
{
    /* PTT tapped and released while the streams are opening */
    txGuard guard;
    txg_init(&guard, 0);

    txg_enter(&guard, 0);
    txg_streamsOpen(&guard, 5);
    txg_finishing(&guard, 8); /* accepted: the tail is bounded from here */
    REQUIRE(guard.phase == TXG_PHASE_DRAINING);

    Supervision s = supervise(guard, 8, 500, false);
    REQUIRE(s.abortTime - 8 <= TXG_DRAIN_MS + TICK);
    REQUIRE(guard.reason == TXG_REASON_TIMEOUT_DRAIN);

    /* From STARTING, finishing is not a state: the start deadline rules */
    txGuard other;
    txg_init(&other, 0);
    txg_enter(&other, 0);
    txg_finishing(&other, 5);
    REQUIRE(other.phase == TXG_PHASE_STARTING);
}

TEST_CASE("A transmission that never ends hits the absolute cap", "[txguard]")
{
    txGuard guard;
    txg_init(&guard, 0);

    txg_enter(&guard, 0);
    txg_streamsOpen(&guard, 10);
    txg_firstBlock(&guard, 40);

    /* The PTT stays down: no drain deadline applies, the cap does */
    Supervision s = supervise(guard, 40, TXG_MAX_TX_MS + 2000, true);

    /* The cap counts from the PTT, which came at t=0 */
    REQUIRE(s.abortTime != 0);
    REQUIRE(s.abortTime >= TXG_MAX_TX_MS);
    REQUIRE(s.abortTime <= TXG_MAX_TX_MS + 2 * TICK);
    REQUIRE(guard.reason == TXG_REASON_TIMEOUT_MAX);
}

TEST_CASE("A late block counts as a cadence slip", "[txguard]")
{
    txGuard guard;
    txg_init(&guard, 0);

    txg_enter(&guard, 0);
    txg_streamsOpen(&guard, 10);
    txg_firstBlock(&guard, 40);

    /* On time */
    txg_block(&guard, 70);
    txg_block(&guard, 100);
    REQUIRE(guard.slips == 0);

    /* 90 ms instead of 30: two slots were missed */
    txg_block(&guard, 190);
    REQUIRE(guard.slips == 1);
    REQUIRE(guard.blocks == 4);
}

TEST_CASE("The trace says where a transmission stopped", "[txguard]")
{
    txGuard guard;
    txg_init(&guard, 0);

    txg_enter(&guard, 1200);
    txg_streamsOpen(&guard, 1208);
    txg_firstBlock(&guard, 1231);
    txg_block(&guard, 1261);
    txg_finishing(&guard, 5000);
    txg_exit(&guard, 5094, TXG_REASON_PTT);

    char trace[128];
    char summary[160];
    REQUIRE(txg_formatTrace(&guard, trace, sizeof(trace)) > 0);
    REQUIRE(txg_format(&guard, summary, sizeof(summary)) > 0);

    const std::string t(trace);
    REQUIRE(t.find("0:enter") != std::string::npos);
    REQUIRE(t.find("8:open") != std::string::npos);
    REQUIRE(t.find("31:first") != std::string::npos);
    REQUIRE(t.find("94:exit") != std::string::npos);

    const std::string s(summary);
    REQUIRE(s.find("ph=idle") != std::string::npos);
    REQUIRE(s.find("rs=ptt") != std::string::npos);
    REQUIRE(s.find("fst=+31") != std::string::npos);
    REQUIRE(s.find("blk=2") != std::string::npos);
    REQUIRE(s.find("tail=94") != std::string::npos);
}

TEST_CASE("A guard opened twice keeps one transmission", "[txguard]")
{
    txGuard guard;
    txg_init(&guard, 0);

    REQUIRE(txg_enter(&guard, 0) == true);
    REQUIRE(txg_enter(&guard, 10) == false);
    REQUIRE(guard.transmissions == 1);

    txg_exit(&guard, 100, TXG_REASON_PTT);
    REQUIRE(txg_enter(&guard, 200) == true);
    REQUIRE(guard.transmissions == 2);
    /* A new over starts with a clean trace and no blocks */
    REQUIRE(guard.blocks == 0);
    REQUIRE(guard.traceCount == 1);
}
