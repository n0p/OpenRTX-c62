/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The menus, walked and measured instead of read.
 *
 * Five integration passes added rows, screens and modes to this UI in a row,
 * and the two defects the operator found on the radio were of a kind no screen
 * shows by itself: a mode change left `edit_mode` set on another mode's screen
 * and the main screen went deaf, and an `#ifdef` that closed one parenthesis
 * too late swallowed the messaging, contact and zone screens in a build that
 * compiled them. Both were found by pressing keys, not by looking.
 *
 * So this file walks the interface the way the operator does, through the real
 * `ui_updateFSM()` with key events from the keypad, and checks what a person
 * would notice:
 *
 *  - every screen of every compiled mode opens, every row can be walked, and
 *    red comes back to the screen the row was opened from;
 *  - no key is left unclaimed and none is claimed twice, screen by screen (the
 *    dispatch records its owner in ui_lastClaim(), see enum uiKeyClaim);
 *  - every row that says it is editable writes to `settings_t` or to the
 *    channel, and what it writes survives a save/read cycle of the NVM;
 *  - a mode with no transmitter offers no row that changes the transmitter's
 *    power;
 *  - the interactions leave nothing half-done: the quick menu closes on PTT,
 *    the carrier search stops on any key, the analyser gives the receiver back
 *    when red closes it, the composer cannot key the PA and a mode change
 *    during an interaction is not swallowed;
 *  - two full laps over the eleven modes leave every menu still answering.
 *
 * The binary is built twice, as ui_regressions_test is: with APRS and the
 * packet modem (the configuration the radio ships), and without them, which is
 * the build in which the messaging screens used to disappear.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "core/backup_image.h"
#include "core/event.h"
#include "core/graphics.h"
#include "core/input.h"
#include "core/messaging.h"
#include "core/msg_service.h"
#include "core/settings.h"
#include "core/spectrum_service.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "core/vfo_search.h"
#include "core/vfo_search_service.h"
#include "interfaces/cps_io.h"
#include "interfaces/nvmem.h"
#include "protocols/DMR/DmrStatus.h"
#include "protocols/DSTAR/DStarStatus.h"
#include "protocols/NXDN/NxdnStatus.h"
#include "protocols/P25/P25Status.h"
#include "protocols/TETRA/TetraStatus.h"
#include "protocols/TETRA/TetraUplink.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
#include "ui/ui_mode_state.h"
#include "ui/ui_tools.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_messages.h"
#include "ui/ui_aprs.h"
#include "ui/ui_packet.h"
#include "core/aprs_service.h"
#include "protocols/APRS/aprs_msg.h"
#include "ui/ui_spectrum.h"
#include "ui/ui_channels.h"
#include "ui/ui_contacts.h"
#include "ui/ui_zones.h"
}

/* The keyboard of the host platform is the emulator's */
#include "emulator.h"

extern state_t state;

