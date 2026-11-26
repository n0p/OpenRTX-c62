/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>

#include "core/vfo_search.h"

/*
 * VFO carrier search on the host, with a simulated receiver.
 *
 * The machine is pure (core/vfo_search.h): the time and a sample of the
 * receiver go in, an action comes out.  What is asserted here is what the
 * operator asked for, one test per sentence of the request:
 *
 *   - the sweep starts where the receiver is and steps up or down with the
 *     step it is handed;
 *   - an analog mode stops when the squelch opens and stays open, and a
 *     carrier that flickers does not stop it;
 *   - a digital mode stops on synchronism only when there are frames behind it
 *     (a sync flag alone is a false positive), and falls back to a dwell in a
 *     mode that publishes no frame counter;
 *   - TETRA-style frequency correction: the absolute frequency the network
 *     publishes wins, the measured carrier offset is the fallback, both are
 *     bounded, and the action asks for a retune and reports the offset;
 *   - every cancellation leaves the receiver where the sweep last was, without
 *     a retune;
 *   - a full lap of the band with nothing on it ends the search and puts the
 *     receiver back where it started;
 *   - a search cannot be started outside the VFO screen, during a
 *     transmission, during a scan, twice, or from outside the band.
 *
 * The last test measures cost, because the firmware calls the machine from
 * state_task() every 5 ms.
 */

namespace
{

constexpr uint32_t TICK  = 5;         ///< The cadence of state_task(), ms
constexpr uint32_t START = 1000;      ///< Arbitrary non-zero clock origin
constexpr uint32_t LOW   = 145000000; ///< 145.000 MHz
constexpr uint32_t HIGH  = 145100000; ///< 145.100 MHz
constexpr uint32_t STEP  = 12500;     ///< 12.5 kHz

/** A simulated receiver plus the clock the machine never reads. */
struct Rig
{
    vfoSearch_t machine;
    uint32_t    now = START;

    /* What the receiver reports */
    vfoSearchSample_t sample = {};

    /* What came out */
    uint32_t tunes = 0;

    Rig()
    {
        vfoSearch_init(&machine);
        /* Short timers so a test walks the band in a handful of calls */
        vfoSearchConfig_t cfg;
        vfoSearch_defaultConfig(&cfg);
        cfg.step       = STEP;
        cfg.low        = LOW;
        cfg.high       = HIGH;
        cfg.settleMs   = 40;
        cfg.dwellMs    = 100;
        cfg.debounceMs = 20;
        cfg.syncFrames = 2;
        cfg.syncHoldMs = 100;
        cfg.syncMs     = 400;
        cfg.offsetMin  = 200;
        cfg.offsetMax  = 3000;
        cfg.reportedMax = 25000;

        REQUIRE(vfoSearch_configure(&machine, &cfg));
    }

    void digital(bool on)
    {
        machine.cfg.digital = on ? 1 : 0;
    }

    void down()
    {
        machine.cfg.direction = VFOS_DOWN;
    }

    vfoSearchAction_t start(uint32_t frequency)
    {
        vfoSearchAction_t a;
        REQUIRE(vfoSearch_start(&machine, frequency, now, &sample, &a));
        if (a.target == VFOS_TARGET_FREQUENCY)
            tunes++;
        return a;
    }

    /** Advance the clock and one sample. */
    vfoSearchAction_t tick(uint32_t ms = TICK)
    {
        now += ms;
        vfoSearchAction_t a;
        vfoSearch_step(&machine, &sample, now, &a);
        if (a.target == VFOS_TARGET_FREQUENCY)
            tunes++;
        return a;
    }

