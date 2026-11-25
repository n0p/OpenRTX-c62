/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

extern "C" {
#include "core/audio_path.h"
}

/*
 * The audio path table (core/audio_path.cpp).
 *
 * Two parts, in the order they matter:
 *
 *  1. the semantics: a lower-priority path is suspended by a higher one and
 *     resumed when that one goes, and releasing an ID twice is a no-op;
 *  2. the property that a change of mode depends on: the table is used from
 *     two threads at once -- the RTX thread opens and closes the receiver's
 *     path from the squelch, the UI thread opens the prompt path for a beep or
 *     a voice announcement -- and it has to survive that. Before the table had
 *     a lock, the same loop this test runs corrupted the std::map behind it:
 *     six runs out of six ended in a SIGSEGV or in a std::out_of_range from
 *     routes.at() inside audioPath_release(), and on the radio either one is a
 *     dead or a rebooting receiver.
 *
 * The concurrency part has a deadline of its own: a corrupted tree does not
 * have to crash, it can also make a traversal never return, which is the
 * freeze measured on hardware (keyboard and screen dead, receiver working).
 * The watchdog below is that symptom turned into a test failure.
 */

namespace
{

constexpr int ROUNDS = 20000;

/// Raise the ticket when either thread stops making progress.
std::atomic<unsigned long> tickets{ 0 };

/// Set by the watchdog when the two threads stop making progress.
std::atomic<bool> stalled{ false };
/// Cleared when the test is over, so the watchdog can stop watching.
std::atomic<bool> watching{ true };

} /* namespace */

TEST_CASE("a higher priority path suspends the receiver and resumes it",
          "[audio][path]")
{
    /* The receiver's path, as every mode opens it */
    pathId rx = audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX);
    REQUIRE(rx > 0);
    REQUIRE(audioPath_getStatus(rx) == PATH_OPEN);

    /* A voice prompt takes the speaker: the RX path is suspended, not closed */
    pathId prompt = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
    REQUIRE(prompt > 0);
    REQUIRE(audioPath_getStatus(prompt) == PATH_OPEN);
    REQUIRE(audioPath_getStatus(rx) == PATH_SUSPENDED);

    /* A second receiver path cannot take the speaker from the prompt */
    pathId second = audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX);
    REQUIRE(second == -1);

    /* The prompt ends: the receiver comes back without asking for it again */
    audioPath_release(prompt);
    REQUIRE(audioPath_getStatus(prompt) == PATH_CLOSED);
    REQUIRE(audioPath_getStatus(rx) == PATH_OPEN);

    audioPath_release(rx);
    REQUIRE(audioPath_getStatus(rx) == PATH_CLOSED);

    /* Releasing nothing, or the same path twice, changes nothing */
    audioPath_release(rx);
    audioPath_release(-1);
    audioPath_release(0x7fffffff);
    REQUIRE(audioPath_getStatus(rx) == PATH_CLOSED);
}

TEST_CASE("the path table survives the UI thread and the RTX thread at once",
          "[audio][path][mode]")
{
    tickets = 0;
    stalled = false;
    watching = true;

    /* The receiver, open: the mode change happens with audio on the speaker */
    pathId rx = audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX);
    REQUIRE(rx > 0);

    /*
     * The RTX thread's side of a mode change: the squelch closes and opens the
     * receiver path, over and over, as it does on a carrier that fades.
     */
    std::thread rtx([] {
        for (int i = 0; i < ROUNDS; i++) {
            pathId p = audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX);
            if (p > 0)
                audioPath_release(p);
            tickets++;
        }
    });

    /*
     * The UI thread's side: the beep and the voice announcement of the new mode
     * take the speaker and give it back, and the player asks for the status of
     * its path in between (vp_tick).
     */
    std::thread ui([] {
        for (int i = 0; i < ROUNDS; i++) {
            pathId p = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
            if (p > 0) {
                (void)audioPath_getStatus(p);
                (void)audioPath_getInfo(p);
                audioPath_release(p);
            }
            (void)audioPath_getStatus(0x7fffffff);
            tickets++;
        }
    });

    /*
     * A watchdog thread: it fails the test if neither of the two moved for a
     * second, which is what a lookup over a corrupted tree looks like.
     */
    std::thread watch([] {
        unsigned long last = tickets.load();
        for (int i = 0; (i < 300) && watching.load() && (!stalled.load());
             i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            unsigned long now = tickets.load();
            if (now == last) {
                stalled = true;
                return;
            }
            last = now;
        }
    });

    rtx.join();
    ui.join();
    watching = false;
    watch.join();

    INFO("both threads have to keep moving: a stalled one is the freeze");
    REQUIRE(stalled.load() == false);
    REQUIRE(tickets.load() == 2UL * ROUNDS);

    /* The table is consistent afterwards: the receiver path still there */
    REQUIRE(audioPath_getStatus(rx) == PATH_OPEN);
    audioPath_release(rx);
    REQUIRE(audioPath_getStatus(rx) == PATH_CLOSED);
}
