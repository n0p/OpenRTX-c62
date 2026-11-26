/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Settings -> Identity, driven through the real UI FSM: the callsign and the
 * DMR ID are the two halves of who the radio is on the air, and until this page
 * existed only the PC protocol could set the second one.
 *
 * What is checked, in the order the operator does it:
 *
 *   - the row is reachable from the main screen with the keys and nothing else
 *     (a page that is compiled but not dispatched is a page nobody can open);
 *   - the callsign entry takes the callsign alphabet, and a confirm writes
 *     state.settings.callsign -- the one field the M17 source address, the APRS
 *     base address and the splash screen read -- and asks for an RTX resync;
 *   - the DMR ID entry takes digits and writes state.settings.dmr_id;
 *   - a value the field cannot hold (0, more than 24 bits, an empty or
 *     malformed callsign) is refused: the settings keep the old value, the
 *     screen keeps the reason and the edit stays open so the operator can fix
 *     it instead of starting over;
 *   - both values survive the settings NVM, which is the same record a `settings
 *     set` over the PC protocol writes (tests/unit/pc_proto.cpp, tools/cps) and
 *     the same one state_task() stores three seconds after the change.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>

extern "C" {
#include "core/event.h"
#include "core/input.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "interfaces/nvmem.h"
#include "ui/ui_default.h"
#include "ui/ui_quickmenu.h"
}

extern state_t state;

namespace
{

constexpr freq_t FREQ = 145500000;

/**
 * One key event, as the keypad queue delivers it. Returns whether the pass asked
 * for an RTX resync: that flag is how the UI tells the RTX thread that a
 * setting it copies into rtx_cfg has changed.
 */
bool pressKey(uint32_t keys)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = 0;

    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
    return sync;
}

/** A digit is a press and a release, as the matrix reports it. */
void digit(uint32_t k)
{
    pressKey(k);
    pressKey(0);
}

/**
 * Open the host settings NVM once, in the scratch directory: nvm_init() is what
 * the emulator's main() calls and a test binary does not, and pointing it at the
 * desktop application's state file would overwrite it.
 */
void openNvm(void)
{
    static bool opened = false;

    if (!opened) {
        setenv("XDG_STATE_HOME",
               "/home/n0p/.hermes/cache/scratch/identity-nvm", 1);
        nvm_init();
        opened = true;
    }
}

/** A clean main screen; the UI state is a static of ui.c and survives. */
void resetUi(void)
{
    /*
     * Close what the previous case left open first. The quick menu and the
     * macro grid are overlays with no public handle: while one is open every
     * key goes to it and never reaches the screen below, which makes the next
     * test walk the grid instead of the menus. Red closes both, and drops a
     * stale edit if it finds one.
     */
    qm_close();
    qm_page_close();
    for (int i = 0; i < 4; i++)
        pressKey(KEY_ESC);

    ui_setInputLock(false);
    macro_latched = false;

    state.channel.mode             = OPMODE_FM;
    state.channel.rx_frequency     = FREQ;
    state.channel.tx_frequency     = FREQ;
    state.channel.bandwidth        = BW_25;
    state.channel.power            = 1000;
    state.channel.rx_only          = false;
    state.ui_screen                = MAIN_VFO;
    state.tuner_mode               = VFO;
    state.txDisable                = false;
    state.settings                 = default_settings;
    state.settings.vpLevel         = vpNone;   /* silent in the harness */

    qm_close();
    qm_page_close();
    ui_menu_select(0);
    macro_latched = false;
}

/** Main screen -> Menu -> Settings -> Identity, with the keys the radio has. */
void openIdentity(void)
{
    resetUi();

    pressKey(KEY_ENTER);                       /* main menu */
    REQUIRE(state.ui_screen == MENU_TOP);
    for (uint8_t i = 0; i < M_SETTINGS; i++)
        pressKey(KEY_DOWN);
    pressKey(KEY_ENTER);                       /* Settings list */
    REQUIRE(state.ui_screen == MENU_SETTINGS);

    for (uint8_t i = 0; i < S_IDENTITY; i++)
        pressKey(KEY_DOWN);
    pressKey(KEY_ENTER);                       /* the page itself */
    REQUIRE(state.ui_screen == SETTINGS_IDENTITY);
}

} /* namespace */

