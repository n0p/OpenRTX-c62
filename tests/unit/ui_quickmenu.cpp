/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contextual quick menu: table selection per mode, navigation, and the value
 * limits every row must respect (a row that can be driven past its min/max is
 * a row that can write garbage into the state).
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "core/state.h"
#include "interfaces/delays.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_hints.h"
#include "core/graphics.h"
}

extern "C" state_t state;

/* Index of a row by label, -1 if the table has none */
static int rowOf(const qm_menu_t *qm, const char *label)
{
    for (uint8_t i = 0; i < qm->count; i++) {
        if (strcmp(qm->items[i].label, label) == 0)
            return i;
    }
    return -1;
}

static void openMode(uint8_t mode)
{
    state.settings = default_settings;
    state.channel.mode = mode;
    state.channel.power = 1000;
    state.channel.bandwidth = BW_25;
    state.channel.fm.txToneEn = 0;
    state.channel.fm.rxToneEn = 0;
    qm_close();
    qm_state.qm = NULL;
    qm_state.selected = 0;
    qm_state.first = 0;
    qm_open();
    REQUIRE(qm_state.open);
    REQUIRE(qm_state.qm != NULL);
}

TEST_CASE("Quick menu has a table per operating mode", "[ui][quickmenu]")
{
    const qm_menu_t *fm = qm_findMenu(OPMODE_FM);
    REQUIRE(fm != NULL);
    REQUIRE(fm->mode == OPMODE_FM);
    REQUIRE(fm->count > 0);
    REQUIRE(std::strcmp(fm->items[0].label, "SQL") == 0);

    /* Every mode participates in the same selector and can close it again */
    REQUIRE(rowOf(fm, "MODE") == fm->count - 1);

    /* No mode specific junk: an unknown mode falls back to FM */
    const qm_menu_t *fallback = qm_findMenu(0x7f);
    REQUIRE(fallback->mode == OPMODE_FM);

#ifdef CONFIG_M17
    const qm_menu_t *m17 = qm_findMenu(OPMODE_M17);
    REQUIRE(m17->mode == OPMODE_M17);
    REQUIRE(rowOf(m17, "DEST") == 0);
#endif

#ifdef CONFIG_TETRA
    const qm_menu_t *tetra = qm_findMenu(OPMODE_TETRA);
    REQUIRE(tetra->mode == OPMODE_TETRA);
    REQUIRE(rowOf(tetra, "AFC") == 0);
#endif

#ifdef CONFIG_DMR
    const qm_menu_t *dmr = qm_findMenu(OPMODE_DMR);
    REQUIRE(dmr->mode == OPMODE_DMR);
    REQUIRE(rowOf(dmr, "CC") == 0);
#endif
}

TEST_CASE("Quick menu navigation wraps and keeps the cursor in range",
          "[ui][quickmenu]")
{
    openMode(OPMODE_FM);
    uint8_t count = qm_state.qm->count;

    REQUIRE(qm_state.selected == 0);

    qm_move(+1);
    REQUIRE(qm_state.selected == 1);

    /* Down from the last row wraps to the first */
    for (uint8_t i = 1; i < count; i++)
        qm_move(+1);
    REQUIRE(qm_state.selected == 0);

    /* Up from the first wraps to the last */
    qm_move(-1);
    REQUIRE(qm_state.selected == count - 1);

    qm_move(+1);
    REQUIRE(qm_state.selected == 0);

    /* A digit past the end is ignored, not a write out of bounds */
    qm_select(count + 3);
    REQUIRE(qm_state.selected == 0);
    qm_select(count - 1);
    REQUIRE(qm_state.selected == count - 1);
}

