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
#include "emulator/emulator.h"
#include "interfaces/radio.h"
#include "interfaces/delays.h"
#include "core/utils.h"
#include "core/audio_path.h"
#include "rtx/rtx.h"
}

/*
 * FM -> AM (airband) -> FM, the way the radio does it.
 *
 * This is the incident: from FM on a VFO frequency, selecting AM left the radio
 * with a dead keyboard and a frozen screen while the receiver kept working, the
 * protocol kept answering and the uptime kept counting.
 *
 * Two things are modelled here, because they are what the hardware showed:
 *
 *  1. The two threads that touch the audio path table at a mode change. The RTX
 *     thread opens the new mode's receiver from its squelch; the UI thread takes
 *     the speaker for the beep and the "A M" announcement and gives it back.
 *     They both used to insert into and erase from the same std::map with no
 *     lock (core/audio_path.cpp), and that is the freeze: a red-black tree
 *     rotated under a concurrent reader makes a lookup never return -- in the UI
 *     thread, which is why the keys and the screen died and nothing else did.
 *  2. A new mode that cannot start. The driver is made to refuse the receiver,
 *     the change must not leave the radio half-entered: it goes back to the mode
 *     it came from, the receiver comes back, and the log says why.
 *
 * THE RADIO'S OWN DOOR IS USED THROUGHOUT: rtx_configure() + rtx_task(), the
 * same pair the RTX thread runs, and the real OpMode_Airband and OpMode_FM.
 */

/* The stub driver's hook (platform/drivers/baseband/radio_linux.cpp) */
void radio_linux_refuseRx(uint8_t mode, bool refuse);

namespace
{

pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;

/// Ask the RTX thread for a mode and let it act once.
void selectMode(uint8_t mode)
{
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.opMode = static_cast<opmode>(mode);
    rtx_configure(&cfg);
    rtx_task();
}

/// The UI thread's share of a mode change: a beep and its announcement.
class UiNoise
{
public:
    UiNoise() : running(true), turns_(0), stall_(false)
    {
        thread_ = std::thread([this] { loop(); });
    }

    ~UiNoise()
    {
        running = false;
        if (thread_.joinable())
            thread_.join();
    }

    void stop()
    {
        running = false;
        if (thread_.joinable())
            thread_.join();
    }

    unsigned long turns() const
    {
        return turns_.load();
    }
    bool stalled() const
    {
        return stall_.load();
    }

private:
    void loop()
    {
        unsigned long last = 0;
        unsigned long same = 0;

        while (running.load()) {
            /* vp_beep()/vp_announceRadioMode(): the prompt path comes and goes */
            pathId p = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
            if (p > 0) {
                (void)audioPath_getStatus(p);
                audioPath_release(p);
            }
            /* vp_tick(): the player asks about its own path */
            (void)audioPath_getStatus(0x7fffffff);
            turns_++;

            /*
             * A reader that stops advancing is the freeze: report it and leave
             * the loop, so the test fails instead of hanging.
             */
            if (turns_.load() == last) {
                if (++same >= 20) {
                    stall_ = true;
                    return;
                }
            } else {
                same = 0;
                last = turns_.load();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::atomic<bool> running;
    std::atomic<unsigned long> turns_;
    std::atomic<bool> stall_;
    std::thread thread_;
};

} /* namespace */

TEST_CASE("FM to AM and back, with the UI thread using the audio paths",
          "[rtx][airband][mode][audio]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;
    radio_linux_refuseRx(OPMODE_AIRBAND, false);

    /* The receiver in FM on the VFO frequency of the report */
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.rxFrequency = 145500000;
    cfg.txFrequency = 145500000;
    cfg.opMode = OPMODE_FM;
    rtx_configure(&cfg);
    rtx_task();
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(radio_getStatus() == RX);

    UiNoise noise;

    /* Into AM: the change has to end with a receiver, as every change does */
    selectMode(OPMODE_AIRBAND);
    rtx_task();

    INFO("the airband mode is the one active after the change");
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_AIRBAND);
    REQUIRE(radio_getStatus() == RX);

    /* Let it run for a while, with both threads on the path table */
    for (int i = 0; i < 100; i++)
        rtx_task();
    REQUIRE(noise.stalled() == false);

    /* And back to FM, twice, the way an operator tries the mode out */
    selectMode(OPMODE_FM);
    rtx_task();
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(radio_getStatus() == RX);

    selectMode(OPMODE_AIRBAND);
    rtx_task();
    selectMode(OPMODE_FM);
    rtx_task();
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(radio_getStatus() == RX);

    noise.stop();
    INFO("the UI thread made " << noise.turns() << " turns");
    REQUIRE(noise.stalled() == false);
    REQUIRE(noise.turns() > 0);

    selectMode(OPMODE_NONE);
    rtx_terminate();
}

TEST_CASE("a mode that cannot start hands the radio back",
          "[rtx][airband][mode]")
{
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;

    /* FM, receiving */
    radio_linux_refuseRx(OPMODE_AIRBAND, false);
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.rxFrequency = 118000000;
    cfg.txFrequency = 118000000;
    cfg.opMode = OPMODE_FM;
    rtx_configure(&cfg);
    rtx_task();
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(radio_getStatus() == RX);

    /*
     * Now the chip does not come up: the airband mode can be constructed and
     * enabled, but its receiver never reports RX. The change must not leave the
     * radio there -- that is the "mode with no receiver and no way back" this
     * test exists for.
     */
    radio_linux_refuseRx(OPMODE_AIRBAND, true);

    const uint32_t start = (uint32_t)getTick();
    selectMode(OPMODE_AIRBAND);
    const uint32_t elapsed = (uint32_t)getTick() - start;

    INFO("the change took " << elapsed << " ms");
    REQUIRE(elapsed >= RTX_MODE_START_MS - 50);
    REQUIRE(elapsed < (RTX_MODE_START_MS * 4));

    /* Back on FM, and the receiver works again */
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(radio_getStatus() == RX);

    /* And when the chip comes back, AM can be entered normally */
    radio_linux_refuseRx(OPMODE_AIRBAND, false);
    selectMode(OPMODE_AIRBAND);
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_AIRBAND);
    REQUIRE(radio_getStatus() == RX);

    selectMode(OPMODE_NONE);
    rtx_terminate();
}
