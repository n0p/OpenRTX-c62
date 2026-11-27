/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The VFO carrier search as the keypad sees it, driven through the real UI FSM.
 *
 * What is asserted here is the part of the feature that lives in the
 * interface and cannot be reached from core/vfo_search.h:
 *
 *   - a long press of an arrow on the VFO screen is not a tuning step: it arms
 *     the search and the frequency does not move;
 *   - the sweep starts when the arrow has been held for three seconds, and
 *     letting go before that starts nothing;
 *   - on the channel screen, and anywhere that is not the VFO, a long press
 *     starts nothing at all;
 *   - a short key -- the red one, any other -- stops a running sweep and leaves
 *     the receiver where the sweep was.
 *
 * The three second hold is real time (getTick() is the host clock), so the two
 * tests that need it sleep for it.  Everything else about the machine is in
 * vfo_search_test, this only checks the wiring.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

extern "C" {
#include "core/state.h"
#include "core/input.h"
#include "core/event.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "core/vfo_search.h"
#include "core/vfo_search_service.h"
#include "ui/ui_tools.h"
#include "interfaces/cps_io.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
}

extern state_t state;

namespace
{

constexpr freq_t FREQ = 145500000;   /* 2 m: inside a band the search knows */
constexpr uint32_t HOLD_MS = 3000;   /* the operator's three seconds        */

void press(uint32_t keys, bool isLong)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = isLong ? 1 : 0;

    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

/** One pass of the UI with no key event: the periodic part only. */
void tick(void)
{
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

/** Hold: wait, then let the UI tick, the way the 40 Hz loop does. */
void hold(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    tick();
}

void resetUi(uint8_t screen, uint8_t mode, freq_t freq)
{
    /*
     * The menu is a state of its own and owns the keypad while it is open: a
     * case that presses P1 (any of them may, the order is random) would leave
     * it up and the arrows of the next case would move its cursor instead of
     * the frequency.
     */
    ui_tools_close();

    state.channel.mode          = mode;
    state.channel.rx_frequency  = freq;
    state.channel.tx_frequency  = freq;
    state.channel.bandwidth     = BW_25;
    state.channel.power         = 1000;
    state.channel.rx_only       = false;
    state.ui_screen             = screen;
    state.tuner_mode            = VFO;
    state.txDisable             = false;
    state.unattendedTxDisable = false;
    state.charge                = 100;
    state.emergency             = false;
    state.step_index            = 4;            /* 12.5 kHz */
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;       /* silent in the harness */
}

} /* namespace */

TEST_CASE("A long press of an arrow on the VFO screen arms and does not tune")
{
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);

    /* A short press is still a tuning step */
    press(KEY_UP, false);
    REQUIRE(state.channel.rx_frequency == FREQ + 12500);
    REQUIRE_FALSE(vfoSearchService_active());

    /* A long one is not: it arms the search and leaves the frequency alone */
    press(KEY_UP, true);
    REQUIRE(state.channel.rx_frequency == FREQ + 12500);
    REQUIRE_FALSE(vfoSearchService_active());

    /* Letting go cancels the arming, and the sweep never starts */
    press(0, false);
    hold(HOLD_MS + 200);
    REQUIRE_FALSE(vfoSearchService_active());
    REQUIRE(state.channel.rx_frequency == FREQ + 12500);
}

TEST_CASE("Holding an arrow for three seconds starts the sweep")
{
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);

    press(KEY_DOWN, true);       /* armed: the frequency must not have moved */
    REQUIRE(state.channel.rx_frequency == FREQ);
    REQUIRE_FALSE(vfoSearchService_active());

    hold(HOLD_MS + 200);

    REQUIRE(vfoSearchService_active());

    struct vfoSearchStatus st;
    REQUIRE(vfoSearchService_status(&st));
    REQUIRE(st.active);
    REQUIRE(st.direction == VFOS_DOWN);
    REQUIRE(st.frequency == FREQ);

    /*
     * Any short key stops it, and the receiver stays on the frequency the
     * sweep was listening to.
     */
    press(KEY_ESC, false);
    REQUIRE_FALSE(vfoSearchService_active());
    REQUIRE(vfoSearchService_status(&st));
    REQUIRE(st.result == VFOS_RESULT_CANCELLED);
    REQUIRE(st.frequency == FREQ);
    REQUIRE(state.channel.rx_frequency == FREQ);
}

TEST_CASE("On the channel screen a long press starts nothing")
{
    /*
     * An arrow on the channel screen loads the neighbouring channel, which
     * reads the codeplug: give the driver an empty one so the harness can
     * drive that path at all.
     */
    char cps_name[] = "ui_vfo_search_test.rtxc";
    REQUIRE(cps_create(cps_name) == 0);
    REQUIRE(cps_open(cps_name) == 0);

    resetUi(MAIN_MEM, OPMODE_FM, FREQ);

    press(KEY_UP, true);
    hold(HOLD_MS + 200);

    REQUIRE_FALSE(vfoSearchService_active());

    resetUi(MAIN_VFO_INPUT, OPMODE_FM, FREQ);
    press(KEY_UP, true);
    hold(HOLD_MS + 200);

    REQUIRE_FALSE(vfoSearchService_active());
}

TEST_CASE("Any other key cancels the arming")
{
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    REQUIRE(vfoSearch_mayStart(true, false, false));

    press(KEY_UP, true);

    /* A key event whose arrows are not the armed one disarms: no sweep */
    press(KEY_F1, false);
    hold(HOLD_MS + 200);

    REQUIRE_FALSE(vfoSearchService_active());
}