    /** Tick until the state changes or the budget runs out. */
    vfoSearchAction_t runUntilState(uint8_t state, uint32_t budgetMs = 20000)
    {
        uint32_t spent = 0;
        vfoSearchAction_t a;
        a.event  = VFOS_EV_NONE;
        a.target = VFOS_TARGET_NONE;

        while (spent < budgetMs) {
            uint32_t before = vfoSearch_state(&machine);
            a = tick();
            spent += TICK;

            if ((a.event != VFOS_EV_NONE) && (a.event != VFOS_EV_TUNED))
                break;                       /* stopped for a reason */
            if ((vfoSearch_state(&machine) != before) &&
                (vfoSearch_state(&machine) == state))
                break;
        }

        return a;
    }
};

} /* namespace */

/* ------------------------------------------------------------------ start */

TEST_CASE("A search starts on the VFO frequency and says so")
{
    Rig rig;
    vfoSearchAction_t a = rig.start(145050000);

    REQUIRE(a.event == VFOS_EV_STARTED);
    REQUIRE(a.target == VFOS_TARGET_NONE);   /* already tuned there */
    REQUIRE(vfoSearch_frequency(&rig.machine) == 145050000);
    REQUIRE(vfoSearch_state(&rig.machine) == VFOS_STATE_LISTEN);
    REQUIRE(vfoSearch_isRunning(&rig.machine));
    REQUIRE(rig.tunes == 0);
}

TEST_CASE("A search is not started twice, nor from outside the band")
{
    Rig rig;
    rig.start(145050000);

    vfoSearchAction_t a;
    REQUIRE_FALSE(vfoSearch_start(&rig.machine, 145050000, rig.now,
                                  &rig.sample, &a));

    Rig other;
    REQUIRE_FALSE(vfoSearch_start(&other.machine, LOW - STEP, other.now,
                                  &other.sample, &a));
    REQUIRE_FALSE(vfoSearch_start(&other.machine, HIGH + STEP, other.now,
                                  &other.sample, &a));
    REQUIRE_FALSE(vfoSearch_isRunning(&other.machine));
}

TEST_CASE("The long press only starts a search from the VFO screen")
{
    /* The UI hook asks this before it arms; this is the whole rule */
    REQUIRE(vfoSearch_mayStart(true, false, false));
    REQUIRE_FALSE(vfoSearch_mayStart(false, false, false)); /* channel screen */
    REQUIRE_FALSE(vfoSearch_mayStart(true, true, false));   /* PTT down      */
    REQUIRE_FALSE(vfoSearch_mayStart(true, false, true));   /* memory scan   */
    REQUIRE_FALSE(vfoSearch_mayStart(false, true, true));
}

/* ------------------------------------------------------------- analog stop */

TEST_CASE("An analog search steps up with the mode's step")
{
    Rig rig;
    rig.start(145000000);

    /* No carrier: the sweep moves on after the dwell */
    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_TUNE, 1000);
    REQUIRE(a.event == VFOS_EV_TUNED);
    REQUIRE(a.target == VFOS_TARGET_FREQUENCY);
    REQUIRE(a.frequency == 145000000 + STEP);
    REQUIRE(vfoSearch_frequency(&rig.machine) == 145000000 + STEP);

    /* And again, one step at a time */
    a = rig.runUntilState(VFOS_STATE_TUNE, 1000);
    REQUIRE(a.frequency == 145000000 + 2 * STEP);
}

TEST_CASE("An analog search walks down with the mode's step")
{
    Rig rig;
    rig.down();
    rig.start(145050000);

    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_TUNE, 1000);
    REQUIRE(a.event == VFOS_EV_TUNED);
    REQUIRE(a.frequency == 145050000 - STEP);
}

