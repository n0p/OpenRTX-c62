/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests of the *default* reaction to a stall: write the record, then ask
 * the platform to reset the radio (core/blackbox.c, bb_platformHang()).
 *
 * This is a file of its own, and that is the point.  tests/unit/blackbox.cpp
 * overrides bb_platformHang() to catch the mask without rebooting, so it never
 * runs the code that actually recovers the radio -- and that code is where the
 * radio stayed alive for a minute and a half with a dead screen after the
 * record had already been written.  Here the override is gone: the weak
 * default runs, and what is checked is the order of the two steps, that the
 * reboot is asked for whatever the flash write did, and that a hang reported
 * once does not come back every 100 ms to erase the sector again.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "core/blackbox.h"
}

/* ------------------------------------------------------------------------- */
/* Platform hooks: everything except the hang reaction                       */
/* ------------------------------------------------------------------------- */

namespace
{

std::atomic<uint32_t> fakeNow{ 0 };

std::vector<uint8_t> persisted;
uint32_t persistCalls = 0;
int persistResult = 0;
uint32_t rebootCalls = 0;
uint32_t persistAtReboot = 0; /* persist calls seen at the first reboot    */

bbSnapshot_t snapshot;

} // namespace

extern "C" uint32_t bb_platformNowMs(void)
{
    return fakeNow.load();
}

extern "C" const void *bb_platformCurrentThread(void)
{
    static thread_local uint8_t id;
    return &id;
}

extern "C" void bb_platformSnapshot(bbSnapshot_t *s)
{
    *s = snapshot;
}

extern "C" int bb_platformPersist(const uint8_t *rec, size_t len)
{
    persistCalls++;
    persisted.assign(rec, rec + len);
    return persistResult;
}

/*
 * The reboot is a counter here: in a test it must return, and returning is
 * exactly the case the radio has to survive (the SoC refusing the reset, a
 * platform with no reset at all).
 */
extern "C" void bb_platformReboot(void)
{
    if (rebootCalls == 0)
        persistAtReboot = persistCalls;
    rebootCalls++;
}

namespace
{

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void resetAll()
{
    bb_suspend();
    bb_clear();

    persisted.clear();
    persistCalls = 0;
    persistResult = 0;
    rebootCalls = 0;
    persistAtReboot = 0;
    fakeNow = 0;

    snapshot = bbSnapshot_t{};
    snapshot.rtxMode = 10; /* AM */
    snapshot.uiMode = 10;
}

} // namespace

TEST_CASE("A stall writes the record and then asks for the reset", "[bbhang]")
{
    resetAll();

    /* The AM freeze: the UI thread stops beating and nothing else does. */
    bb_beat(BB_BEAT_UI);

    fakeNow = 17264;
    bb_resume();
    REQUIRE(bb_supervise(17264) == false); /* primes */

    REQUIRE(bb_supervise(17264 + BB_STALL_MS - 1) == false);
    REQUIRE(persistCalls == 0);
    REQUIRE(rebootCalls == 0);

    /* At the threshold the default hook runs: record first, reset second. */
    REQUIRE(bb_supervise(17264 + BB_STALL_MS) == true);

    REQUIRE(persistCalls == 1);
    REQUIRE(rebootCalls == 1);
    REQUIRE(persistAtReboot == 1); /* the record was written before the reset */

    REQUIRE(persisted.size() == BB_RECORD_SIZE);
    REQUIRE(bb_recordValid(persisted.data(), persisted.size()));
    REQUIRE(persisted[BB_OFF_REASON] == BB_REASON_STALL);
    REQUIRE(rd32(&persisted[BB_OFF_DETAIL]) == (1U << BB_BEAT_UI));
    REQUIRE(rd32(&persisted[BB_OFF_UPTIME]) == 17264 + BB_STALL_MS);
}

TEST_CASE("A record that cannot be written still ends in a reset", "[bbhang]")
{
    resetAll();

    persistResult = -1; /* the sector is worn out, or the flash path wedged */

    bb_beat(BB_BEAT_RTX);

    fakeNow = 5000;
    bb_resume();
    bb_supervise(5000);

    REQUIRE(bb_supervise(5000 + BB_STALL_MS) == true);

    /* The radio has to come back even when it cannot say why it froze: that is
     * the difference between a post-mortem and a paperweight. */
    REQUIRE(persistCalls == 1);
    REQUIRE(rebootCalls == 1);
}

TEST_CASE("A hang is reported once, not rewritten every pass", "[bbhang]")
{
    resetAll();

    bb_beat(BB_BEAT_UI);

    fakeNow = 1000;
    bb_resume();
    bb_supervise(1000);

    REQUIRE(bb_supervise(1000 + BB_STALL_MS) == true);
    REQUIRE(persistCalls == 1);

    /*
     * The platform hook returned (in the radio: the SoC refused the reset).
     * The next passes must not erase and rewrite the record's flash sector:
     * ten erases a second would destroy the record of the hang with the record
     * of the rewrite.  One hang, one record; the hardware watchdog, which the
     * radio stops feeding, is the way out from here.
     */
    for (uint32_t i = 1; i <= 20; i++)
        REQUIRE(bb_supervise(1000 + BB_STALL_MS + i * 100) == false);

    REQUIRE(persistCalls == 1);
    REQUIRE(rebootCalls == 1);
}

TEST_CASE("The keypad is one of the watched threads", "[bbhang]")
{
    resetAll();

    /* Only the keypad scanner beats: the mask must name it and nothing else. */
    bb_beat(BB_BEAT_KBD);

    fakeNow = 900;
    bb_resume();
    bb_supervise(900);

    REQUIRE(bb_supervise(900 + BB_STALL_MS) == true);
    REQUIRE(persisted.size() == BB_RECORD_SIZE);
    REQUIRE(rd32(&persisted[BB_OFF_DETAIL]) == (1U << BB_BEAT_KBD));
    REQUIRE(rebootCalls == 1);
}
