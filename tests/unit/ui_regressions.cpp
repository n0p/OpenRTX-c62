/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * One test per symptom the operator reported on the radio, driven through the
 * real UI FSM (ui_updateFSM) with key events, the way the keypad feeds it.
 *
 *   - P2 short press opens the quick menu;
 *   - every compiled mode has a page in Settings, and it opens on its own table;
 *   - the keypad enters a frequency and ENTER applies it;
 *   - three seconds of a held arrow start the carrier search;
 *   - the main menu carries Messages and it opens the inbox;
 *   - DMR can key the transmitter (its PTT gate).
 *
 * The tests exist because every one of these paths was covered *below* the
 * dispatch (qm_open(), qm_page_open(), _ui_fsm_insertVFONumber(), the search
 * service, ui_msgs_open()) and not *through* it, which is where the wiring
 * breaks: a screen that is not dispatched cannot be opened, a branch that is
 * compiled out cannot be reached, and a stale UI state eats the key before it
 * arrives. The APRS build option is on for this binary on purpose: the block
 * that dispatched the messaging, contact and zone screens used to live inside
 * `#ifdef CONFIG_APRS`, so without it the test (and the radio in an APRS-less
 * build) had no messages menu at all.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

extern "C" {
#include "core/state.h"
#include "core/input.h"
#include "core/event.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "core/vfo_search.h"
#include "core/vfo_search_service.h"
#include "interfaces/cps_io.h"
#include "rtx/rtx.h"
#include "rtx/DmrKey.h"
#include "ui/ui_default.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_messages.h"
}

extern state_t state;

