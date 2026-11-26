/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * P2 held on the FM main screen opens the APRS screens, whatever the popup did
 * in between.
 *
 * The keypad does not report "a long press": it reports the press when it
 * happens and, 700 ms later, the same key again with the long-press flag. By
 * then the quick menu is already open -- it opens on the press edge so that it
 * feels instant -- and the popup consumes every key while it is up. So P2 held
 * on the FM main screen only latched the popup, and the APRS station list was
 * unreachable from the keypad (docs/UI.md says P2 long is APRS; the operator
 * reported "the APRS screens never open").
 *
 * The test presses the two events in that order on purpose: a single event with
 * long_press set is what a test harness would do naturally and it is exactly
 * what hid the defect. It passed before the fix only because it skipped the
 * press edge.
 */

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "core/event.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "ui/ui_default.h"
#include "ui/ui_aprs.h"
#include "ui/ui_quickmenu.h"
}

extern state_t state;

namespace
{

constexpr freq_t FREQ = 145500000;

void pressKey(uint32_t keys, bool isLong = false)
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

/** A clean radio, in FM, on the main screen, popup closed. */
void resetUi(uint8_t mode = OPMODE_FM, freq_t freq = FREQ)
{
    qm_close();
    qm_page_close();
    macro_latched = false;
    ui_setInputLock(false);

    state.channel.mode          = mode;
    state.channel.rx_frequency  = freq;
    state.channel.tx_frequency  = freq;
    state.channel.bandwidth     = BW_25;
    state.channel.power         = 1000;
    state.channel.rx_only       = false;
    state.ui_screen             = MAIN_VFO;
    state.tuner_mode            = VFO;
    state.txDisable             = false;
    state.charge                = 100;
    state.emergency             = false;
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;
    state.settings.macroMenuLatch = 0;
    ui_rtxSyncRequest           = false;
}

} /* namespace */

TEST_CASE("P2 held on the FM main screen reaches the APRS station list",
          "[ui][aprs]")
{
    resetUi();

    /* The press edge: the popup opens here, as it does on the radio. */
    pressKey(KEY_F2);
    REQUIRE(qm_state.open);

    /* 700 ms later the keypad reports the long press of the same key. */
    pressKey(KEY_F2, true);

    REQUIRE_FALSE(qm_state.open);
    REQUIRE(state.ui_screen == MENU_APRS);
    REQUIRE(ui_aprs_isScreen(state.ui_screen));
    REQUIRE(state.txDisable);
}

TEST_CASE("P2 held with the popup already open still reaches APRS",
          "[ui][aprs]")
{
    resetUi();

    /* A popup opened earlier (P2 short, then released) */
    pressKey(KEY_F2);
    pressKey(0);
    REQUIRE(qm_state.open);

    pressKey(KEY_F2, true);

    REQUIRE_FALSE(qm_state.open);
    REQUIRE(state.ui_screen == MENU_APRS);
}

TEST_CASE("P2 held in another mode keeps the popup and does not open APRS",
          "[ui][aprs]")
{
    /* M17: the long press of P2 is the macro grid, and APRS is FM only */
    resetUi(OPMODE_M17);

    pressKey(KEY_F2);
    REQUIRE(qm_state.open);
    pressKey(KEY_F2, true);

    REQUIRE(state.ui_screen != MENU_APRS);
}

TEST_CASE("esc leaves the APRS screens and gives the main screen back",
          "[ui][aprs]")
{
    resetUi();

    pressKey(KEY_F2);
    pressKey(KEY_F2, true);
    REQUIRE(state.ui_screen == MENU_APRS);

    /* Red on the station list goes back to the main screen */
    pressKey(KEY_ESC);

    REQUIRE_FALSE(ui_aprs_isScreen(state.ui_screen));
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(state.txDisable);
}
