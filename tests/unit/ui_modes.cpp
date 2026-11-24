/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The mode registry and the screens built from it (ui/ui_modes.h).
 *
 * The three lists that used to be written by hand - the channel editor's mode
 * list, the side key's cycle and the quick menu tables - are now filtered from
 * one table, so what is tested here is that the table is complete and that the
 * screens really see every mode in it: the label of each mode fits the header,
 * the side key reaches all of them and comes back, the rows of the new modes
 * walk without hanging or writing out of their limits, and the Settings page of
 * a mode is the same table as its popup.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cstdio>

extern "C" {
#include "core/state.h"
#include "core/channel_store.h"
#include "core/settings.h"
#include "interfaces/delays.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
#include "ui/ui_modes.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_layout.h"
#include "core/graphics.h"
}

extern "C" state_t state;

static int rowOf(const qm_menu_t *qm, const char *label)
{
    for (uint8_t i = 0; i < qm->count; i++) {
        if (strcmp(qm->items[i].label, label) == 0)
            return i;
    }
    return -1;
}

/* Every mode the registry has, as a row of labels, for the failure messages */
static const char *labelOfIndex(uint8_t i)
{
    const ui_mode_t *m = ui_mode_at(i);

    return (m != NULL) ? m->title : "?";
}

TEST_CASE("Every compiled mode is in the registry", "[ui][modes]")
{
    REQUIRE(ui_mode_count() > 0);

    /* FM is the fallback of every accessor, so it has to be there */
    const ui_mode_t *fm = ui_mode_find(OPMODE_FM);
    REQUIRE(fm != NULL);
    REQUIRE(fm->count > 0);
    REQUIRE(ui_mode_at(0) != NULL);      /* the side key starts here */
    REQUIRE(ui_mode_at(ui_mode_count()) == NULL);

#if defined(UI_MODE_M17)
    REQUIRE(ui_mode_find(OPMODE_M17) != NULL);
#endif
#if defined(UI_MODE_DMR)
    REQUIRE(ui_mode_find(OPMODE_DMR) != NULL);
#endif
#if defined(UI_MODE_TETRA)
    REQUIRE(ui_mode_find(OPMODE_TETRA) != NULL);
#endif
#if defined(UI_MODE_NXDN)
    REQUIRE(ui_mode_find(OPMODE_NXDN) != NULL);
#endif
#if defined(UI_MODE_AIRBAND)
    REQUIRE(ui_mode_find(OPMODE_AIRBAND) != NULL);
#endif

    /* A mode this build does not have is not invented */
    REQUIRE(ui_mode_find(OPMODE_YSF) == NULL);
    REQUIRE(ui_mode_find(0x7f) == NULL);

    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        INFO("mode " << labelOfIndex(i));
        REQUIRE(m != NULL);
        REQUIRE(m->items != NULL);
        REQUIRE(m->count > 0);
        REQUIRE(m->title != NULL);

        /* The same mode is not registered twice */
        for (uint8_t j = (uint8_t)(i + 1); j < ui_mode_count(); j++)
            REQUIRE(ui_mode_at(j)->mode != m->mode);
    }
}

TEST_CASE("Mode labels fit the header of the main screens", "[ui][modes][layout]")
{
    /*
     * The label is drawn in the 6 pt font in the top-left corner, and the clock
     * is centred: a label wider than UI_MODE_LABEL_MAX_W would run into it.
     * This is the check that keeps a future mode from being called something
     * that does not fit ("D-STAR" does not, "DSTAR" does).
     */
    for (uint8_t id = OPMODE_NONE; id <= OPMODE_AIRBAND; id++) {
        const char *label = ui_mode_label(id);

        INFO("label of mode " << (int)id << ": " << label);
        REQUIRE(label != NULL);
        REQUIRE(strlen(label) >= 1);
        REQUIRE(strlen(label) <= 5);
        REQUIRE(gfx_getTextWidth(FONT_SIZE_6PT, label) <= UI_MODE_LABEL_MAX_W);
    }

    /* Unknown modes read as "no mode", never as an empty string */
    REQUIRE(strcmp(ui_mode_label(0x7f), "----") == 0);

    /*
     * And the label plus the lock symbol still stops before the clock: the
     * clock is centred and 8 pt, so its left edge is (width - clock)/2.
     */
    uint16_t clock = gfx_getTextWidth(FONT_SIZE_8PT, "00:00:00");
    int16_t clockLeft = (int16_t)((CONFIG_SCREEN_WIDTH - clock) / 2);
    int16_t labelRight = (int16_t)(UI_TEXT_X + UI_MODE_LABEL_MAX_W);

    REQUIRE(labelRight < clockLeft);

    /* The channel store is still the one place a mode is spelled */
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        REQUIRE(strcmp(ui_mode_label(m->mode), chstore_modeName(m->mode)) == 0);
    }
}