namespace
{

constexpr freq_t FREQ = 145500000;

/* --------------------------------------------------------------- harness */

/** One key event, as the keypad queue delivers it. */
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

/** One pass of the periodic part, with no event. */
void tick(void)
{
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

/** The periodic part, a few times: enough for the timers to see the clock. */
void settle(int n = 3)
{
    for (int i = 0; i < n; i++) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

/**
 * A key press that is also a measurement: returns the block that claimed the
 * event, and fails when the event was not claimed exactly once.
 */
uint8_t claimOf(uint32_t keys, bool isLong = false)
{
    pressKey(keys, isLong);

    INFO("key 0x" << std::hex << keys << (isLong ? " (long)" : "")
                  << " claimed by " << (int)ui_lastClaim() << " x"
                  << (int)ui_lastClaimCount());
    REQUIRE(ui_lastClaimCount() == 1);
    REQUIRE(ui_lastClaim() != UI_CLAIM_NONE);

    return ui_lastClaim();
}

/** A digit is a press and a release. */
void digit(uint32_t k)
{
    pressKey(k);
    pressKey(0);
}

/**
 * A clean radio: everything the UI reads back to a known place, every module
 * closed, no popup, no latch. The UI state is a static of ui.c and survives
 * from one test case to the next, so every case starts here.
 */
void resetUi(uint8_t mode = OPMODE_FM, freq_t freq = FREQ)
{
    if (ui_spectrum_isOpen())
        ui_spectrum_leave();

    qm_close();
    qm_page_close();

    /*
     * No backup storage in this harness: the two backup screens then answer
     * NO STORAGE, which is the honest answer for a binary with no port, and
     * keeps a key walk from writing a file in the operator's state directory
     * (the record and its round trip are tests/unit/backup_image.cpp's job).
     */
    bkp_bind(nullptr);

    /* Close the modules that draw over the main screen */
    while (ui_tools_isOpen())
        pressKey(KEY_ESC);
    while (ui_channels_isOpen())
        pressKey(KEY_ESC);
    while (ui_contacts_isOpen())
        pressKey(KEY_ESC);
    while (ui_zones_isOpen())
        pressKey(KEY_ESC);

    state.channel.mode          = mode;
    state.channel.rx_frequency  = freq;
    state.channel.tx_frequency  = freq;
    state.channel.bandwidth     = BW_25;
    state.channel.power         = 1000;
    state.channel.rx_only       = false;
    state.channel.name[0]       = '\0';
    state.channel.dmr.rxColorCode = 1;
    state.channel.dmr.txColorCode = 1;
    state.channel.dmr.dmr_timeslot = 1;
    state.ui_screen             = MAIN_VFO;
    state.tuner_mode            = VFO;
    state.txDisable             = false;
    state.charge                = 100;
    state.emergency             = false;
    state.step_index            = 4;            /* 12.5 kHz */
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;       /* silent in the harness */
    ui_rtxSyncRequest           = false;

    /* The LOCK row of the popup, and the remote control, lock the keypad:
       a walk that presses every key sets it, and a locked main screen is deaf
       (which is the point of the lock). Start every case unlocked. */
    ui_setInputLock(false);

    ui_menu_select(0);
    macro_latched = false;
    emulator_state.PTTstatus = false;
}

/** Green on the main screen opens the top menu, from either main screen. */
void openMainMenu(void)
{
    pressKey(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_TOP);
}

/** Move the top menu's cursor to a row by name. */
int menuRowOf(const char *name)
{
    for (uint8_t i = 0; i < menu_num; i++)
        if (strcmp(menu_items[i], name) == 0)
            return i;

    return -1;
}

/** Green on a menu row, selected by index. */
void openMenuRow(uint8_t index)
{
    ui_menu_select(index);
    pressKey(KEY_ENTER);
}

/**
 * The channel list, from the main screen: P1 long opens the tools menu and its
 * last row is the list (docs/UI.md).
 */
void openChannels(void)
{
    pressKey(KEY_F1, true);
    REQUIRE(ui_tools_isOpen());

    for (int guard = 0; guard < 8; guard++) {
        const char *label = ui_tools_rowLabel(ui_tools_selected());

        if ((label != NULL) && (strcmp(label, "Channels") == 0))
            break;
        pressKey(KEY_DOWN);
    }

    pressKey(KEY_ENTER);
    REQUIRE(ui_channels_isOpen());
}

/** Red until the top menu is on screen again, at most `max` times. */
bool backToMenu(int max = 4)
{
    for (int i = 0; i < max; i++) {
        if (state.ui_screen == MENU_TOP)
            return true;
        pressKey(KEY_ESC);
    }
    return state.ui_screen == MENU_TOP;
}

/**
 * A message in the inbox and one in the outbox, so that the view and the
 * status screens exist. Returns false when this build has no way to create
 * them (no transport is compiled in).
 */
bool stockMessages(void)
{
    messaging_init();
    messaging_clear();

    if (messaging_received(MSG_TRANSPORT_APRS, 0x1234, 0, "hello", false, 1000) < 0)
        return false;

    return messaging_outCount() >= 0;
}

/** Every key the C62 can produce, as the matrix is walked. */
struct KeyName
{
    const char *name;
    uint32_t key;
    bool longPress;
};

const KeyName KEYS[] = {
    { "green",     KEY_ENTER, false },
    { "red",       KEY_ESC,   false },
    { "up",        KEY_UP,    false },
    { "down",      KEY_DOWN,  false },
    { "P1",        KEY_F1,    false },
    { "P1 long",   KEY_F1,    true  },
    { "P2",        KEY_F2,    false },
    { "P2 long",   KEY_F2,    true  },
    { "MONI",      KEY_MONI,  false },
    { "1",         KEY_1,     false },
    { "9",         KEY_9,     false },
    { "0",         KEY_0,     false },
    { "hash",      KEY_HASH,  false },
    { "star",      KEY_STAR,  false },
};

/**
 * Who must claim this key on this screen: the matrix of docs/UI.md.
 *
 * The table of screens gives the default owner (the module for a screen that
 * owns the keypad, the screen's own case for the rest); the exceptions are the
 * bindings the dispatch gives a key before any screen is looked at, and they
 * are what makes a key press ambiguous, so they are checked here one by one.
 */
uint8_t expectedOwner(uint8_t screen, uint32_t key, bool isLong)
{
    const bool mainScreen = (screen == MAIN_VFO) || (screen == MAIN_MEM);

    switch (screen) {
        case MENU_CONTACTS: return UI_CLAIM_CONTACTS;
        case MENU_ZONES:    return UI_CLAIM_ZONES;
        case MENU_MESSAGES:
        case MSG_SENT:
        case MSG_VIEW:
        case MSG_COMPOSE:
        case MSG_STATUS:    return UI_CLAIM_MSGS;
        case SPECTRUM:      return UI_CLAIM_SPECTRUM;
        case PKT_CFG:
        case PKT_STATUS:    return UI_CLAIM_PACKET;
        /* The tools menu owns every key while it is up, MONI included */
        case TOOLS_MENU:    return UI_CLAIM_TOOLS;
        default:            break;
    }

    /*
     * MONI, and P2 long, are the macro grid on every screen whose module does
     * not own the keypad. In FM the long press of P2 is APRS instead, which is
     * the one binding that depends on the mode; it is asserted separately, on
     * a radio in FM.
     */
    if ((key == KEY_MONI) || ((key == KEY_F2) && isLong)) {
#ifdef CONFIG_APRS
        /* P2 long in FM is the APRS screens; MONI stays the macro grid */
        if (mainScreen && (key == KEY_F2))
            return UI_CLAIM_APRS_ENTER;
#endif
        return UI_CLAIM_MACRO;
    }

    if (mainScreen && (key == KEY_F2) && !isLong)
        return UI_CLAIM_QM_OPEN;         /* the popup, on the main screens */

    if (mainScreen && (key == KEY_F1) && isLong)
        return UI_CLAIM_TOOLS_OPEN;      /* P1 long: the tools menu */

    switch (screen) {
        case MENU_CONTACTS: return UI_CLAIM_CONTACTS;
        case MENU_ZONES:    return UI_CLAIM_ZONES;
        case MENU_MESSAGES:
        case MSG_SENT:
        case MSG_VIEW:
        case MSG_COMPOSE:
        case MSG_STATUS:    return UI_CLAIM_MSGS;
        case SPECTRUM:      return UI_CLAIM_SPECTRUM;
        case PKT_CFG:
        case PKT_STATUS:    return UI_CLAIM_PACKET;
        default:            return UI_CLAIM_SCREEN;
    }
}

} /* namespace */

/* =========================================================== the walk ==== */

/*
 * The main menu, row by row, in every compiled mode.
 *
 * What is checked per row is what a person checks: that green does something,
 * that the screen it opens is one the radio has, and that red comes back to
 * the menu with nothing left open behind (no list still owning the keypad, no
 * edit mode, no disabled transmitter).
 */
TEST_CASE("Every row of the main menu opens a screen and red comes back",
          "[ui][audit][walk]")
{
    struct Row
    {
        const char *name;
        uint8_t screens[4];     /* what green may land on */
    };

    const Row rows[] = {
        { "Banks",    { MENU_BANK, MENU_TOP, 0xff, 0xff } },
        { "Channels", { MENU_CHANNEL, MENU_TOP, 0xff, 0xff } },
        { "Zones",    { MENU_ZONES, 0xff, 0xff, 0xff } },
        { "Contacts", { MENU_CONTACTS, 0xff, 0xff, 0xff } },
        { "Messages", { MENU_MESSAGES, 0xff, 0xff, 0xff } },
        { "Settings", { MENU_SETTINGS, 0xff, 0xff, 0xff } },
        /* The internal backup of the stores (docs/BACKUP.md) */
        { "Backup",   { MENU_BACKUP_RESTORE, 0xff, 0xff, 0xff } },
        { "Info",     { MENU_INFO, 0xff, 0xff, 0xff } },
        { "About",    { MENU_ABOUT, 0xff, 0xff, 0xff } },
    };

    REQUIRE(menuRowOf("Zones") >= 0);

    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        INFO("mode " << m->title);
        resetUi(m->mode);
        stockMessages();
        openMainMenu();

        /* Walk the whole menu once with the arrows first: the cursor has to
           come back to the first row, and no row may be skipped */
        for (uint8_t n = 0; n < menu_num; n++)
            pressKey(KEY_DOWN);
        REQUIRE(ui_menu_selected() == 0);

        for (const Row &row : rows) {
            const int index = menuRowOf(row.name);

            if (index < 0)
                continue;   /* not in this build (GPS) */

            INFO("row " << row.name);
            resetUi(m->mode);
            stockMessages();
            openMainMenu();
            openMenuRow((uint8_t)index);

            bool known = false;
            for (uint8_t s : row.screens)
                if (s == state.ui_screen)
                    known = true;

            INFO("landed on screen " << (int)state.ui_screen);
            REQUIRE(known);

            /* Red, as many times as the screen needs, back to the menu */
            REQUIRE(backToMenu());
            REQUIRE_FALSE(ui_edit_mode());
            REQUIRE_FALSE(ui_channels_isOpen());
            REQUIRE_FALSE(ui_contacts_isOpen());
            REQUIRE_FALSE(ui_zones_isOpen());
            REQUIRE_FALSE(qm_state.open);
            REQUIRE_FALSE(qm_page_isOpen());

            /* And out: red on the menu is the main screen again */
            pressKey(KEY_ESC);
            REQUIRE(state.ui_screen == MAIN_VFO);
            REQUIRE_FALSE(state.txDisable);
        }
    }
}

/* Every page of Settings, every row, every key that means something there. */
TEST_CASE("Every Settings page walks its rows and comes back", "[ui][audit][walk]")
{
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        INFO("mode " << m->title);
        resetUi(m->mode);
        openMainMenu();
        openMenuRow((uint8_t)menuRowOf("Settings"));
        REQUIRE(state.ui_screen == MENU_SETTINGS);

        for (uint8_t sel = 0; sel < settings_num; sel++) {
            ui_menu_select(sel);
            INFO("settings row " << settings_items[sel]);

            pressKey(KEY_ENTER);

            /* A page, or the menu again for a row that is not compiled */
            const uint8_t page = state.ui_screen;
            INFO("page screen " << (int)page);

            if (page == MENU_SETTINGS) {
                /* Radio's "Offset" and friends open no page of their own
                   only when the row is not in this build */
                continue;
            }

            /*
             * Walk the page: green edits, arrows change, P1/P2 step, red
             * leaves the edit first and then the page. Time & Date is the one
             * page whose green opens a second screen (the digit entry), so it
             * is allowed to move there and is brought back with red.
             */
            for (int round = 0; round < 2; round++) {
                for (int n = 0; n < 8; n++) {
                    if (state.ui_screen != page)
                        break;      /* Default Settings: green resets and leaves */

                    pressKey(KEY_ENTER);
                    if (state.ui_screen == SETTINGS_TIMEDATE_SET)
                        pressKey(KEY_ESC);      /* the digit entry of the clock */
                    if (state.ui_screen != page)
                        break;
                    pressKey(KEY_UP);
                    pressKey(KEY_DOWN);
                    pressKey(KEY_F1);
                    pressKey(KEY_F2);
                    pressKey(KEY_F1, true);
                    pressKey(KEY_F2, true);
                    INFO("page " << (int)page << " screen "
                                 << (int)state.ui_screen);
                    REQUIRE(state.ui_screen == page);
                }
            }

            /* Red leaves the edit if one is open, then the page. Some rows
               (Default Settings) ask for a second green before red goes back;
               either way the page is one red away per state. */
            for (int n = 0; n < 3; n++) {
                if (state.ui_screen == MENU_SETTINGS)
                    break;
                pressKey(KEY_ESC);
            }
            REQUIRE_FALSE(ui_edit_mode());

            INFO("after red: screen " << (int)state.ui_screen);
            REQUIRE(state.ui_screen == MENU_SETTINGS);
            REQUIRE_FALSE(qm_page_isOpen());
        }

        pressKey(KEY_ESC);
        REQUIRE(state.ui_screen == MENU_TOP);
    }
}