TEST_CASE("An analog search stops when the squelch opens and stays there")
{
    Rig rig;
    rig.start(145000000);

    /* Three quiet steps, then a carrier */
    for (int i = 0; i < 3; i++)
        REQUIRE(rig.runUntilState(VFOS_STATE_TUNE, 1000).event == VFOS_EV_TUNED);

    uint32_t foundAt = vfoSearch_frequency(&rig.machine);
    rig.sample.squelch = true;

    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 2000);
    REQUIRE(a.event == VFOS_EV_FOUND);
    REQUIRE(a.result == VFOS_RESULT_CARRIER);
    REQUIRE(a.target == VFOS_TARGET_NONE);      /* stays, no retune */
    REQUIRE(vfoSearch_frequency(&rig.machine) == foundAt);
    REQUIRE_FALSE(vfoSearch_isRunning(&rig.machine));

    /*
     * And it does not move again: a minute of samples with the carrier on
     * produces no event and leaves the frequency where it was.
     */
    for (int i = 0; i < 12000; i++) {
        vfoSearchAction_t n = rig.tick();
        REQUIRE(n.event == VFOS_EV_NONE);
        REQUIRE(n.target == VFOS_TARGET_NONE);
    }
    REQUIRE(vfoSearch_frequency(&rig.machine) == foundAt);
    REQUIRE(vfoSearch_result(&rig.machine) == VFOS_RESULT_CARRIER);
}

TEST_CASE("Chatter on the squelch does not stop an analog search")
{
    Rig rig;
    rig.start(145000000);

    /* Open for less than the debounce, then closed, for a whole dwell */
    for (int i = 0; i < 10; i++) {
        rig.sample.squelch = (i % 2) == 0;
        rig.tick();
    }
    REQUIRE(vfoSearch_state(&rig.machine) != VFOS_STATE_FOUND);
    REQUIRE(vfoSearch_result(&rig.machine) == VFOS_RESULT_NONE);
}

/* ------------------------------------------------------------ digital stop */

TEST_CASE("A digital search needs frames behind the sync")
{
    Rig rig;
    rig.digital(true);
    rig.sample.haveFrames = true;
    rig.sample.frames     = 10;
    rig.start(145000000);            /* ten frames were decoded before */

    /* A sync word, held: not a carrier without a frame that decodes */
    rig.sample.synced = true;
    for (int i = 0; i < 30; i++)
        rig.tick();                  /* 150 ms of unbroken sync */
    REQUIRE(vfoSearch_state(&rig.machine) == VFOS_STATE_LISTEN);
    REQUIRE(vfoSearch_result(&rig.machine) == VFOS_RESULT_NONE);

    /* Two frames with their check passing while the sync holds: a carrier */
    rig.sample.frames = 12;
    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 1000);
    REQUIRE(a.event == VFOS_EV_FOUND);
    REQUIRE(a.result == VFOS_RESULT_SYNC);
    REQUIRE(a.target == VFOS_TARGET_NONE);
    REQUIRE_FALSE(vfoSearch_isRunning(&rig.machine));
}

TEST_CASE("A digital search is not stopped by a sync flag with no frames at all")
{
    Rig rig;
    rig.digital(true);
    rig.sample.haveFrames = true;
    rig.sample.frames     = 3;
    rig.start(145000000);

    /* Sync held for the whole dwell, but nothing ever decoded: the mode
     * publishes a counter, so the time fallback does not apply */
    rig.sample.synced = true;

    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_TUNE, 2000);
    REQUIRE(a.event == VFOS_EV_TUNED);
    REQUIRE(vfoSearch_result(&rig.machine) == VFOS_RESULT_NONE);
}

TEST_CASE("A digital mode with no frame counter stops on a stable sync")
{
    Rig rig;
    rig.digital(true);
    rig.start(145000000);

    /* YSF, M17, dPMR and D-STAR publish no counter: the time criterion */
    rig.sample.synced     = true;
    rig.sample.haveFrames = false;

    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 2000);
    REQUIRE(a.event == VFOS_EV_FOUND);
    REQUIRE(a.result == VFOS_RESULT_SYNC);
}

