/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "core/ui_watchdog.h"
#include "rtx/TxWatch.h"

/*
 * Two watchdog rules live here, because they are the same kind of rule and they
 * failed the same way -- written, correct, and in a place no test could reach:
 *
 *  - the transmit clock below (rtx/TxWatch.h): 90 s of air and the supervisor
 *    unkeys, whatever the mode believes;
 *  - the application watchdog (core/ui_watchdog.h): the main thread feeds the
 *    hardware watchdog only while the UI thread's beat is moving, so a UI that
 *    is parked turns into a reset instead of a radio that answers the cable and
 *    does nothing else.
 *
 * The cases for the transmit clock are the ones that matter for it:
 *
 *  - 89 s are still transmitting, 90 s are not;
 *  - the count is of the *air*: a chain of unattended messages, a mode that
 *    re-keys straight after its over and a change of mode all continue it;
 *  - only a release -- the PA off the air for TXW_RELEASE_MS -- puts it back to
 *    zero, and none of the state updates in between do;
 *  - once it has cut, keying is refused until that release happens.
 */

namespace
{

/// Drive the clock the way the supervisor does: one sample per tick.
void run(struct txWatch *w, uint32_t from, uint32_t to, bool keyed,
         uint32_t step = 5)
{
    for (uint32_t now = from; now <= to; now += step)
        txw_sample(w, now, keyed);
}

} /* namespace */

TEST_CASE("89 seconds of air keep the transmitter on", "[txwatch]")
{
    struct txWatch w;
    txw_init(&w, 0, 0);

    txw_sample(&w, 0, false);
    txw_sample(&w, 5, true);        /* keyed at 5 ms */
    run(&w, 10, 89000, true);

    REQUIRE(txw_airMs(&w, 89000) == 88995);
    REQUIRE(txw_tick(&w, 89000) == TXW_ACTION_NONE);
    REQUIRE(txw_keyAllowed(&w, 89000));
    REQUIRE(!txw_blocked(&w));
}

TEST_CASE("90.1 seconds of air cut the transmitter", "[txwatch]")
{
    struct txWatch w;
    txw_init(&w, 0, 0);

    txw_sample(&w, 5, true);
    run(&w, 10, 90000, true);
    REQUIRE(txw_tick(&w, 90000) == TXW_ACTION_NONE);   /* exactly on the bar */

    run(&w, 90005, 90100, true);
    REQUIRE(txw_tick(&w, 90100) == TXW_ACTION_CUT);
    REQUIRE(txw_blocked(&w));
    REQUIRE(!txw_keyAllowed(&w, 90100));
    REQUIRE(w.cuts == 1);

    /* The cut is latched: it does not repeat on the next tick */
    REQUIRE(txw_tick(&w, 90105) == TXW_ACTION_NONE);
    REQUIRE(w.cuts == 1);
}

TEST_CASE("a release at 60 s starts the count again", "[txwatch]")
{
    struct txWatch w;
    txw_init(&w, 0, 0);

    /* 60 s keyed */
    txw_sample(&w, 5, true);
    run(&w, 10, 60000, true);
    REQUIRE(txw_airMs(&w, 60000) == 59995);

    /* Released for a second and a half: that is a release */
    run(&w, 60005, 61500, false);
    REQUIRE(w.rekeys == 1);
    REQUIRE(txw_airMs(&w, 61500) == 0);

    /* Keyed again: the clock starts from zero, so 30 s later it is only 30 s */
    txw_sample(&w, 61505, true);
    run(&w, 61510, 91500, true);
    REQUIRE(txw_tick(&w, 91500) == TXW_ACTION_NONE);
    REQUIRE(txw_airMs(&w, 91500) == 29995);
    REQUIRE(!txw_blocked(&w));
}

TEST_CASE("a short gap does not put the clock back to zero", "[txwatch]")
{
    struct txWatch w;
    txw_init(&w, 0, 0);

    txw_sample(&w, 5, true);
    run(&w, 10, 40000, true);

    /* 300 ms off the air, then keyed again: a re-key, not a release */
    run(&w, 40005, 40300, false);
    txw_sample(&w, 40305, true);
    run(&w, 40310, 40330, true);

    REQUIRE(w.rekeys == 0);
    REQUIRE(txw_airMs(&w, 40330) == 40025);
    REQUIRE(txw_tick(&w, 40330) == TXW_ACTION_NONE);
}