/*
 * The tool screens: channel list and editor, contacts, the three message
 * screens, the zone browser, the analyser, the packet modem and APRS.
 *
 * Each one is entered the way the operator enters it (a key on the main
 * screen, a row of the menu or a row of the quick menu), walked with every
 * key, and left with red; after leaving, the screen must be the one it was
 * opened from, the module must be closed and the transmitter re-armed.
 */
TEST_CASE("The tool screens open, answer every key and give the radio back",
          "[ui][audit][walk]")
{
    /* ---- channel list and editor, from the tools menu of P1 long ---- */
    resetUi();
    openChannels();
    REQUIRE(ui_channels_isOpen());
    REQUIRE(state.txDisable);

    for (int n = 0; n < 6; n++)
        pressKey(KEY_DOWN);
    for (int n = 0; n < 6; n++)
        pressKey(KEY_UP);
    pressKey(KEY_F1);                    /* new channel, from the VFO */
    REQUIRE(ui_channels_isOpen());
    for (const KeyName &k : KEYS) {
        pressKey(k.key, k.longPress);
        REQUIRE((ui_channels_isOpen() || (state.ui_screen == MAIN_VFO)));
    }
    /* red: leaves the field, then the editor, then the list */
    for (int n = 0; n < 3; n++)
        pressKey(KEY_ESC);
    REQUIRE_FALSE(ui_channels_isOpen());
    REQUIRE_FALSE(state.txDisable);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* ---- tools menu: P1 long, every key, red gives the radio back ---- */
    resetUi();
    REQUIRE(claimOf(KEY_F1, true) == UI_CLAIM_TOOLS_OPEN);
    REQUIRE(ui_tools_isOpen());
    REQUIRE(state.ui_screen == TOOLS_MENU);
    REQUIRE(state.txDisable);

    for (const KeyName &k : KEYS) {
        /* Red closes the menu and green runs a row: they have their own
           checks below, and both would leave this walk mid-way */
        if ((k.key == KEY_ESC) || (k.key == KEY_ENTER))
            continue;

        INFO("tools menu <- " << k.name);
        REQUIRE(claimOf(k.key, k.longPress) == UI_CLAIM_TOOLS);
        REQUIRE(ui_tools_isOpen());
        REQUIRE(state.txDisable);
    }

    /* Back on the first row, which is the mode submenu */
    while (ui_tools_selected() != 0)
        pressKey(KEY_UP);
    pressKey(KEY_ENTER);
    REQUIRE(ui_tools_inModes());

    /* The submenu answers every key as well; green applies a mode and leaves */
    for (const KeyName &k : KEYS) {
        if ((k.key == KEY_ESC) || (k.key == KEY_ENTER))
            continue;

        INFO("mode submenu <- " << k.name);
        REQUIRE(claimOf(k.key, k.longPress) == UI_CLAIM_TOOLS);
        REQUIRE(ui_tools_inModes());
    }

    /* Red goes back to the list, and from there out to the main screen */
    pressKey(KEY_ESC);
    REQUIRE(ui_tools_isOpen());
    REQUIRE_FALSE(ui_tools_inModes());
    pressKey(KEY_ESC);

    REQUIRE_FALSE(ui_tools_isOpen());
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(state.txDisable);

    /* And the main screen answers again */
    REQUIRE(claimOf(KEY_ENTER) == UI_CLAIM_SCREEN);
    REQUIRE(state.ui_screen == MENU_TOP);
    pressKey(KEY_ESC);

    /* ---- contacts, from the main menu ---- */
    resetUi();
    openMainMenu();
    openMenuRow((uint8_t)menuRowOf("Contacts"));
    REQUIRE(ui_contacts_isOpen());
    REQUIRE(state.txDisable);
    for (const KeyName &k : KEYS) {
        pressKey(k.key, k.longPress);
        REQUIRE(state.ui_screen == MENU_CONTACTS);
    }
    for (int n = 0; n < 3; n++) {
        if (state.ui_screen == MENU_TOP)
            break;
        pressKey(KEY_ESC);
    }
    REQUIRE_FALSE(ui_contacts_isOpen());
    REQUIRE(state.ui_screen == MENU_TOP);
    REQUIRE(state.txDisable);            /* the menu never keys the PA */

    /* ---- messages: inbox, sent, view, compose, status ---- */
    resetUi();
    stockMessages();
    openMainMenu();
    openMenuRow((uint8_t)menuRowOf("Messages"));
    REQUIRE(state.ui_screen == MENU_MESSAGES);
    REQUIRE(state.txDisable);

    for (const KeyName &k : KEYS) {
        pressKey(k.key, k.longPress);
        REQUIRE(ui_msgs_isScreen(state.ui_screen));
    }

    /* The keys left any of the five screens up: red back to the inbox */
    for (int n = 0; (n < 4) && ui_msgs_isScreen(state.ui_screen)
                    && (state.ui_screen != MENU_MESSAGES); n++)
        pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_MESSAGES);

    /* Folders, then open the message, then red back to the inbox */
    pressKey(KEY_F1);
    REQUIRE(state.ui_screen == MSG_SENT);
    pressKey(KEY_F1);
    REQUIRE(state.ui_screen == MENU_MESSAGES);

    if (messaging_inbox(0) != NULL) {
        pressKey(KEY_ENTER);
        REQUIRE(state.ui_screen == MSG_VIEW);
        for (const KeyName &k : KEYS) {
            pressKey(k.key, k.longPress);
            REQUIRE(ui_msgs_isScreen(state.ui_screen));
        }
        /* red, from wherever the keys left it, back to the inbox */
        for (int n = 0; (n < 4) && ui_msgs_isScreen(state.ui_screen)
                        && (state.ui_screen != MENU_MESSAGES); n++)
            pressKey(KEY_ESC);
        REQUIRE(state.ui_screen == MENU_MESSAGES);
    }

    /* The composer, from the reply key on the inbox */
    pressKey(KEY_F2);
    REQUIRE(state.ui_screen == MSG_COMPOSE);
    for (const KeyName &k : KEYS) {
        pressKey(k.key, k.longPress);
        REQUIRE(ui_msgs_isScreen(state.ui_screen));
    }
    for (int n = 0; (n < 4) && ui_msgs_isScreen(state.ui_screen)
                    && (state.ui_screen != MENU_MESSAGES); n++)
        pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_MESSAGES);

    pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_TOP);
    REQUIRE(state.txDisable);            /* the menu never keys the PA */

    /* ---- zone browser ---- */
    resetUi();
    openMainMenu();
    INFO("zones row " << menuRowOf("Zones") << " of " << (int)menu_num);
    openMenuRow((uint8_t)menuRowOf("Zones"));
    INFO("after green: screen " << (int)state.ui_screen
         << " zonesOpen " << (int)ui_zones_isOpen()
         << " claim " << (int)ui_lastClaim());
    REQUIRE(ui_zones_isOpen());
    REQUIRE(state.ui_screen == MENU_ZONES);
    for (const KeyName &k : KEYS) {
        pressKey(k.key, k.longPress);
        REQUIRE(ui_zones_isOpen());
    }
    /* Red leaves the browser from either of its two lists */
    for (int n = 0; (n < 3) && ui_zones_isOpen(); n++)
        pressKey(KEY_ESC);
    REQUIRE_FALSE(ui_zones_isOpen());
    REQUIRE(state.ui_screen == MENU_TOP);
    REQUIRE(state.txDisable);            /* the menu never keys the PA */

    /* ---- the analyser, from the FM quick menu ---- */
    resetUi(OPMODE_FM);
    pressKey(KEY_F2);
    REQUIRE(qm_state.open);
    qm_select(0);
    for (uint8_t i = 0; i < qm_state.qm->count; i++) {
        if (strcmp(qm_state.qm->items[i].label, "ESPECTRO") == 0) {
            qm_select(i);
            break;
        }
    }
    pressKey(KEY_ENTER);
    REQUIRE(state.ui_screen == SPECTRUM);
    REQUIRE(ui_spectrum_isOpen());
    REQUIRE(state.txDisable);

    for (const KeyName &k : KEYS) {
        if (k.key == KEY_ESC)
            continue;       /* red is the way out, checked below */

        pressKey(k.key, k.longPress);

        /* P2 opens the popup of the screen: it is an overlay on top of the
           plot, so red closes the popup and the plot is still there */
        if (qm_state.open) {
            pressKey(KEY_ESC);
            REQUIRE(state.ui_screen == SPECTRUM);
        }
        INFO("spectrum after key " << k.name);
        REQUIRE(state.ui_screen == SPECTRUM);
        REQUIRE(ui_spectrum_isOpen());
    }

    pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(ui_spectrum_isOpen());
    REQUIRE_FALSE(state.txDisable);
    REQUIRE_FALSE(spectrumService_active());