/* ------------------------------------------------------------- validation -- */

TEST_CASE("The callsign field takes the callsign alphabet and nothing else",
          "[ui][identity][callsign]")
{
    /* What the entry offers: A-Z, 0-9, '/' and '-' */
    REQUIRE(settings_callsignValid("N0CALL"));
    REQUIRE(settings_callsignValid("EA5TEST"));
    REQUIRE(settings_callsignValid("M0ABC"));
    REQUIRE(settings_callsignValid("EA5/A"));
    REQUIRE(settings_callsignValid("K1-2"));
    REQUIRE(settings_callsignValid("0"));

    /* Nine characters is the field; the M17 source takes exactly that many */
    REQUIRE(settings_callsignValid("ABCDEFGHI"));
    REQUIRE_FALSE(settings_callsignValid("ABCDEFGHIJ"));
    REQUIRE(strlen("ABCDEFGHI") < SETTINGS_CALLSIGN_LEN);

    /* An empty identity is not an identity: M17 would send an all-zero source
     * and APRS a frame with no base address */
    REQUIRE_FALSE(settings_callsignValid(""));
    REQUIRE_FALSE(settings_callsignValid(NULL));

    /* Outside the alphabet the entry cannot produce; a blank at either end is
     * the 0 key's alternate symbol pressed too many times, not an identity */
    REQUIRE_FALSE(settings_callsignValid("ea5test"));
    REQUIRE_FALSE(settings_callsignValid("EA5 TEST!"));
    REQUIRE_FALSE(settings_callsignValid(" N0CALL"));
    REQUIRE_FALSE(settings_callsignValid("N0CALL "));
    REQUIRE_FALSE(settings_callsignValid("N0\tCALL"));

    /* A blank between two words is allowed: it is in the entry's alphabet */
    REQUIRE(settings_callsignValid("N0 CALL"));
}

TEST_CASE("The DMR ID field takes 1..16777215 and nothing else",
          "[ui][identity][dmr]")
{
    REQUIRE_FALSE(settings_dmrIdValid(0));      /* no identity, no call */
    REQUIRE(settings_dmrIdValid(1));
    REQUIRE(settings_dmrIdValid(2143001));
    REQUIRE(settings_dmrIdValid(16777215U));    /* 24 bits on the air */
    REQUIRE_FALSE(settings_dmrIdValid(16777216U));
    REQUIRE_FALSE(settings_dmrIdValid(0xFFFFFFFFU));
}

/* -------------------------------------------------------------- the page --- */

TEST_CASE("The Identity page edits the callsign, applies it and stores it",
          "[ui][identity][callsign]")
{
    openIdentity();

    /* Row 0 is the callsign, and it already shows what is in force */
    REQUIRE(std::strcmp(settings_identity_items[ID_CALLSIGN], "Callsign") == 0);
    REQUIRE(std::strcmp(state.settings.callsign, "N0CALL") == 0);

    /* ENTER starts the text entry the M17 page used to own; the box is drawn
     * from new_callsign, which starts empty */
    pressKey(KEY_ENTER);
    REQUIRE(state.ui_screen == SETTINGS_IDENTITY);

    /* D, G, T, 0: one press per key, in the callsign alphabet */
    digit(KEY_3);
    digit(KEY_4);
    digit(KEY_8);
    digit(KEY_0);

    /* Nothing is applied until the confirm */
    REQUIRE(std::strcmp(state.settings.callsign, "N0CALL") == 0);

    bool sync = pressKey(KEY_ENTER);
    REQUIRE(std::strcmp(state.settings.callsign, "DGT0") == 0);
    REQUIRE(sync);              /* the RTX copies it into rtx_cfg on that pass */

    /* And it is what the settings record -- the NVM -- carries */
    openNvm();
    REQUIRE(nvm_writeSettings(&state.settings) == 0);

    settings_t back;
    std::memset(&back, 0, sizeof(back));
    REQUIRE(nvm_readSettings(&back) == 0);
    REQUIRE(std::strcmp(back.callsign, "DGT0") == 0);

    /* Red leaves the page back to the Settings list */
    pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
}