TEST_CASE("a change of mode is not a release", "[txwatch]")
{
    struct txWatch w;
    txw_init(&w, 0, 0);

    /* 30 s of air, a mode change (the PA off for 150 ms), keyed again */
    txw_sample(&w, 5, true);
    run(&w, 10, 30000, true);
    run(&w, 30005, 30150, false);
    txw_sample(&w, 30155, true);
    run(&w, 30160, 60000, true);

    REQUIRE(w.rekeys == 0);
    /* 30 s before the change plus the 30 s after it, minus the 150 ms gap */
    REQUIRE(txw_airMs(&w, 60000) == 59845);
    REQUIRE(txw_keyAllowed(&w, 60000));
}

TEST_CASE("a chain of unattended sends cuts at 90 s of air", "[txwatch]")
{
    struct txWatch w;
    txw_init(&w, 0, 0);

    /*
     * The DMR message path keys with no PTT and may have another message
     * queued: two seconds on the air, 30 ms off, again. The count is of the
     * air, not of the message, so the chain cannot outlive the bar.
     */
    uint32_t now = 5;
    uint32_t sends = 0;
    bool fired = false;

    while ((now < 200000) && (!fired))
    {
        txw_sample(&w, now, true);           /* this message keys */
        const uint32_t end = now + 2000;
        for (uint32_t t = now + 5; t <= end; t += 5)
        {
            txw_sample(&w, t, true);
            if (txw_tick(&w, t) == TXW_ACTION_CUT)
            {
                fired = true;
                now = t;
                break;
            }
        }

        if (fired)
            break;

        for (uint32_t t = end + 5; t <= end + 30; t += 5)
            txw_sample(&w, t, false);

        now = end + 35;
        sends++;
    }

    REQUIRE(fired);
    REQUIRE(w.keyed);                 /* the cut happened with the PA keyed */
    /* 45 sends of 2 s of air (and 30 ms gaps) put it at the bar at ~91.5 s */
    REQUIRE(now < 92000);
    REQUIRE(w.cuts == 1);
    REQUIRE(w.rekeys == 0);           /* not one of those gaps was a release */
    REQUIRE(sends + 1 == 45);
}

TEST_CASE("after a cut, keying is refused until the radio is released",
          "[txwatch]")
{
    struct txWatch w;
    txw_init(&w, 0, 0);

    txw_sample(&w, 5, true);
    run(&w, 10, 90100, true);
    REQUIRE(txw_tick(&w, 90100) == TXW_ACTION_CUT);
    REQUIRE(!txw_keyAllowed(&w, 90100));

    /* The supervisor unkeys; the operator keeps pressing for a while */
    run(&w, 90105, 90600, false);
    txw_sample(&w, 90605, true);
    REQUIRE(!txw_keyAllowed(&w, 90605));
    REQUIRE(txw_tick(&w, 90605) == TXW_ACTION_NONE);   /* already latched */

    /* Pressing again inside the release window is still the same air */
    run(&w, 90610, 91000, true);
    REQUIRE(!txw_keyAllowed(&w, 91000));

    /* A real release: a second off the air, then the bar is available again */
    run(&w, 91005, 92100, false);
    REQUIRE(!txw_blocked(&w));            /* the release completed at 91.9 s */
    REQUIRE(txw_airMs(&w, 92100) == 0);
    txw_sample(&w, 92105, true);
    REQUIRE(txw_keyAllowed(&w, 92105));
    REQUIRE(w.rekeys == 1);
}

TEST_CASE("a driver barrier with the margin refuses at its own limit",
          "[txwatch]")
{
    struct txWatch w;
    txw_init(&w, 0, TXW_DRIVER_LIMIT_MS);

    REQUIRE(TXW_DRIVER_LIMIT_MS == TXW_LIMIT_MS + TXW_MARGIN_MS);

    txw_sample(&w, 5, true);
    run(&w, 10, 90000, true);
    /* Past the supervisor's bar and still inside the driver's */
    REQUIRE(txw_tick(&w, 90000) == TXW_ACTION_NONE);
    REQUIRE(txw_keyAllowed(&w, 90000));
    REQUIRE(txw_airSeconds(&w, 90000) == 89);

    run(&w, 90005, 95005, true);
    REQUIRE(txw_tick(&w, 95005) == TXW_ACTION_CUT);
    REQUIRE(!txw_keyAllowed(&w, 95005));
}