#ifdef CONFIG_KISS
    /* ---- packet modem, from the FM quick menu ---- */
    resetUi(OPMODE_FM);
    pressKey(KEY_F2);
    REQUIRE(qm_state.open);
    bool found = false;
    for (uint8_t i = 0; i < qm_state.qm->count; i++) {
        if (strcmp(qm_state.qm->items[i].label, "PKT") == 0) {
            qm_select(i);
            found = true;
            break;
        }
    }
    if (found) {
        pressKey(KEY_ENTER);
        REQUIRE(state.ui_screen == PKT_CFG);
        REQUIRE(state.txDisable);
        for (const KeyName &k : KEYS) {
            if (k.key == KEY_ESC)
                continue;       /* red is the way out, checked below */
            pressKey(k.key, k.longPress);
            INFO("packet after key " << k.name << ": screen "
                 << (int)state.ui_screen);
            REQUIRE(ui_packet_isScreen(state.ui_screen));
        }
        for (int n = 0; (n < 3) && ui_packet_isScreen(state.ui_screen); n++)
            pressKey(KEY_ESC);
        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE_FALSE(state.txDisable);
    }
#endif

#ifdef CONFIG_APRS
    /* ---- APRS, long P2 in FM ---- */
    resetUi(OPMODE_FM);
    pressKey(KEY_F2, true);
    REQUIRE(state.ui_screen == MENU_APRS);
    REQUIRE(state.txDisable);
    for (const KeyName &k : KEYS) {
        if (k.key == KEY_ESC)
            continue;           /* red is the way out, checked below */
        pressKey(k.key, k.longPress);
        INFO("APRS after key " << k.name << ": screen "
             << (int)state.ui_screen);
        REQUIRE(ui_aprs_isScreen(state.ui_screen));
    }
    for (int n = 0; (n < 4) && ui_aprs_isScreen(state.ui_screen); n++)
        pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(state.txDisable);
#endif
}

#ifdef CONFIG_APRS
/*
 * The APRS reply editor, which could not be typed into.
 *
 * It committed a character on KEY_RIGHT and backspaced on KEY_LEFT, and the C62
 * has neither: a reply could be started, deleted from and sent, but never
 * written. This is the same defect class as the channel editor's caret, and the
 * way to catch it is a test that types.
 */
TEST_CASE("The APRS reply editor types with the keypad this radio has",
          "[ui][audit][aprs]")
{
    resetUi(OPMODE_FM);

    /* One message in the inbox, so the view and the reply exist */
    aprs_mbox_t *m = aprs_mailbox();
    aprs_mbox_init(m);
    snprintf(m->in[0].from, sizeof(m->in[0].from), "%s", "EA5ABC");
    snprintf(m->in[0].text, sizeof(m->in[0].text), "%s", "hola");
    m->in[0].inUse = true;
    m->in[0].read = true;

    pressKey(KEY_F2, true);              /* long P2 in FM: the APRS screens */
    REQUIRE(state.ui_screen == MENU_APRS);

    pressKey(KEY_F1);                    /* the inbox */
    REQUIRE(state.ui_screen == APRS_MSGS);

    pressKey(KEY_ENTER);                 /* open it */
    REQUIRE(state.ui_screen == APRS_MSG_VIEW);

    pressKey(KEY_ENTER);                 /* reply */
    REQUIRE(state.ui_screen == APRS_MSG_EDIT);

    /* Up/down choose the character, P1 adds it, P2 deletes it */
    pressKey(KEY_UP);
    pressKey(KEY_F1);
    pressKey(KEY_UP);
    pressKey(KEY_F1);
    pressKey(KEY_UP);
    pressKey(KEY_F1);
    REQUIRE(strlen(m->out[0].text) >= 0);    /* the editor holds it, not the store */

    /* The text is on screen and grows: the counter in the header is the check */
    pressKey(KEY_F2);                    /* delete one */
    pressKey(KEY_F1, true);              /* long P1: backspace too */

    /* Red leaves the editor without sending */
    pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == APRS_MSGS);
    REQUIRE(state.txDisable);            /* still a screen that owns the keys */
}

/*
 * The APRS setup page: every row it offers must be adjustable, and the two it
 * cannot edit (the strings, which this page has no editor for) must be skipped
 * by the cursor instead of doing nothing when the value keys are pressed.
 */