TEST_CASE("Quick menu values stay inside their limits", "[ui][quickmenu]")
{
    /* Direct setters, the ones the menu and the rows share */
    qm_set_power(0);
    REQUIRE(state.channel.power == 1000);
    qm_set_power(60000);
    REQUIRE(state.channel.power == 5000);

    qm_set_brightness(0);
    REQUIRE(state.settings.brightness >= 5);
    qm_set_brightness(200);
    REQUIRE(state.settings.brightness == 100);

    qm_fm_set_sql(-3);
    REQUIRE(state.settings.sqlLevel == 0);
    qm_fm_set_sql(99);
    REQUIRE(state.settings.sqlLevel == 15);

    /* Fine tuning: +-500 Hz in 10 Hz steps, rounded to the step */
    qm_set_rx_fine(1000);
    REQUIRE(state.settings.rxFineHz == 500);
    qm_set_rx_fine(-1000);
    REQUIRE(state.settings.rxFineHz == -500);
    qm_set_rx_fine(37);
    REQUIRE(state.settings.rxFineHz == 30);
}

TEST_CASE("Quick menu editing clamps at the top and bottom", "[ui][quickmenu]")
{
    openMode(OPMODE_FM);
    int power = rowOf(qm_state.qm, "PWR");
    REQUIRE(power > 0);

    qm_select((uint8_t)power);

    /* P2/P1 change the value of the highlighted row directly: no modal edit
       mode, because the keypad has no arrow keys to drive one. */
    for (int i = 0; i < 20; i++)
        qm_adjust(+1);
    REQUIRE(state.channel.power == 5000);

    for (int i = 0; i < 20; i++)
        qm_adjust(-1);
    REQUIRE(state.channel.power == 1000);

    /* The cursor did not move while adjusting */
    REQUIRE(qm_state.selected == power);

    /* Enumerations wrap instead of clamping */
    int bw = rowOf(qm_state.qm, "BW");
    qm_select((uint8_t)bw);
    state.channel.bandwidth = BW_6_25;
    qm_adjust(+1);
    REQUIRE(state.channel.bandwidth == BW_12_5);
    qm_adjust(-1);
    REQUIRE(state.channel.bandwidth == BW_6_25);

    /* Green never closes the popup, not even on a read-only row */
    qm_close();
    openMode(OPMODE_DMR);
    qm_select(0); /* CC: read only in DMR */
    qm_enter();
    REQUIRE(qm_state.open == true);

    /* ESC is the only key that closes it */
    qm_cancel();
    REQUIRE(qm_state.open == false);
}

TEST_CASE("Quick menu rows fit their line", "[ui][quickmenu][layout]")
{
    /* The reported bug: the selected row's text was drawn on a baseline five
       pixels above the selection rectangle, so the black-on-white text fell
       outside it and only a slice was visible. Pin the geometry. */
    openMode(OPMODE_FM);

    const uint8_t rowH = qm_rowHeight();
    const uint8_t baseline = qm_rowBaseline();
    const uint16_t capHeight = gfx_getFontHeight(FONT_SIZE_5PT);

    REQUIRE(rowH > 0);
    REQUIRE(baseline > 0);
    REQUIRE(capHeight > 0);

    /* TomThumb draws from baseline-capHeight+1 to baseline-1 */
    REQUIRE((int)baseline - (int)capHeight >= 1);
    REQUIRE(baseline <= rowH - 1);

    /* Rows do not overlap the header nor the hint bar */
    uint8_t rows = qm_visibleRows();
    REQUIRE(rows >= 8);
    for (uint8_t r = 0; r < rows; r++) {
        int16_t top = qm_rowTop(r);
        REQUIRE(top >= (int16_t)layout.top_h);
        REQUIRE(top + rowH <= CONFIG_SCREEN_HEIGHT - UI_HINTS_H);
    }

    /* Scroll keeps the selection visible on both ends */
    for (uint8_t i = 0; i < qm_state.qm->count; i++)
        qm_select(i);
    qm_select((uint8_t)(qm_state.qm->count - 1));
    REQUIRE(qm_state.selected >= qm_state.first);
    REQUIRE(qm_state.selected < qm_state.first + rows);
}