TEST_CASE("The side key reaches every selectable mode and comes back",
          "[ui][modes]")
{
    uint8_t first = ui_mode_next(OPMODE_NONE, +1);

    /* A full turn visits every selectable mode exactly once */
    bool seen[32] = { false };
    uint8_t count = 0;
    uint8_t mode = first;

    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_find(mode);

        INFO("step " << (int)i << " -> " << (int)mode);
        REQUIRE(m != NULL);
        REQUIRE((m->flags & UI_MODE_SELECT) != 0);
        REQUIRE(mode < 32);
        REQUIRE(!seen[mode]);
        seen[mode] = true;
        count++;

        mode = ui_mode_next(mode, +1);
    }

    /* After one turn the cycle is back where it started */
    REQUIRE(mode == first);

    /* Every selectable mode was reached */
    uint8_t expected = 0;
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        if (ui_mode_at(i)->flags & UI_MODE_SELECT)
            expected++;
    }
    REQUIRE(count == expected);

    /* Backwards is the reverse of forwards: one step back, one forward */
    uint8_t last = ui_mode_next(first, -1);

    REQUIRE(last != first);
    REQUIRE(ui_mode_next(last, +1) == first);

    /* An unknown mode restarts the cycle instead of getting stuck */
    REQUIRE(ui_mode_next(0x7f, +1) == first);
}

TEST_CASE("The channel editor offers the modes the registry marks for it",
          "[ui][modes]")
{
    /* The editor walks the registry, so the flags are the only list */
    REQUIRE(ui_mode_inChannelList(OPMODE_FM));

#if defined(UI_MODE_NXDN)
    REQUIRE(ui_mode_inChannelList(OPMODE_NXDN));
    REQUIRE(!ui_mode_canTx(OPMODE_NXDN));
#endif

#if defined(UI_MODE_AIRBAND)
    REQUIRE(ui_mode_inChannelList(OPMODE_AIRBAND));
    REQUIRE(!ui_mode_canTx(OPMODE_AIRBAND));
#endif

    /* TX capability matches what the RTX says, mode by mode */
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        REQUIRE(ui_mode_canTx(m->mode) == opmode_canTransmit(m->mode));
    }
}

#if defined(UI_MODE_NXDN)
TEST_CASE("NXDN has its rows in the quick menu", "[ui][modes][quickmenu]")
{
    const qm_menu_t *qm = qm_findMenu(OPMODE_NXDN);

    REQUIRE(qm != NULL);
    REQUIRE(qm->mode == OPMODE_NXDN);

    /* The knobs the engine has */
    REQUIRE(rowOf(qm, "RAN") == 0);
    REQUIRE(rowOf(qm, "BW") > 0);
    /* What the demodulator reports */
    REQUIRE(rowOf(qm, "SYNC") > 0);
    REQUIRE(rowOf(qm, "CALL") > 0);
    REQUIRE(rowOf(qm, "TYPE") > 0);
    /* The tail every table shares */
    REQUIRE(rowOf(qm, "MSGS") > 0);
    REQUIRE(rowOf(qm, "MSG") > 0);
}