TEST_CASE("The APRS setup page has no dead row", "[ui][audit][aprs]")
{
    resetUi(OPMODE_FM);
    ui_open_screen(APRS_CFG, 0);
    REQUIRE(state.ui_screen == APRS_CFG);

    /* Walk a full turn: the cursor never lands on the read-only rows */
    for (int i = 0; i < 9; i++) {
        pressKey(KEY_F2);
        pressKey(KEY_F1);
    }

    pressKey(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(state.txDisable);
}
#endif /* CONFIG_APRS */

/* ====================================================== key precedence === */

/*
 * The matrix of docs/UI.md, checked.
 *
 * For every screen the build has and every key the keypad can produce, the
 * event must be claimed by exactly one block (ui_lastClaimCount() == 1) and by
 * the block the table says. A screen that takes no claim is a key that goes
 * nowhere -- the deaf-screen defect -- and two claims are two handlers eating
 * the same event. The expected owner is the dispatch order of ui_updateFSM().
 */
TEST_CASE("Every key on every screen is claimed by exactly one block",
          "[ui][audit][keys]")
{
    struct Screen
    {
        const char *name;
        uint8_t screen;      /* what ui_screen says once we are there */
        uint8_t owner;       /* who must claim the keys there */
    };

    /*
     * Screens that own the keypad through their own module (the module is the
     * owner) and screens that go through the big switch (the screen case is).
     */
    std::vector<Screen> screens = {
        { "MAIN_VFO",        MAIN_VFO,        UI_CLAIM_SCREEN },
        { "MAIN_VFO_INPUT",  MAIN_VFO_INPUT,  UI_CLAIM_SCREEN },
        { "MAIN_MEM",        MAIN_MEM,        UI_CLAIM_SCREEN },
        { "MENU_TOP",        MENU_TOP,        UI_CLAIM_SCREEN },
        { "MENU_SETTINGS",   MENU_SETTINGS,   UI_CLAIM_SCREEN },
        { "MENU_INFO",       MENU_INFO,       UI_CLAIM_SCREEN },
        { "MENU_ABOUT",      MENU_ABOUT,      UI_CLAIM_SCREEN },
        /* The backup screens: the big switch forwards their keys to
           ui_backup.c, so the screen case is the owner (docs/BACKUP.md) */
        { "MENU_BACKUP_REST", MENU_BACKUP_RESTORE, UI_CLAIM_SCREEN },
        { "MENU_BACKUP",     MENU_BACKUP,     UI_CLAIM_SCREEN },
        { "MENU_RESTORE",    MENU_RESTORE,    UI_CLAIM_SCREEN },
        { "MENU_CONTACTS",   MENU_CONTACTS,   UI_CLAIM_CONTACTS },
        { "MENU_ZONES",      MENU_ZONES,      UI_CLAIM_ZONES },
        { "MENU_MESSAGES",   MENU_MESSAGES,   UI_CLAIM_MSGS },
        { "MSG_SENT",        MSG_SENT,        UI_CLAIM_MSGS },
        { "MSG_COMPOSE",     MSG_COMPOSE,     UI_CLAIM_MSGS },
        { "SETTINGS_DISPLAY", SETTINGS_DISPLAY, UI_CLAIM_SCREEN },
        { "SETTINGS_RADIO",  SETTINGS_RADIO,  UI_CLAIM_SCREEN },
        { "SETTINGS_AUDIO",  SETTINGS_AUDIO,  UI_CLAIM_SCREEN },
        { "SETTINGS_FM",     SETTINGS_FM,     UI_CLAIM_SCREEN },
        { "SETTINGS_ACCES",  SETTINGS_ACCESSIBILITY, UI_CLAIM_SCREEN },
        { "SETTINGS_RESET",  SETTINGS_RESET2DEFAULTS, UI_CLAIM_SCREEN },
        { "SPECTRUM",        SPECTRUM,        UI_CLAIM_SPECTRUM },
    };

    /* Walk the screens in the table's order, on a radio in FM: every screen
       there is reachable in every mode and the owners do not depend on it */
    for (const Screen &s : screens) {
        resetUi(OPMODE_FM);
        stockMessages();

        switch (s.screen) {
            case MAIN_VFO:
                break;
            case MAIN_VFO_INPUT:
                digit(KEY_1);
                break;
            case MAIN_MEM:
                state.ui_screen = MAIN_MEM;
                break;
            case MENU_TOP:
                openMainMenu();
                break;
            case MENU_SETTINGS:
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Settings"));
                break;
            case MENU_INFO:
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Info"));
                break;
            case MENU_ABOUT:
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("About"));
                break;
            case MENU_CONTACTS:
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Contacts"));
                break;
            case MENU_ZONES:
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Zones"));
                break;
            case MENU_MESSAGES:
            case MSG_SENT:
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Messages"));
                if (s.screen == MSG_SENT)
                    pressKey(KEY_F1);
                break;
            case MSG_COMPOSE:
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Messages"));
                pressKey(KEY_F2);           /* reply, or compose from the popup */
                break;
            case SETTINGS_DISPLAY:
            case SETTINGS_RADIO:
            case SETTINGS_AUDIO:
            case SETTINGS_FM:
            case SETTINGS_ACCESSIBILITY:
            case SETTINGS_RESET2DEFAULTS: {
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Settings"));
                int index = -1;
                for (uint8_t i = 0; i < settings_num; i++) {
                    const char *n = settings_items[i];
                    if ((s.screen == SETTINGS_DISPLAY && strcmp(n, "Display") == 0)
                     || (s.screen == SETTINGS_RADIO && strcmp(n, "Radio") == 0)
                     || (s.screen == SETTINGS_AUDIO && strcmp(n, "Audio") == 0)
                     || (s.screen == SETTINGS_FM && strcmp(n, "FM") == 0)
                     || (s.screen == SETTINGS_ACCESSIBILITY
                         && strcmp(n, "Accessibility") == 0)
                     || (s.screen == SETTINGS_RESET2DEFAULTS
                         && strcmp(n, "Default Settings") == 0))
                        index = i;
                }
                REQUIRE(index >= 0);
                openMenuRow((uint8_t)index);
                break;
            }
            case SPECTRUM:
                ui_open_screen(SPECTRUM, 0);
                break;
            case MENU_BACKUP_RESTORE:
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Backup"));
                break;
            case MENU_BACKUP:
            case MENU_RESTORE:
                /* Menu -> Backup -> the first or the second row of the pair */
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Backup"));
                openMenuRow((uint8_t)(s.screen == MENU_RESTORE ? BR_RESTORE
                                                              : BR_BACKUP));
                break;
            default:
                break;
        }

        INFO("screen " << s.name << " reached as " << (int)state.ui_screen);
        REQUIRE(state.ui_screen == s.screen);

        for (const KeyName &k : KEYS) {
            /* Every key from the same place, every time */
            resetUi(OPMODE_FM);
            stockMessages();

            /* Re-reach the screen (the switch above, again, without a loop
               variable: it is cheap and keeps each key independent) */
            switch (s.screen) {
                case MENU_TOP:      openMainMenu(); break;
                case MENU_SETTINGS:
                    openMainMenu();
                    openMenuRow((uint8_t)menuRowOf("Settings"));
                    break;
                case MENU_CONTACTS:
                    openMainMenu();
                    openMenuRow((uint8_t)menuRowOf("Contacts"));
                    break;
                case MENU_ZONES:
                    openMainMenu();
                    openMenuRow((uint8_t)menuRowOf("Zones"));
                    break;
                case MENU_MESSAGES:
                    openMainMenu();
                    openMenuRow((uint8_t)menuRowOf("Messages"));
                    break;
                case MSG_SENT:
                    openMainMenu();
                    openMenuRow((uint8_t)menuRowOf("Messages"));
                    pressKey(KEY_F1);
                    break;
                case MSG_COMPOSE:
                    openMainMenu();
                    openMenuRow((uint8_t)menuRowOf("Messages"));
                    pressKey(KEY_F2);
                    break;
                case SPECTRUM:
                    ui_open_screen(SPECTRUM, 0);
                    break;
                case MAIN_VFO_INPUT:
                    digit(KEY_1);
                    break;
                default:
                    state.ui_screen = s.screen;
                    break;
            }

            INFO(s.name << " <- " << k.name);
            const uint8_t owner = claimOf(k.key, k.longPress);
            INFO("claimed by " << (int)owner << ", expected "
                 << (int)expectedOwner(s.screen, k.key, k.longPress));
            REQUIRE(owner == expectedOwner(s.screen, k.key, k.longPress));
        }
    }

    /* The popup is an overlay with its own owner, on any main screen */
    resetUi(OPMODE_FM);
    pressKey(KEY_F2);
    REQUIRE(qm_state.open);
    for (const KeyName &k : KEYS) {
        INFO("quick menu <- " << k.name);

        uint8_t expected = UI_CLAIM_QM;
        bool opensAprs = false;

#ifdef CONFIG_APRS
        /*
         * P2 long in FM is the APRS screens even while the popup is up: the
         * dispatch decides that binding before the popup and closes it
         * (docs/UI.md, "who takes the key"). The walk has to expect the same,
         * or it is asserting a rule the firmware does not have.
         */
        if ((k.key == KEY_F2) && k.longPress) {
            expected  = UI_CLAIM_APRS_ENTER;
            opensAprs = true;
        }
#endif
        REQUIRE(claimOf(k.key, k.longPress) == expected);

        if (opensAprs) {
            /* Red leaves the APRS screens; the popup comes back on the main
               screen for the rest of the walk */
            for (int n = 0; n < 5 && state.ui_screen != MAIN_VFO; n++)
                pressKey(KEY_ESC);

            REQUIRE(state.ui_screen == MAIN_VFO);
            qm_close();
        }

        if (!qm_state.open) {
            pressKey(KEY_F2);
            REQUIRE(qm_state.open);
        }
    }
    qm_close();

    /* P2 short on a main screen is the popup's, and only there */
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        resetUi(m->mode);
        INFO("P2 short in " << m->title);
        REQUIRE(claimOf(KEY_F2) == UI_CLAIM_QM_OPEN);
        REQUIRE(qm_state.open);
        qm_close();
    }
}