TEST_CASE("Quick menu labels and values fit the row width", "[ui][quickmenu][layout]")
{
    openMode(OPMODE_FM);

    /* 5 pt is 4 px per character: the widest realistic value plus the widest
       label must leave the row readable, not overflow it. */
    for (uint8_t i = 0; i < qm_state.qm->count; i++) {
        const qm_item_t *item = &qm_state.qm->items[i];
        char value[QM_VALUE_BUF] = "";
        uint16_t label_w = gfx_getTextWidth(FONT_SIZE_5PT, item->label);
        REQUIRE(label_w + 4 < CONFIG_SCREEN_WIDTH);

        if (qm_value_text(i, value, sizeof(value)) && value[0] != '\0') {
            uint16_t value_w = gfx_getTextWidth(FONT_SIZE_5PT, value);
            REQUIRE(value_w < CONFIG_SCREEN_WIDTH);
        }
    }
}

TEST_CASE("Quick menu latch and timeout", "[ui][quickmenu]")
{
    openMode(OPMODE_FM);
    REQUIRE(qm_state.latched == false);

    /* The timeout only fires after QM_TIMEOUT_MS without keys */
    REQUIRE(qm_tick(qm_state.lastKeyTick + QM_TIMEOUT_MS - 1) == false);
    REQUIRE(qm_state.open == true);
    REQUIRE(qm_tick(qm_state.lastKeyTick + QM_TIMEOUT_MS + 1) == true);
    REQUIRE(qm_state.open == false);

    /* A latched popup stays */
    qm_open();
    qm_toggleLatch();
    REQUIRE(qm_state.latched == true);
    REQUIRE(qm_tick(qm_state.lastKeyTick + 10 * QM_TIMEOUT_MS) == false);
    REQUIRE(qm_state.open == true);
    qm_close();
}

TEST_CASE("Quick menu renders the value text of its rows", "[ui][quickmenu]")
{
    openMode(OPMODE_FM);
    char buf[QM_VALUE_BUF];

    /* Squelch level, plain integer */
    int sql = rowOf(qm_state.qm, "SQL");
    state.settings.sqlLevel = 7;
    REQUIRE(qm_value_text((uint8_t)sql, buf, sizeof(buf)));
    REQUIRE(std::strcmp(buf, "7") == 0);

    /* Power, with its own formatter: watts, not milliwatts */
    int power = rowOf(qm_state.qm, "PWR");
    state.channel.power = 2500;
    REQUIRE(qm_value_text((uint8_t)power, buf, sizeof(buf)));
    REQUIRE(std::strcmp(buf, "2.5W") == 0);

    /* Enum row: the label of the selected value, not its index */
    int bw = rowOf(qm_state.qm, "BW");
    state.channel.bandwidth = BW_6_25;
    REQUIRE(qm_value_text((uint8_t)bw, buf, sizeof(buf)));
    REQUIRE(std::strcmp(buf, "6.25") == 0);

    /* Out of range reads nothing */
    REQUIRE(qm_value_text(qm_state.qm->count, buf, sizeof(buf)) == false);

    /* Closing leaves no table behind: no stale row can be drawn */
    qm_close();
    REQUIRE(qm_value_text(0, buf, sizeof(buf)) == false);
}

TEST_CASE("Quick menu follows a mode change made elsewhere", "[ui][quickmenu]")
{
    openMode(OPMODE_FM);

#ifdef CONFIG_M17
    /* Key 5 changes the mode behind the popup's back */
    state.channel.mode = OPMODE_M17;
    REQUIRE(qm_tick(getTick()) == true);
    REQUIRE(qm_state.qm->mode == OPMODE_M17);
    REQUIRE(qm_state.selected == 0);

    /* No further change: nothing to redraw */
    REQUIRE(qm_tick(getTick()) == false);
#else
    SUCCEED("no second mode on this platform");
#endif

    /* Closing makes the popup invisible to everything else */
    qm_close();
    REQUIRE(qm_state.open == false);
    REQUIRE(qm_tick(getTick()) == false);
}