TEST_CASE("A digital search rebases when a mode resets its counter")
{
    Rig rig;
    rig.digital(true);
    rig.start(145000000);

    rig.sample.haveFrames = true;
    rig.sample.frames     = 40;      /* carried over from the old frequency */
    rig.tick();
    rig.sample.frames = 2;           /* the mode reinitialised on the retune */
    rig.sample.synced = true;
    rig.tick();
    rig.sample.frames = 4;           /* two frames with the check passed    */

    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 1000);
    REQUIRE(a.event == VFOS_EV_FOUND);
    REQUIRE(a.result == VFOS_RESULT_SYNC);
}

/* ------------------------------------------------------ frequency correction */

TEST_CASE("TETRA's published frequency corrects the tuning")
{
    Rig rig;
    rig.digital(true);
    rig.sample.haveFrames = true;
    rig.sample.frames     = 2;
    rig.start(145012500);            /* on a carrier, 2.5 kHz high */

    rig.sample.synced     = true;
    rig.sample.frames     = 4;       /* two frames arrived while listening */
    rig.sample.reported   = true;    /* the cell says where it really is */
    rig.sample.reportedHz = 145010000;

    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 2000);
    REQUIRE(a.event == VFOS_EV_FOUND);
    REQUIRE(a.result == VFOS_RESULT_SYNC);
    REQUIRE(a.corrected);
    REQUIRE(a.target == VFOS_TARGET_FREQUENCY);
    REQUIRE(a.frequency == 145010000);
    REQUIRE(a.offsetHz == -2500);
    REQUIRE(vfoSearch_frequency(&rig.machine) == 145010000);
    REQUIRE(vfoSearch_offset(&rig.machine) == -2500);
    REQUIRE(rig.tunes == 1);         /* exactly one correcting retune */
}

TEST_CASE("A measured carrier offset corrects the tuning")
{
    Rig rig;
    rig.start(145050000);

    rig.sample.squelch     = true;
    rig.sample.offsetValid = true;
    rig.sample.offsetHz    = 350;

    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 1000);
    REQUIRE(a.event == VFOS_EV_FOUND);
    REQUIRE(a.corrected);
    REQUIRE(a.frequency == 145050000 + 350);
    REQUIRE(a.offsetHz == 350);
    REQUIRE(vfoSearch_frequency(&rig.machine) == 145050350);
}

TEST_CASE("A correction below the threshold, above it, or outside the band is ignored")
{
    SECTION("below the threshold")
    {
        Rig rig;
        rig.start(145050000);
        rig.sample.squelch     = true;
        rig.sample.offsetValid = true;
        rig.sample.offsetHz    = 50;      /* under offsetMin */

        vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 1000);
        REQUIRE(a.event == VFOS_EV_FOUND);
        REQUIRE_FALSE(a.corrected);
        REQUIRE(a.frequency == 145050000);
        REQUIRE(vfoSearch_frequency(&rig.machine) == 145050000);
    }

    SECTION("above the maximum")
    {
        Rig rig;
        rig.start(145050000);
        rig.sample.squelch     = true;
        rig.sample.offsetValid = true;
        rig.sample.offsetHz    = 5000;    /* over offsetMax: another signal */

        vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 1000);
        REQUIRE_FALSE(a.corrected);
        REQUIRE(vfoSearch_frequency(&rig.machine) == 145050000);
    }

    SECTION("published frequency too far away")
    {
        Rig rig;
        rig.start(145050000);
        rig.sample.squelch     = true;
        rig.sample.reported    = true;
        rig.sample.reportedHz  = 145050000 + 100000; /* another channel */

        vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 1000);
        REQUIRE_FALSE(a.corrected);
        REQUIRE(vfoSearch_frequency(&rig.machine) == 145050000);
    }

    SECTION("correction would leave the band")
    {
        Rig rig;
        rig.start(LOW);
        rig.sample.squelch     = true;
        rig.sample.offsetValid = true;
        rig.sample.offsetHz    = -2500;   /* below the band edge */

        vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_FOUND, 1000);
        REQUIRE_FALSE(a.corrected);
        REQUIRE(vfoSearch_frequency(&rig.machine) == LOW);
    }
}