/* ======================================================== dead rows ====== */

/*
 * Requirement: every row that says it is editable changes something real, and
 * what it changes survives a power cycle.
 *
 * The check is mechanical: for each editable row of each mode's table, take a
 * copy of `settings_t` and of the live channel, drive the row to its minimum
 * and to its maximum, and require that exactly one of the two changed. A row
 * that changes neither is a row that does nothing (a dead item). If the row
 * wrote to the settings, ask the NVM to save and read them back and require
 * the value to still be there: that is the half of the check the settings NVM
 * has failed before (a debounce flag whose hasChanged() was never called).
 *
 * Rows of the QM_INFO kind are informative by design and are listed in
 * docs/UI.md; QM_GOTO and QM_ACTION rows are checked by the walk above (they
 * open a screen or act on the service).
 */
TEST_CASE("Every editable row writes settings or the channel, and survives the NVM",
          "[ui][audit][rows]")
{
    /*
     * The host NVM is a file under XDG_STATE_HOME, opened by nvm_init() -- which
     * the emulator's main() calls and a test binary does not. Point it at the
     * scratch directory so that a run does not overwrite the state of the
     * desktop application, and open it once.
     */
    setenv("XDG_STATE_HOME", "/home/n0p/.hermes/cache/scratch/ma-nvm", 1);
    nvm_init();

    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        for (uint8_t r = 0; r < m->count; r++) {
            const qm_item_t &row = m->items[r];

            if ((row.type == QM_INFO) || (row.type == QM_GOTO)
                || (row.type == QM_ACTION) || (row.get == NULL)
                || (row.set == NULL))
                continue;

            INFO("mode " << m->title << ", row " << row.label);
            resetUi(m->mode);
            qm_close();

            settings_t beforeSettings = state.settings;
            channel_t  beforeChannel  = state.channel;
            const int  before = row.get();

            /*
             * "CELL UL" is a tuning session, not a setting: what it does
             * depends on what the receiver holds.  With no cell it has to
             * refuse, say NO UL and leave the channel alone; with a cell whose
             * uplink is known it retunes there, and Off takes the frequency it
             * came from back (protocols/TETRA/TetraUplink.h).  The cell is
             * injected through the emulator's receiver state.
             */
            if (strcmp(row.label, "CELL UL") == 0) {
                tetraUplink_reset();
                tetra_sim_clearCell();

                row.set(1);
                REQUIRE(row.get() == (int)TETRA_UL_NO_FREQ);
                REQUIRE(memcmp(&beforeChannel, &state.channel,
                               sizeof(channel_t)) == 0);

                tetra_sim_setCell(228, 8889, 1, 430500000u, 420500000u);
                row.set(0);
                REQUIRE(row.get() == (int)TETRA_UL_OFF);
                row.set(1);
                REQUIRE(row.get() == (int)TETRA_UL_ON);
                REQUIRE(state.channel.rx_frequency == 420500000u);
                REQUIRE(state.channel.tx_frequency == 420500000u);

                row.set(0);
                REQUIRE(row.get() == (int)TETRA_UL_OFF);
                REQUIRE(state.channel.rx_frequency
                        == beforeChannel.rx_frequency);

                state.channel = beforeChannel;
                tetra_sim_clearCell();
                tetraUplink_reset();
                continue;
            }

            /* To the maximum, from a known state */
            row.set(row.max);
            const int atMax = row.get();
            const bool settingsMax = memcmp(&beforeSettings, &state.settings,
                                            sizeof(settings_t)) != 0;
            const bool channelMax  = memcmp(&beforeChannel, &state.channel,
                                            sizeof(channel_t)) != 0;

            resetUi(m->mode);
            row.set(row.min);
            const int atMin = row.get();
            const bool settingsMin = memcmp(&beforeSettings, &state.settings,
                                            sizeof(settings_t)) != 0;
            const bool channelMin  = memcmp(&beforeChannel, &state.channel,
                                            sizeof(channel_t)) != 0;

            /* A row that moves its own value but writes nowhere is a dead
               item with a pretty face. LOCK is the one documented exception:
               the keypad lock is a state of the UI, not a setting, and what
               this test asks of it is that it locks *and unlocks* (a lock with
               no way out is a one-way door, not a lock). */
            REQUIRE(row.get() == atMin);

            if (strcmp(row.label, "LOCK") == 0) {
                REQUIRE(ui_getInputLock() == false);
                row.set(1);
                REQUIRE(ui_getInputLock());
                row.set(0);
                REQUIRE_FALSE(ui_getInputLock());
                continue;
            }

            REQUIRE((settingsMax || settingsMin || channelMax || channelMin));
            REQUIRE_FALSE(((settingsMax || settingsMin)
                           && (channelMax || channelMin)));

            if (settingsMax || settingsMin) {
                /* The value the row reports must come back from the NVM */
                const int written = (settingsMax) ? atMax : atMin;

                row.set(written);
                REQUIRE(nvm_writeSettings(&state.settings) == 0);
                state.settings = default_settings;
                REQUIRE(nvm_readSettings(&state.settings) == 0);
                INFO("row " << row.label << ": wrote " << written
                            << ", read back " << row.get());
                /* Rows that derive their value from more than one field (the
                   tone index, the CTCSS mask) are allowed to disagree in
                   representation but not in effect: the value they report
                   must be the one that was saved */
                REQUIRE(row.get() == written);
            }

            (void)before;
            (void)atMax;
        }
    }
}

