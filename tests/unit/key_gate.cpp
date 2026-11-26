/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "rtx/KeyGate.h"

/*
 * The keying rule, walked edge by edge.
 *
 * This is the whole answer to "a change of mode must not key the PA": the
 * modes ask keyGate_pttKey() instead of reading the PTT level, so what a change
 * of mode does to the gate is what the radio does. The cases below are the ones
 * that were wrong on the radio, in the order they happened:
 *
 *  - the PTT reading pressed across the change (held, stale, a pin sampled
 *    while the chip was being reconfigured) must not key;
 *  - a press during the change must not key even if it is a real edge;
 *  - after the change the PTT has to be released once: the level that was
 *    there before is not the operator asking to transmit;
 *  - a press has to last long enough (one 30 ms tick) to be the operator's;
 *  - an unattended transmission waits for the change to settle and never runs
 *    with the PTT down during it.
 */

TEST_CASE("a PTT read as pressed across a mode change does not key", "[keygate]")
{
    struct keyGate g;
    keyGate_init(&g, 0);

    /* The operator is holding the PTT while the mode is changed */
    keyGate_beginTransition(&g, 1000);
    keyGate_endTransition(&g, 1050);

    /* Every tick from the change onwards, the PTT reads pressed */
    for (uint32_t now = 1050; now < 3000; now += 30)
    {
        keyGate_sample(&g, now, true);
        REQUIRE_FALSE(keyGate_pttKey(&g, now));
    }
}

TEST_CASE("the PTT has to be released once after the change", "[keygate]")
{
    struct keyGate g;
    keyGate_init(&g, 0);
    keyGate_beginTransition(&g, 100);
    keyGate_endTransition(&g, 120);

    /* Held from before: no release seen, so even a long press cannot key */
    for (uint32_t now = 120; now < 1200; now += 30)
    {
        keyGate_sample(&g, now, true);
        REQUIRE_FALSE(keyGate_pttKey(&g, now));
    }

    /* Release, then press: that is the operator, and it keys */
    keyGate_sample(&g, 1200, false);
    keyGate_sample(&g, 1230, true);
    REQUIRE_FALSE(keyGate_pttKey(&g, 1230)); /* not held long enough yet */
    REQUIRE(keyGate_pttKey(&g, 1260));
}

TEST_CASE("a press is not a key until it has lasted", "[keygate]")
{
    struct keyGate g;
    keyGate_init(&g, 0);
    keyGate_sample(&g, 0, false); /* released at boot */
    keyGate_beginTransition(&g, 10);
    keyGate_endTransition(&g, 20);
    keyGate_sample(&g, 200, false); /* settled, release seen */
    REQUIRE_FALSE(keyGate_pttKey(&g, 200));

    keyGate_sample(&g, 230, true);
    /* A single-sample glitch: the press is gone before the hold elapsed */
    keyGate_sample(&g, 240, false);
    keyGate_sample(&g, 300, true);
    REQUIRE_FALSE(keyGate_pttKey(&g, 300));

    /* Held for the hold time: keys */
    REQUIRE(keyGate_pttKey(&g, 330));

    /* And stays keyed while it is held */
    keyGate_sample(&g, 5000, true);
    REQUIRE(keyGate_pttKey(&g, 5000));

    /* Released: not keyed any more */
    keyGate_sample(&g, 5030, false);
    REQUIRE_FALSE(keyGate_pttKey(&g, 5030));
}

TEST_CASE("nothing keys while the change is in progress", "[keygate]")
{
    struct keyGate g;
    keyGate_init(&g, 0);
    keyGate_sample(&g, 0, false);
    keyGate_beginTransition(&g, 100);

    /* A real edge, held for a second, inside the transition */
    for (uint32_t now = 100; now < 1100; now += 30)
    {
        keyGate_sample(&g, now, true);
        REQUIRE_FALSE(keyGate_pttKey(&g, now));
        REQUIRE_FALSE(keyGate_unattended(&g, now));
    }
}

TEST_CASE("an unattended transmission waits for the change to settle",
          "[keygate]")
{
    struct keyGate g;
    keyGate_init(&g, 0);
    keyGate_sample(&g, 0, false);
    keyGate_beginTransition(&g, 1000);
    keyGate_endTransition(&g, 1100);

    /* The guard is 150 ms and the settle another 250: nothing before 1500 */
    REQUIRE_FALSE(keyGate_unattended(&g, 1300));
    REQUIRE_FALSE(keyGate_unattended(&g, 1490));
    REQUIRE(keyGate_unattended(&g, 1500));

    /* With the PTT down it never runs: the operator owns the transmitter */
    keyGate_sample(&g, 1600, true);
    REQUIRE_FALSE(keyGate_unattended(&g, 1600));

    /* And it comes back as soon as the PTT is released */
    keyGate_sample(&g, 1630, false);
    REQUIRE(keyGate_unattended(&g, 1630));
}

TEST_CASE("the guard survives a change that never ends", "[keygate]")
{
    struct keyGate g;
    keyGate_init(&g, 0);
    keyGate_sample(&g, 0, false);
    keyGate_beginTransition(&g, 50);

    for (uint32_t now = 50; now < 20000; now += 30)
    {
        keyGate_sample(&g, now, true);
        REQUIRE_FALSE(keyGate_pttKey(&g, now));
    }

    /* Only ending it lets a press key, and only after the guard */
    keyGate_endTransition(&g, 20000);
    keyGate_sample(&g, 20200, false);
    keyGate_sample(&g, 20230, true);
    REQUIRE_FALSE(keyGate_pttKey(&g, 20230)); /* guard + hold not done */
    REQUIRE(keyGate_pttKey(&g, 20290));
}

TEST_CASE("keying with the PTT up needs a grant from the unattended path",
          "[keygate]")
{
    struct keyGate g;
    keyGate_init(&g, 0);
    keyGate_sample(&g, 0, false);
    keyGate_beginTransition(&g, 100);
    keyGate_endTransition(&g, 120);

    /* Settled, but nobody asked to transmit unattended: the choke point refuses */
    REQUIRE(keyGate_unattended(&g, 1000));
    REQUIRE_FALSE(keyGate_unattendedGranted(&g, 1000));

    /* The queued message asks, and keys within the grant */
    keyGate_grantUnattended(&g, 1000);
    REQUIRE(keyGate_unattendedGranted(&g, 1000));
    REQUIRE(keyGate_unattendedGranted(&g, 1900));

    /* The grant is short: a stale one cannot key a later mode */
    REQUIRE_FALSE(keyGate_unattendedGranted(&g, 2100));
}