/* ------------------------------------------------------------- cancellations */

TEST_CASE("A cancellation stops the search where it is")
{
    SECTION("while listening on the start frequency")
    {
        Rig rig;
        rig.start(145050000);

        vfoSearchAction_t a;
        REQUIRE(vfoSearch_stop(&rig.machine, rig.now, &a));
        REQUIRE(a.event == VFOS_EV_CANCELLED);
        REQUIRE(a.target == VFOS_TARGET_NONE);
        REQUIRE(a.frequency == 145050000);
        REQUIRE(vfoSearch_result(&rig.machine) == VFOS_RESULT_CANCELLED);
        REQUIRE_FALSE(vfoSearch_isRunning(&rig.machine));
        REQUIRE(rig.tunes == 0);
    }

    SECTION("while listening after a step")
    {
        Rig rig;
        rig.start(145000000);
        rig.runUntilState(VFOS_STATE_TUNE, 1000);
        uint32_t at = vfoSearch_frequency(&rig.machine);

        vfoSearchAction_t a;
        REQUIRE(vfoSearch_stop(&rig.machine, rig.now, &a));
        REQUIRE(a.target == VFOS_TARGET_NONE);
        REQUIRE(a.frequency == at);
        REQUIRE(rig.tunes == 1);
    }

    SECTION("while settling after a retune")
    {
        Rig rig;
        rig.start(145000000);
        vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_TUNE, 1000);
        REQUIRE(a.event == VFOS_EV_TUNED);
        REQUIRE(vfoSearch_state(&rig.machine) == VFOS_STATE_TUNE);

        uint32_t at = a.frequency;
        REQUIRE(vfoSearch_stop(&rig.machine, rig.now, &a));
        REQUIRE(a.frequency == at);
        REQUIRE_FALSE(vfoSearch_isRunning(&rig.machine));
    }

    SECTION("twice is refused")
    {
        Rig rig;
        rig.start(145050000);
        vfoSearchAction_t a;
        REQUIRE(vfoSearch_stop(&rig.machine, rig.now, &a));
        REQUIRE_FALSE(vfoSearch_stop(&rig.machine, rig.now, &a));
    }

    SECTION("a stopped search can be started again")
    {
        Rig rig;
        rig.start(145050000);
        vfoSearchAction_t a;
        vfoSearch_stop(&rig.machine, rig.now, &a);
        REQUIRE(rig.start(145060000).event == VFOS_EV_STARTED);
        REQUIRE(vfoSearch_frequency(&rig.machine) == 145060000);
    }
}

TEST_CASE("Nothing happens after the search has stopped")
{
    Rig rig;
    rig.start(145050000);
    vfoSearchAction_t a;
    vfoSearch_stop(&rig.machine, rig.now, &a);

    rig.sample.squelch = true;
    for (int i = 0; i < 100; i++) {
        vfoSearchAction_t n = rig.tick();
        REQUIRE(n.event == VFOS_EV_NONE);
        REQUIRE(n.target == VFOS_TARGET_NONE);
    }
    REQUIRE(vfoSearch_frequency(&rig.machine) == 145050000);
}

/* ---------------------------------------------------------------- band end */

TEST_CASE("A full lap of the band ends the search and goes back to the start")
{
    Rig rig;
    rig.start(145050000);            /* in the middle of a 100 kHz band */

    /* Walk until the machine stops by itself */
    vfoSearchAction_t a;
    uint32_t guard = 0;
    do {
        a = rig.tick();
        guard++;
        REQUIRE(guard < 20000);
    } while (vfoSearch_isRunning(&rig.machine));

    REQUIRE(a.event == VFOS_EV_BAND_END);
    REQUIRE(a.result == VFOS_RESULT_BAND_END);
    REQUIRE(a.target == VFOS_TARGET_FREQUENCY);
    REQUIRE(a.frequency == 145050000);        /* back where it began */
    REQUIRE(vfoSearch_frequency(&rig.machine) == 145050000);

    /*
     * Every frequency of the band was listened to exactly once: 100 kHz at
     * 12.5 kHz is nine of them, from the start round the wrap and up to it.
     */
    REQUIRE(vfoSearch_visited(&rig.machine) == 9);
}