TEST_CASE("The Identity page edits the DMR ID, applies it and stores it",
          "[ui][identity][dmr]")
{
    openIdentity();

    /* Row 1: the radio's own subscriber ID, empty in a default record */
    pressKey(KEY_DOWN);
    REQUIRE(state.settings.dmr_id == 0);

    pressKey(KEY_ENTER);

    /* 2143001, the DMR-MARC style ID of the test in tests/unit/pc_proto.cpp */
    for (uint32_t k : { KEY_2, KEY_1, KEY_4, KEY_3, KEY_0, KEY_0, KEY_1 })
        digit(k);

    REQUIRE(state.settings.dmr_id == 0);        /* still not applied */

    bool sync = pressKey(KEY_ENTER);
    REQUIRE(state.settings.dmr_id == 2143001);
    REQUIRE(sync);

    openNvm();
    REQUIRE(nvm_writeSettings(&state.settings) == 0);

    settings_t back;
    std::memset(&back, 0, sizeof(back));
    REQUIRE(nvm_readSettings(&back) == 0);
    REQUIRE(back.dmr_id == 2143001);
    pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
}

TEST_CASE("An out-of-range DMR ID is refused, not clamped",
          "[ui][identity][dmr]")
{
    openIdentity();
    pressKey(KEY_DOWN);

    const uint32_t before = state.settings.dmr_id;
    REQUIRE(before == 0);

    /* Eight nines: more than 24 bits, and it must not wrap into a valid ID */
    pressKey(KEY_ENTER);
    for (int i = 0; i < 8; i++)
        digit(KEY_9);

    bool sync = pressKey(KEY_ENTER);
    REQUIRE(state.settings.dmr_id == before);
    REQUIRE_FALSE(sync);                        /* nothing to resync */
    REQUIRE(state.ui_screen == SETTINGS_IDENTITY); /* edit mode still open */

    /* 0 is refused as well: a call from nobody is not a call */
    pressKey(KEY_ESC);                          /* back to the list */
    pressKey(KEY_ENTER);                        /* a fresh entry, zero digits */
    sync = pressKey(KEY_ENTER);                 /* confirm 0 */
    REQUIRE(state.settings.dmr_id == before);
    REQUIRE_FALSE(sync);

    /* The page is still there and red still gets out of it, so a refused
     * confirm is a dead end only if the operator wants it to be */
    pressKey(KEY_ESC);
    pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
}

TEST_CASE("An empty callsign is refused, not stored",
          "[ui][identity][callsign]")
{
    openIdentity();

    /* ENTER and ENTER again: the entry is empty and the confirm must not
     * overwrite the identity with nothing (the M17 page used to store the
     * entry's own "_" placeholder this way) */
    pressKey(KEY_ENTER);
    bool sync = pressKey(KEY_ENTER);

    REQUIRE(std::strcmp(state.settings.callsign, "N0CALL") == 0);
    REQUIRE_FALSE(sync);
    REQUIRE(state.ui_screen == SETTINGS_IDENTITY);

    /* The edit is still open on the empty entry: red drops it and red again
     * leaves the page, and the identity is the one that was there */
    pressKey(KEY_ESC);
    pressKey(KEY_ESC);
    REQUIRE(std::strcmp(state.settings.callsign, "N0CALL") == 0);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
}