/*
 * ---------------------------------------------------------------------------
 * The application watchdog: fed only while the UI thread advances
 * ---------------------------------------------------------------------------
 */

TEST_CASE("the watchdog is fed while the UI thread advances", "[watchdog][ui]")
{
    struct uiWatchdog w;
    uiw_init(&w, 0, 0);

    /* The UI thread turns at 40 Hz; the main loop feeds at 200 Hz. */
    for (uint32_t now = 5; now <= 2000; now += 5)
        REQUIRE(uiw_service(&w, now, now / 25));

    REQUIRE(!uiw_stalled(&w));
    REQUIRE(w.stalls == 0);
}

TEST_CASE("a UI thread that stops advancing stops the feed", "[watchdog][ui]")
{
    struct uiWatchdog w;
    uiw_init(&w, 0, 0);

    /* It was alive and stopped at 1 s: the beat stays where it was. */
    for (uint32_t now = 5; now <= 1000; now += 5)
        REQUIRE(uiw_service(&w, now, now / 25));

    const uint32_t frozen = 1000 / 25;

    /* Under three seconds without a new beat is a slow pass, not a hang: the
     * feed goes on, which is what keeps a heavy screen from resetting the
     * radio. */
    for (uint32_t now = 1005; now <= 1000 + UIW_STALL_MS; now += 5)
        REQUIRE(uiw_service(&w, now, frozen));

    REQUIRE(!uiw_stalled(&w));

    /* One tick past the bar: the feed stops, and it stays stopped while the
     * UI does not come back. The watchdog is now the only thing left that can
     * bring the radio back. */
    REQUIRE(!uiw_service(&w, 1000 + UIW_STALL_MS + 5, frozen));
    REQUIRE(uiw_stalled(&w));
    REQUIRE(w.stalls == 1);

    for (uint32_t now = 1010 + UIW_STALL_MS; now <= 20000; now += 5)
        REQUIRE(!uiw_service(&w, now, frozen));

    REQUIRE(w.stalls == 1); /* one stall, not one per pass */
}

TEST_CASE("a UI thread that comes back is fed again", "[watchdog][ui]")
{
    struct uiWatchdog w;
    uiw_init(&w, 0, 0);

    uiw_service(&w, 5, 0);
    REQUIRE(!uiw_service(&w, 5000, 0));
    REQUIRE(uiw_stalled(&w));

    /* The pass that was very slow ends: the beat moves and the feed returns
     * before the 2 s hardware window closes.  A reset for a slow pass would be
     * a bug of its own. */
    REQUIRE(uiw_service(&w, 5010, 1));
    REQUIRE(!uiw_stalled(&w));
    REQUIRE(w.stalls == 1);

    for (uint32_t now = 5015; now <= 8000; now += 5)
        REQUIRE(uiw_service(&w, now, 1 + now / 25));
}

TEST_CASE("the stall deadline is three seconds, and it is the one in the loop",
          "[watchdog][ui]")
{
    /* Two seconds of a busy screen: fed.  The number is the promise the mode
     * switch and the display deadline were sized against. */
    REQUIRE(UIW_STALL_MS == 3000);

    struct uiWatchdog w;
    uiw_init(&w, 0, 0);
    REQUIRE(uiw_service(&w, 2999, 0));
    REQUIRE(!uiw_service(&w, 3001, 0));
}

TEST_CASE("two instances of the watchdog rule do not share state",
          "[watchdog][ui]")
{
    /*
     * The wiring itself (that the C62 main loop calls this and only this) is
     * checked in the source by tests/unit/platform_wiring.cpp.  What can be
     * said here is that the rule holds no static state of its own: an instance
     * fed with one thread's beat knows nothing about another's, so a caller
     * cannot accidentally inherit a feed it did not earn.
     */
    struct uiWatchdog a, b;
    uiw_init(&a, 0, 0);
    uiw_init(&b, 0, 0);

    REQUIRE(uiw_service(&a, 5, 1));   /* a sees the UI advance     */
    REQUIRE(uiw_service(&b, 5, 0));   /* b does not, but it is new */
    REQUIRE(!uiw_service(&b, 5000, 0));
    REQUIRE(uiw_stalled(&b));
    REQUIRE(!uiw_stalled(&a));
    REQUIRE(a.stalls == 0);
}