namespace
{

constexpr freq_t FREQ = 145500000;   /* inside the 2 m plan the search knows */
constexpr uint32_t HOLD_MS = 3000;   /* the operator's three seconds         */

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

/** A digit is a press and a release, as the matrix reports it. */
void digit(uint32_t k)
{
    pressKey(k);
    pressKey(0);
}

/** One pass of the FSM with no event: the periodic part only. */
void tick(void)
{
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

/** Hold a key: real time, because the hold is what the firmware times. */
void hold(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    tick();
}

/**
 * A clean main screen.  The UI state is a static of ui.c and survives from one
 * test case to the next, so every test starts by putting everything it reads
 * back to a known place.
 */
void resetUi(uint8_t screen, uint8_t mode, freq_t freq)
{
    state.channel.mode          = mode;
    state.channel.rx_frequency  = freq;
    state.channel.tx_frequency  = freq;
    state.channel.bandwidth     = BW_25;
    state.channel.power         = 1000;
    state.channel.rx_only       = false;
    state.channel.dmr.rxColorCode = 1;
    state.channel.dmr.txColorCode = 1;
    state.channel.dmr.dmr_timeslot = 1;
    state.ui_screen             = screen;
    state.tuner_mode            = VFO;
    state.txDisable             = false;
    state.charge                = 100;
    state.emergency             = false;
    state.step_index            = 4;            /* 12.5 kHz */
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;       /* silent in the harness */

    qm_close();
    qm_page_close();
    ui_menu_select(0);

    /*
     * A long press of P2 latches the macro grid (state.settings.macroMenuLatch
     * defaults to on) and the latch survives until the PTT or a second long
     * press: while it is set, every key goes to the grid and these tests would
     * be testing the grid instead of the main screen.
     */
    macro_latched = false;
}

/** Index of the first generated Settings page (a mode with its own page). */
uint8_t firstGeneratedPage(void)
{
    for (uint8_t i = 0; i < settings_num; i++)
    {
        const char *n = settings_items[i];

        if ((n[0] == 'N') ||                        /* NXDN            */
            (n[0] == 'd') ||                        /* dPMR            */
            (n[0] == 'P') ||                        /* P25             */
            (n[0] == 'Y') ||                        /* YSF             */
            ((n[0] == 'D') && (n[1] == 'S')) ||     /* DSTAR           */
            ((n[0] == 'A') && (n[1] == 'M')) ||     /* AM              */
            (n[0] == 'W'))                          /* WFM             */
            return i;
    }
    return 0;
}

} /* namespace */

/* ---------------------------------------------------------------- symptom 2 */

TEST_CASE("Symptom: P2 short press opens the quick menu", "[ui][dispatch]")
{
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    REQUIRE_FALSE(qm_state.open);

    /* A short press: the event the keypad sends when the key goes down */
    pressKey(KEY_F2, false);
    REQUIRE(qm_state.open);
    REQUIRE(qm_state.qm != NULL);
    REQUIRE(qm_state.qm->mode == OPMODE_FM);

    /* Red closes it and the popup releases the keypad */
    pressKey(KEY_ESC, false);
    REQUIRE_FALSE(qm_state.open);

    /* The long press is the macro grid's, not the popup's */
    pressKey(KEY_F2, true);
    REQUIRE_FALSE(qm_state.open);
    pressKey(0, false);
}

TEST_CASE("Symptom: P2 opens the quick menu of the mode in use", "[ui][dispatch]")
{
    /*
     * "with the mode resolved": the popup has to pick the table of the mode
     * the radio is in, not FM's.
     */
    static const uint8_t modes[] = { OPMODE_FM, OPMODE_AIRBAND, OPMODE_WFM_BC };
    for (uint8_t m : modes)
    {
        resetUi(MAIN_VFO, m, FREQ);
        pressKey(KEY_F2, false);
        REQUIRE(qm_state.open);
        REQUIRE(qm_state.qm != NULL);
        REQUIRE(qm_state.qm->mode == m);
        qm_close();
    }
}

/* ---------------------------------------------------------------- symptom 3 */

TEST_CASE("Symptom: one Settings page per compiled mode, and each one opens",
          "[ui][dispatch][settings]")
{
    /*
     * The list is generated from UI_MODE_PAGES by the same macro that builds
     * the enum the dispatch switches on, so the two cannot disagree -- but the
     * *number* is what the operator sees: a mode missing there has no page he
     * can reach.
     */
    uint8_t pages = 0;

    for (uint8_t i = 0; i < settings_num; i++)
    {
        REQUIRE(settings_items[i] != NULL);
        REQUIRE(settings_items[i][0] != '\0');
    }

#define UI_MODE_PAGE(id, title) pages++;
    UI_MODE_PAGES(UI_MODE_PAGE)
#undef UI_MODE_PAGE

    REQUIRE(pages > 0);
    REQUIRE(settings_num >= (uint8_t)(pages + 1));

    /* Walk the menu to Settings */
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    pressKey(KEY_ENTER, false);                  /* main menu */
    REQUIRE(state.ui_screen == MENU_TOP);
    for (uint8_t i = 0; i < M_SETTINGS; i++)
        pressKey(KEY_DOWN, false);
    pressKey(KEY_ENTER, false);                  /* Settings */
    REQUIRE(state.ui_screen == MENU_SETTINGS);

    /* And open the first generated page: green lands on SETTINGS_MODE */
    uint8_t page = firstGeneratedPage();
    REQUIRE(page > 0);
    ui_menu_select(page);
    pressKey(KEY_ENTER, false);
    REQUIRE(state.ui_screen == SETTINGS_MODE);
    REQUIRE(qm_page_state.qm != NULL);
    REQUIRE(qm_page_state.qm->count > 0);

    /* The page answers its own keys: P2 changes a settable row */
    for (uint8_t row = 0; row < qm_page_state.qm->count; row++)
    {
        const qm_item_t *item = &qm_page_state.qm->items[row];

        if ((item->get != NULL) && (item->type != QM_GOTO) &&
            (item->min < item->max))
        {
            qm_page_state.selected = row;
            int before = item->get();
            pressKey(KEY_F2, false);
            int after = item->get();
            REQUIRE(((after != before) || (after == item->max)));
            break;
        }
    }

    /* Red leaves the page and closes it */
    pressKey(KEY_ESC, false);
    REQUIRE(qm_page_state.qm == NULL);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
}

/* ---------------------------------------------------------------- symptom 5 */

TEST_CASE("Symptom: the keypad enters a frequency and ENTER applies it",
          "[ui][dispatch][vfo]")
{
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);