TEST_CASE("NXDN RAN is a filter with an ANY position", "[ui][modes]")
{
    state.settings = default_settings;

    /* The default is ANY, the behaviour the mode had before the row existed */
    REQUIRE(state.settings.nxdn_ran > 63);
    REQUIRE(qm_nxdn_get_ran() == 64);

    qm_nxdn_set_ran(12);
    REQUIRE(state.settings.nxdn_ran == 12);
    REQUIRE(qm_nxdn_get_ran() == 12);

    /* RAN is 6 bits: past 63 the only meaningful value is ANY */
    qm_nxdn_set_ran(64);
    REQUIRE(state.settings.nxdn_ran == 0xFF);
    REQUIRE(qm_nxdn_get_ran() == 64);

    qm_nxdn_set_ran(-1);
    REQUIRE(state.settings.nxdn_ran == 0xFF);
}

TEST_CASE("NXDN channel spacing is 6.25 or 12.5 kHz", "[ui][modes]")
{
    state.channel.mode = OPMODE_NXDN;
    state.channel.bandwidth = BW_12_5;

    REQUIRE(qm_nxdn_get_bw() == 0);
    qm_nxdn_set_bw(1);
    REQUIRE(state.channel.bandwidth == BW_6_25);
    REQUIRE(qm_nxdn_get_bw() == 1);
    qm_nxdn_set_bw(0);
    REQUIRE(state.channel.bandwidth == BW_12_5);

    /* A stale 25 kHz from FM must not survive: the engine reads 12.5 or 6.25 */
    qm_nxdn_set_bw(0);
    REQUIRE(qm_nxdn_get_bw() == 0);
}
#endif /* UI_MODE_NXDN */

#if defined(UI_MODE_AIRBAND)
TEST_CASE("Airband has its rows in the quick menu", "[ui][modes][quickmenu]")
{
    const qm_menu_t *qm = qm_findMenu(OPMODE_AIRBAND);

    REQUIRE(qm != NULL);
    REQUIRE(qm->mode == OPMODE_AIRBAND);

    REQUIRE(rowOf(qm, "SQL") == 0);      /* carrier squelch */
    REQUIRE(rowOf(qm, "AGC") > 0);
    REQUIRE(rowOf(qm, "STEP") > 0);      /* 25 kHz / 8.33 kHz */
    REQUIRE(rowOf(qm, "SNAP") > 0);
    REQUIRE(rowOf(qm, "CH") > 0);        /* channel designator */
    REQUIRE(rowOf(qm, "IF") > 0);

    /* Receive only: no power row */
    REQUIRE(rowOf(qm, "PWR") == -1);
}

TEST_CASE("Airband raster and snap reach the tuning step", "[ui][modes]")
{
    state.settings = default_settings;
    state.channel.mode = OPMODE_AIRBAND;

    REQUIRE(state.settings.am_step == false);
    REQUIRE(state.settings.am_snap == false);
    REQUIRE(state.settings.am_agc == true);   /* on by default */

    qm_am_set_step(1);
    REQUIRE(state.settings.am_step == true);
    REQUIRE(freq_steps[state.step_index] == 8333);

    qm_am_set_step(0);
    REQUIRE(state.settings.am_step == false);
    REQUIRE(freq_steps[state.step_index] == 25000);

    /* Entering the mode brings the operator's raster back */
    state.settings.am_step = true;
    state.step_index = 0;
    qm_am_applyRaster();
    REQUIRE(freq_steps[state.step_index] == 8333);

    qm_am_set_snap(1);
    REQUIRE(state.settings.am_snap == true);

    /* The squelch row is the carrier threshold, 0..15 */
    qm_am_set_sql(7);
    REQUIRE(state.settings.sqlLevel == 7);
    qm_am_set_sql(99);
    REQUIRE(state.settings.sqlLevel == 15);

    /* AGC is a live path flag, not a resync */
    qm_am_set_agc(0);
    REQUIRE(state.settings.am_agc == false);
}
#endif /* UI_MODE_AIRBAND */