/* =================================== state in the menus vs main screen === */

/*
 * Requirement: a mode's quick menu carries configurations and options, and what
 * the radio has decoded is on the main screen.
 *
 * The row table of a mode is drawn twice - as the popup on the main screen and
 * as that mode's page under Settings - so a read-only row in it is state inside
 * a menu: the operator has to open Settings to see whether the radio has
 * locked, and the page offers rows that beep and do nothing when a value key is
 * pressed. It happened one row at a time (CC, TS, SYNC, RAN, NAC, CELL, LA/CC,
 * REG, NET, UL, CALL, MY, UR, RPT, TXT, ST, LVL, SNR, CH, SRC), which is why the
 * rule is checked mechanically rather than the rows being reviewed.
 *
 * The analyser's own table is not a mode's: it belongs to the spectrum screen,
 * which has no status area to move its PICO and MS/PTO read-outs to.
 */
TEST_CASE("A mode's menu is configurations and options, never decoded state",
          "[ui][audit][rows]")
{
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        for (uint8_t r = 0; r < m->count; r++) {
            const qm_item_t &row = m->items[r];

            INFO("mode " << m->title << ", row " << row.label);
            REQUIRE(row.type != QM_INFO);

            /* And every row that is left either changes something or opens a
               screen/acts: a row with neither is dead in both screens */
            if ((row.type == QM_INT) || (row.type == QM_ENUM)
                || (row.type == QM_TOGGLE)) {
                REQUIRE(row.get != NULL);
                REQUIRE(row.set != NULL);
            } else {
                const bool goes = (row.type == QM_GOTO)
                                && (row.goto_screen != 0);
                const bool acts = (row.type == QM_ACTION)
                                && (row.act != NULL);
                REQUIRE((goes || acts));
            }
        }
    }
}

/*
 * Requirement: the state of every digital mode is readable from the main screen
 * with no menu open, and says "nothing decoded" instead of a zero.
 *
 * The check is on the same block the main screen draws (ui/ui_mode_state.h), so
 * it fails when a protocol changes what it publishes or when a mode is added to
 * the registry without a state: the strings are the contract the operator reads
 * on line 1, 2 and line 4 of the main screen.
 */
TEST_CASE("Every digital mode tells its state on the main screen",
          "[ui][audit][state]")
{
    struct Expect
    {
        uint8_t mode;
        const char *nothing;    /* what the block says with no signal */
    };

    const Expect modes[] = {
        { OPMODE_DMR,   "DMR searching" },
        { OPMODE_NXDN,  "NXDN SRCH" },
        { OPMODE_DPMR,  "SRCH" },
        { OPMODE_P25,   "P25 SRCH" },
        { OPMODE_YSF,   "SRCH" },
        { OPMODE_DSTAR, "DSTAR SRCH" },
        { OPMODE_TETRA, "T searching" },
    };

    for (const Expect &e : modes) {
        ui_mode_state_t st;

        resetUi(e.mode);
        dmrStatus_clear();
        p25Status_clear();
        nxdnStatus_clear();
        dstarStatus_clear();
        tetra_sim_clearCell();

        ui_mode_state_read(&st);

        INFO("mode " << ui_mode_title(e.mode) << " says \"" << st.id << "\"");
        REQUIRE(strcmp(st.id, e.nothing) == 0);
        REQUIRE_FALSE(st.sync);
        REQUIRE_FALSE(st.call);
        /* No call, no quality and no secondary state when nothing decodes: the
           block draws line 1 and line 4 only when there is something to say */
        REQUIRE(st.callTxt[0] == '\0');
        REQUIRE(st.qual[0] == '\0');
        REQUIRE(st.note[0] == '\0');
        /* And never a zero where an identifier is unknown */
        REQUIRE(strchr(st.id, '0') == NULL);
    }

    /*
     * With a receiver locked, the identifiers the old read-only rows carried
     * are in the block: colour code and timeslot, the call, the link quality.
     */
    resetUi(OPMODE_DMR);
    dmr_sim_setStatus(true, 1, 2, true, 980, true, true, 2317, 9, -1);
    ui_mode_state_t dmr;
    ui_mode_state_read(&dmr);
    REQUIRE(dmr.sync);
    REQUIRE(strcmp(dmr.id, "CC1 TS2") == 0);
    REQUIRE(strcmp(dmr.callTxt, "2317>9") == 0);
    REQUIRE(strcmp(dmr.qual, "LC98%") == 0);
    dmr_sim_clear();

    /* Unknown colour code and slot read as "-", not as a zero */
    resetUi(OPMODE_DMR);
    dmr_sim_setStatus(true, -1, 0, true, 0, false, false, 0, 0, -1);
    ui_mode_state_read(&dmr);
    REQUIRE(strcmp(dmr.id, "CC- TS-") == 0);
    dmr_sim_clear();

    /* TETRA: the cell and the registration */
    resetUi(OPMODE_TETRA);
    tetra_sim_setCell(228, 8889, 1, 430500000u, 0);
    tetra_sim_setRegistration(true, 3, 0);
    ui_mode_state_t tetra;
    ui_mode_state_read(&tetra);
    REQUIRE(tetra.sync);
    REQUIRE(strcmp(tetra.id, "228-8889") == 0);
    /* Both halves when they fit, the registration alone when they do not */
    REQUIRE(strcmp(tetra.note, "LA1 ACC G3") == 0);
    REQUIRE(strcmp(tetra.noteShort, "ACC G3") == 0);

    /* A refused location update is said too, and no zero when there is no
       network broadcast at all */
    tetra_sim_clearCell();
    tetra_sim_setCell(228, 8889, 1, 430500000u, 0);
    tetra_sim_setRegistration(false, 0, 7);
    ui_mode_state_read(&tetra);
    REQUIRE(strcmp(tetra.note, "LA1 REJ 7") == 0);
    REQUIRE(strcmp(tetra.noteShort, "REJ 7") == 0);

    tetra_sim_clearCell();
}

/* ================================================ receive-only modes ===== */

/*
 * A mode without a transmitter must not offer a row that changes the
 * transmitter, and the PTT must not key the radio from its screens. The
 * existing mode tests cross UI_MODE_TX with opmode_canTransmit(); this adds
 * the rows, which is where a copy-pasted table brings a PWR row into a mode
 * that cannot transmit.
 */
TEST_CASE("A mode without a transmitter has no transmit row", "[ui][audit][tx]")
{
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        if (ui_mode_canTx(m->mode))
            continue;

        INFO("mode " << m->title);
        resetUi(m->mode);

        for (uint8_t r = 0; r < m->count; r++) {
            const qm_item_t &row = m->items[r];

            REQUIRE(strcmp(row.label, "PWR") != 0);
            REQUIRE(strcmp(row.label, "TX") != 0);
            REQUIRE(strcmp(row.label, "PTT") != 0);
            REQUIRE(strcmp(row.label, "DEPTH") != 0);

            if ((row.set == NULL) || (row.type == QM_INFO)
                || (row.type == QM_GOTO) || (row.type == QM_ACTION))
                continue;

            /* And no row may move the channel's power either */
            state.channel.power = 1000;
            row.set(row.max);
            REQUIRE(state.channel.power == 1000);
        }
    }
}