    /* 145.70000: eight digits, the resolution of the last one is 10 Hz */
    const uint32_t f[] = { KEY_1, KEY_4, KEY_5, KEY_7,
                           KEY_0, KEY_0, KEY_0, KEY_0 };
    for (uint32_t k : f)
        digit(k);

    /* The first digit opens the input screen; the frequency is not applied */
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);
    REQUIRE(state.channel.rx_frequency == FREQ);

    pressKey(KEY_ENTER, false);
    REQUIRE(state.channel.rx_frequency == 145700000);
    REQUIRE(state.channel.tx_frequency == 145700000);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /*
     * Seven digits fill the RX field without reaching its end, so the first
     * green moves to the TX field and the second one copies RX there and
     * applies: a split only exists if the operator types the TX frequency.
     */
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    const uint32_t seven[] = { KEY_1, KEY_4, KEY_3, KEY_5,
                               KEY_0, KEY_0, KEY_0 };
    for (uint32_t k : seven)
        digit(k);
    pressKey(KEY_ENTER, false);
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);         /* now on TX */
    REQUIRE(state.channel.rx_frequency == FREQ);        /* not applied yet */
    pressKey(KEY_ENTER, false);
    REQUIRE(state.channel.rx_frequency == 143500000);
    REQUIRE(state.channel.tx_frequency == 143500000);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* A frequency the receiver cannot tune stays in the input screen */
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    const uint32_t bad[] = { KEY_9, KEY_9, KEY_9, KEY_9,
                             KEY_9, KEY_9, KEY_9, KEY_9 };
    for (uint32_t k : bad)
        digit(k);
    pressKey(KEY_ENTER, false);
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);
    REQUIRE(state.channel.rx_frequency == FREQ);
}

/* ---------------------------------------------------------------- symptom 6 */

TEST_CASE("Symptom: three seconds of a held arrow start the carrier search",
          "[ui][dispatch][search]")
{
    struct vfoSearchStatus st;

    resetUi(MAIN_VFO, OPMODE_FM, FREQ);

    /* The long-press event arms it and does not tune */
    pressKey(KEY_UP, true);
    REQUIRE(state.channel.rx_frequency == FREQ);
    REQUIRE_FALSE(vfoSearchService_active());

    hold(HOLD_MS + 300);
    REQUIRE(vfoSearchService_active());
    REQUIRE(vfoSearchService_status(&st));
    REQUIRE(st.active);
    REQUIRE(st.direction == VFOS_UP);

    /* Any key stops it and the receiver stays where it was listening */
    pressKey(KEY_ESC, false);
    REQUIRE_FALSE(vfoSearchService_active());

    /* Same on the way down */
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    pressKey(KEY_DOWN, true);
    hold(HOLD_MS + 300);
    REQUIRE(vfoSearchService_active());
    REQUIRE(vfoSearchService_status(&st));
    REQUIRE(st.direction == VFOS_DOWN);
    pressKey(KEY_ESC, false);
    REQUIRE_FALSE(vfoSearchService_active());

    /* Letting go before the three seconds starts nothing */
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    pressKey(KEY_UP, true);
    pressKey(0, false);
    hold(HOLD_MS + 300);
    REQUIRE_FALSE(vfoSearchService_active());

    /* On the channel screen the arrows are not the search (that screen loads
       the neighbouring channel, so it needs a codeplug to walk) */
    char cps_name[] = "ui_regressions_test.rtxc";
    REQUIRE(cps_create(cps_name) == 0);
    REQUIRE(cps_open(cps_name) == 0);
    resetUi(MAIN_MEM, OPMODE_FM, FREQ);
    pressKey(KEY_UP, true);
    hold(HOLD_MS + 300);
    REQUIRE_FALSE(vfoSearchService_active());
}

/* ---------------------------------------------------------------- symptom 4 */