TEST_CASE("The sweep wraps at the band edges")
{
    Rig rig;
    rig.start(HIGH);                 /* the top of the band, going up */
    REQUIRE(vfoSearch_state(&rig.machine) == VFOS_STATE_LISTEN);

    vfoSearchAction_t a = rig.runUntilState(VFOS_STATE_TUNE, 1000);
    REQUIRE(a.event == VFOS_EV_TUNED);
    REQUIRE(a.frequency == LOW);     /* wrapped to the bottom, still running */
    REQUIRE(vfoSearch_isRunning(&rig.machine));

    Rig down;
    down.down();
    down.start(LOW);
    a = down.runUntilState(VFOS_STATE_TUNE, 1000);
    REQUIRE(a.event == VFOS_EV_TUNED);
    REQUIRE(a.frequency == HIGH);    /* wrapped to the top, still running */
    REQUIRE(vfoSearch_isRunning(&down.machine));
}

TEST_CASE("A band of one frequency ends at once")
{
    Rig rig;
    rig.machine.cfg.low  = 145050000;
    rig.machine.cfg.high = 145050000;
    rig.start(145050000);

    vfoSearchAction_t a;
    a.event = VFOS_EV_NONE;
    for (int i = 0; (i < 100) && (a.event == VFOS_EV_NONE); i++)
        a = rig.tick();

    REQUIRE(a.event == VFOS_EV_BAND_END);
    REQUIRE_FALSE(vfoSearch_isRunning(&rig.machine));
}

/* --------------------------------------------------------- configuration */

TEST_CASE("An unusable configuration is refused")
{
    vfoSearch_t s;
    vfoSearch_init(&s);

    vfoSearchConfig_t cfg;
    vfoSearch_defaultConfig(&cfg);

    cfg.step = 0;
    REQUIRE_FALSE(vfoSearch_configure(&s, &cfg));

    vfoSearch_defaultConfig(&cfg);
    cfg.low  = 146000000;
    cfg.high = 144000000;
    REQUIRE_FALSE(vfoSearch_configure(&s, &cfg));

    vfoSearch_defaultConfig(&cfg);
    cfg.syncFrames = 0;
    REQUIRE_FALSE(vfoSearch_configure(&s, &cfg));

    vfoSearch_defaultConfig(&cfg);
    REQUIRE(vfoSearch_configure(&s, &cfg));
}

TEST_CASE("A configuration is not changed under a running search")
{
    Rig rig;
    rig.start(145050000);

    vfoSearchConfig_t cfg = rig.machine.cfg;
    cfg.step = 25000;
    REQUIRE_FALSE(vfoSearch_configure(&rig.machine, &cfg));
    REQUIRE(rig.machine.cfg.step == STEP);
}

/* -------------------------------------------------------------------- cost */

TEST_CASE("The cost of a sample on this host")
{
    Rig rig;
    rig.start(145050000);

    /* Listening is the common case: the dwell timer and one comparison */
    constexpr unsigned SAMPLES = 500000;
    auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < SAMPLES; i++)
        rig.tick();
    auto end = std::chrono::steady_clock::now();

    double nsPerCall = std::chrono::duration<double, std::nano>(end - begin)
                       .count() / SAMPLES;

    std::printf("\nvfo_search: %.1f ns per 5 ms sample (host), %u tunes\n",
                nsPerCall, rig.tunes);

    REQUIRE(nsPerCall < 20000.0);
}