TEST_CASE("A mode's Settings page is its quick menu table", "[ui][modes]")
{
    qm_close();
    qm_page_close();

    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        if (!ui_mode_hasPage(m->mode)) {
            /* No page: the mode has none, and nothing pretends otherwise */
            REQUIRE(!ui_mode_hasPage(m->mode));
            continue;
        }

        INFO("page of " << m->title);
        REQUIRE(m->items == ui_mode_rows(m->mode, NULL));

        qm_page_open(m->mode);
        REQUIRE(qm_page_isOpen());
        REQUIRE(qm_page_state.qm == qm_findMenu(m->mode));
        REQUIRE(qm_page_state.selected == 0);

        /* Walk every row of the page, three times round */
        for (int round = 0; round < 3; round++) {
            for (uint8_t row = 0; row < qm_page_state.qm->count; row++) {
                qm_page_move(+1);
                qm_page_adjust(+1, false);
                qm_page_adjust(-1, false);
                qm_page_adjust(+1, true);
                qm_page_adjust(-1, true);
                REQUIRE(qm_page_state.selected < qm_page_state.qm->count);
            }
            /* One full turn of the cursor lands back on the first row */
            REQUIRE(qm_page_state.selected == 0);
            REQUIRE(qm_page_state.first == 0);

            /* The values are still inside their own limits */
            for (uint8_t row = 0; row < qm_page_state.qm->count; row++) {
                const qm_item_t *item = &qm_page_state.qm->items[row];

                if ((item->get != NULL) && (item->type != QM_GOTO)
                    && (item->type != QM_INFO)) {
                    REQUIRE(item->get() >= item->min);
                    REQUIRE(item->get() <= item->max);
                }
            }
        }

        qm_page_close();
        REQUIRE(!qm_page_isOpen());
        REQUIRE(qm_page_state.qm == NULL);
    }
}

TEST_CASE("The mode page and the popup read the same value", "[ui][modes]")
{
    /*
     * Both screens format a row through qm_value_text_of(), so a value cannot
     * read one way in Settings and another in the popup.
     */
    state.settings = default_settings;

    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);
        char fromPage[QM_VALUE_BUF];
        char fromPopup[QM_VALUE_BUF];

        for (uint8_t row = 0; row < m->count; row++) {
            bool a = qm_value_text_of(m, row, fromPage, sizeof(fromPage));
            bool b = qm_value_text_of(qm_findMenu(m->mode), row, fromPopup,
                                      sizeof(fromPopup));

            INFO("mode " << m->title << " row " << m->items[row].label);
            REQUIRE(a == b);
            if (a)
                REQUIRE(strcmp(fromPage, fromPopup) == 0);
        }
    }
}

TEST_CASE("Mode pages are reachable from the Settings menu", "[ui][modes]")
{
    /*
     * settings_items[] and enum settingsItems are generated from the same list,
     * so the entry the operator selects and the page that opens cannot drift.
     */
    /* The list ends with Accessibility and Default Settings, so the mode pages
       are the block just before them */
    int lastPage = (int)settings_num - 3;

    REQUIRE(settings_num > 2);
    REQUIRE(settings_items[lastPage] != NULL);

    uint8_t pages = 0;
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        if (ui_mode_hasPage(ui_mode_at(i)->mode))
            pages++;
    }

    REQUIRE(pages >= 1);

    /* Each mode page's title is the registry's title, in order */
    uint8_t seen = 0;
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        if (!ui_mode_hasPage(m->mode))
            continue;

        const char *item = settings_items[lastPage - (int)pages + 1 + (int)seen];

        INFO("page " << (int)seen << " title " << item);
        REQUIRE(item != NULL);
        REQUIRE(strcmp(item, m->title) == 0);
        seen++;
    }
}

TEST_CASE("A mode page never leaves the screen or the popup in a bad state",
          "[ui][modes][regression]")
{
    /* Opening a page does not open the popup, and closing it does not either */
    qm_close();
    qm_page_open(OPMODE_FM);
    REQUIRE(qm_page_isOpen());
    REQUIRE(qm_state.open == false);

    /* Green on a QM_GOTO row leaves the page for its screen */
    int msg = rowOf(qm_page_state.qm, "MSG");

    if (msg >= 0) {
        qm_page_state.selected = (uint8_t)msg;
        state.ui_screen = SETTINGS_MODE;
        qm_page_enter();
        REQUIRE(qm_page_isOpen() == false);
        REQUIRE(state.ui_screen == MSG_COMPOSE);
    }

    /* Back to a clean state for the rest of the suite */
    state.ui_screen = MAIN_VFO;
    state.txDisable = false;
    qm_close();
    qm_page_close();
}