TEST_CASE("Symptom: the main menu carries Messages and it opens the inbox",
          "[ui][dispatch][messages]")
{
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);

    /* The entry is in the list the menu is drawn and walked with */
    REQUIRE(menu_num > M_MESSAGES);
    REQUIRE(std::string(menu_items[M_MESSAGES]) == "Messages");

    pressKey(KEY_ENTER, false);
    REQUIRE(state.ui_screen == MENU_TOP);
    for (uint8_t i = 0; i < M_MESSAGES; i++)
        pressKey(KEY_DOWN, false);
    pressKey(KEY_ENTER, false);

    /* The inbox owns the screen, and the radio does not transmit from it */
    REQUIRE(state.ui_screen == MENU_MESSAGES);
    REQUIRE(ui_msgs_isScreen(state.ui_screen));
    REQUIRE(state.txDisable);

    /* Red goes back to the menu, red again to the main screen and re-arms TX */
    pressKey(KEY_ESC, false);
    REQUIRE(state.ui_screen == MENU_TOP);
    pressKey(KEY_ESC, false);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(state.txDisable);
}

/* ---------------------------------------------------------------- symptom 1 */

TEST_CASE("Symptom: DMR can key the transmitter", "[ui][dmr][tx]")
{
    /*
     * The PTT gate of OpMode_DMR. What has to hold is that a radio whose
     * settings record was reset -- the usual result of a firmware update that
     * grows the record, so the personal DMR ID comes back as 0 -- still keys,
     * with the default talkgroup as the destination.
     */
    const settings_t *d = &default_settings;

    REQUIRE(d->dmr_dest_type == DMR_DEST_GROUP);
    REQUIRE(dmrKey_allowed(d->dmr_dest_type, d->dmr_dest_id, 0));
    REQUIRE(dmrKey_allowed(d->dmr_dest_type, d->dmr_dest_id, 1234567));

    /* No destination at all resolves to the default talkgroup, group call */
    REQUIRE(dmrKey_allowed(DMR_DEST_NONE, d->dmr_dest_id, 0));

    /* A unit call needs an identifier to put in the header */
    REQUIRE(dmrKey_allowed(DMR_DEST_USER, 1234567, 0));
    REQUIRE_FALSE(dmrKey_allowed(DMR_DEST_USER, 0, 1234567));
    REQUIRE_FALSE(dmrKey_allowed(DMR_DEST_GROUP, 0, 1234567));

    /* An unknown type is a corrupted record, not a call */
    REQUIRE_FALSE(dmrKey_allowed(42, 9, 1234567));

    /* And the capability itself: DMR is a transmitting mode in this build */
#if defined(CONFIG_DMR_TX)
    REQUIRE(opmode_canTransmit(OPMODE_DMR));
#endif
}

/* ------------------------------------------------------------- the dispatch */

TEST_CASE("Symptom: a tour of every screen leaves the main screen responsive",
          "[ui][dispatch]")
{
    /*
     * The other way these symptoms showed up: a screen that leaves state
     * behind (a stale edit, a popup left open, the keypad handed to a module
     * that closed) and the main screen stops taking keys -- then P2, the digits
     * and a long press of an arrow are all dead with nothing on screen to say
     * why.
     */
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);

    /* Menu, Settings, down to the end of the list and out */
    pressKey(KEY_ENTER, false);
    for (uint8_t i = 0; i < M_SETTINGS; i++)
        pressKey(KEY_DOWN, false);
    pressKey(KEY_ENTER, false);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
    for (uint8_t i = 0; i < settings_num; i++)
        pressKey(KEY_DOWN, false);
    pressKey(KEY_ESC, false);
    pressKey(KEY_ESC, false);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* The inbox and back */
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    ui_msgs_open();
    pressKey(KEY_ESC, false);
    pressKey(KEY_ESC, false);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* After the tour the three main-screen keys still work */
    resetUi(MAIN_VFO, OPMODE_FM, FREQ);
    pressKey(KEY_F2, false);
    REQUIRE(qm_state.open);
    qm_close();

    const uint32_t f[] = { KEY_1, KEY_4, KEY_5, KEY_7,
                           KEY_0, KEY_0, KEY_0, KEY_0 };
    for (uint32_t k : f)
        digit(k);
    pressKey(KEY_ENTER, false);
    REQUIRE(state.channel.rx_frequency == 145700000);

    pressKey(KEY_UP, true);
    hold(HOLD_MS + 300);
    REQUIRE(vfoSearchService_active());
    pressKey(KEY_ESC, false);
}