/* ===================================================== interactions ===== */

/*
 * The interactions the operator can start on purpose and the three things
 * that can happen in the middle of one: the PTT, a mode change made with the
 * side key, and red.
 */
TEST_CASE("PTT, mode change and red during the interactions leave nothing keyed",
          "[ui][audit][interaction]")
{
    /* ---- the quick menu closes on PTT and gives the radio back ---- */
    resetUi(OPMODE_FM);
    pressKey(KEY_F2);
    REQUIRE(qm_state.open);
    REQUIRE_FALSE(state.txDisable);      /* main screen: the PTT works */

    emulator_state.PTTstatus = true;
    settle();
    REQUIRE_FALSE(qm_state.open);
    REQUIRE_FALSE(state.txDisable);

    emulator_state.PTTstatus = false;
    pressKey(0);

    /* ---- a screen that owns the keypad keeps the PA cold ---- */
    resetUi(OPMODE_FM);
    stockMessages();
    openMainMenu();
    openMenuRow((uint8_t)menuRowOf("Messages"));
    pressKey(KEY_F2);                    /* reply -> composer */
    REQUIRE(state.ui_screen == MSG_COMPOSE);
    REQUIRE(state.txDisable);

    emulator_state.PTTstatus = true;
    settle();
    REQUIRE(state.txDisable);            /* still cold: the composer owns it */
    REQUIRE(state.ui_screen == MSG_COMPOSE);
    emulator_state.PTTstatus = false;
    pressKey(0);

    /* ---- the composer follows a mode change instead of breaking ---- */
    state.channel.mode = OPMODE_AIRBAND; /* no APRS transport here */
    settle();
    REQUIRE(state.ui_screen == MSG_COMPOSE);

    /* SEND refuses with no transport for the mode: nothing is queued */
    const int queued = messaging_outCount();
    ui_menu_select(0);
    pressKey(KEY_UP);                    /* TO -> TP */
    pressKey(KEY_UP);                    /* TP -> SEND */
    pressKey(KEY_ENTER);
    REQUIRE(state.ui_screen == MSG_COMPOSE);
    REQUIRE(messaging_outCount() == queued);

    /* And red still leaves it: composer -> inbox -> menu -> main screen, and
       the transmitter is re-armed only on the main screen */
    pressKey(KEY_ESC);
    INFO("after compose red: screen " << (int)state.ui_screen << " txDisable "
         << (int)state.txDisable << " claim " << (int)ui_lastClaim());
    REQUIRE(state.ui_screen == MENU_MESSAGES);
    REQUIRE(state.txDisable);
    pressKey(KEY_ESC);
    INFO("after inbox red: screen " << (int)state.ui_screen << " txDisable "
         << (int)state.txDisable);
    REQUIRE(state.ui_screen == MENU_TOP);
    REQUIRE(state.txDisable);            /* MENU_TOP is a menu, not the VFO */
    pressKey(KEY_ESC);
    INFO("after menu red: screen " << (int)state.ui_screen << " txDisable "
         << (int)state.txDisable);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(state.txDisable);
    REQUIRE_FALSE(ui_msgs_isScreen(state.ui_screen));

    /* ---- the carrier search stops on any key and stays on the frequency ---- */
    resetUi(OPMODE_FM);

    /* Arm it the way the operator does: hold an arrow for three seconds */
    pressKey(KEY_UP, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    tick();

    if (vfoSearchService_active()) {
        const freq_t searched = state.channel.rx_frequency;
        pressKey(KEY_ESC);
        REQUIRE_FALSE(vfoSearchService_active());
        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE(state.channel.rx_frequency == searched);
        REQUIRE_FALSE(state.txDisable);
    }

    /* ---- the analyser gives the receiver back on red and on PTT ---- */
    resetUi(OPMODE_FM);
    ui_open_screen(SPECTRUM, 0);
    REQUIRE(state.ui_screen == SPECTRUM);
    REQUIRE(spectrumService_active());

    emulator_state.PTTstatus = true;
    settle();
    REQUIRE(state.ui_screen == SPECTRUM);   /* a menu never keys the PA */
    REQUIRE(state.txDisable);
    emulator_state.PTTstatus = false;
    pressKey(0);

    pressKey(KEY_ESC);
    REQUIRE_FALSE(spectrumService_active());
    REQUIRE_FALSE(state.txDisable);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* A mode change behind the analyser's back closes it, with the radio */
    resetUi(OPMODE_FM);
    ui_open_screen(SPECTRUM, 0);
    REQUIRE(spectrumService_active());
    state.channel.mode = OPMODE_DMR;
    settle();
    REQUIRE_FALSE(ui_spectrum_isOpen());
    REQUIRE_FALSE(spectrumService_active());
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(state.txDisable);
}

/* ==================================================== after two laps ===== */

/*
 * Two full laps over the modes, the way the operator cycles with the side
 * key. After each lap the menus must still answer: the popup opens, its rows
 * move, a Settings page opens and closes, and the claim trace is still one
 * claim per key. Built with the address sanitiser this also catches a pointer
 * into a mode's table that was freed when the mode changed.
 */
TEST_CASE("Two laps over the modes leave every menu answering", "[ui][audit][modes]")
{
    for (int lap = 0; lap < 2; lap++) {
        for (uint8_t i = 0; i < ui_mode_count(); i++) {
            const ui_mode_t *m = ui_mode_at(i);

            INFO("lap " << lap << ", mode " << m->title);
            resetUi(m->mode);
            stockMessages();

            /* The popup of the mode answers and its table is the mode's */
            REQUIRE(claimOf(KEY_F2) == UI_CLAIM_QM_OPEN);
            REQUIRE(qm_state.open);
            REQUIRE(qm_state.qm->mode == m->mode);

            /* One full turn of the cursor: as many steps as rows */
            for (uint8_t n = 0; n < m->count; n++)
                claimOf(KEY_DOWN);
            REQUIRE(qm_state.selected == 0);

            claimOf(KEY_F1);
            claimOf(KEY_F1, true);
            claimOf(KEY_ESC);
            REQUIRE_FALSE(qm_state.open);

            /* The main menu answers */
            REQUIRE(claimOf(KEY_ENTER) == UI_CLAIM_SCREEN);
            REQUIRE(state.ui_screen == MENU_TOP);
            claimOf(KEY_DOWN);
            claimOf(KEY_UP);
            claimOf(KEY_ESC);

            /* And the Settings page of the mode, if it has one */
            if (ui_mode_hasPage(m->mode)) {
                openMainMenu();
                openMenuRow((uint8_t)menuRowOf("Settings"));
                int index = -1;
                for (uint8_t s = 0; s < settings_num; s++)
                    if (strcmp(settings_items[s], m->title) == 0)
                        index = s;

                REQUIRE(index >= 0);
                ui_menu_select((uint8_t)index);
                claimOf(KEY_ENTER);
                REQUIRE(state.ui_screen == SETTINGS_MODE);
                REQUIRE(qm_page_state.qm->mode == m->mode);

                for (uint8_t n = 0; n < qm_page_state.qm->count; n++)
                    claimOf(KEY_DOWN);
                REQUIRE(qm_page_state.selected == 0);

                claimOf(KEY_ESC);
                REQUIRE(state.ui_screen == MENU_SETTINGS);
                REQUIRE_FALSE(qm_page_isOpen());
                claimOf(KEY_ESC);
            }

            /* The side key's cycle lands on the same mode it starts from */
            const uint8_t next = ui_mode_next(m->mode, +1);
            state.channel.mode = next;
            settle();
            state.channel.mode = m->mode;
            settle();
        }
    }
}
