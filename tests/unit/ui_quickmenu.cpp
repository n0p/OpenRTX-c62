/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contextual quick menu: the table of each mode, navigation, the value limits
 * every row must respect (a row that can be driven past its min/max is a row
 * that can write garbage into the state), the geometry of the 6 pt layout and
 * the walk over every row of every table that used to leave the popup
 * unusable.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "core/state.h"
#include "interfaces/delays.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_layout.h"
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
    state.ui_screen = MAIN_VFO;
    qm_close();
    qm_state.qm = NULL;
    qm_state.selected = 0;
    qm_state.first = 0;
    layout.top_h = 16;
    qm_open();
    REQUIRE(qm_state.open);
    REQUIRE(qm_state.qm != NULL);
}

/* Every table the build has, with the mode it is shown for */
struct tableRef { uint8_t mode; const qm_menu_t *qm; };

static uint8_t allTables(tableRef *out)
{
    uint8_t n = 0;
    out[n].mode = OPMODE_FM;
    out[n++].qm = qm_findMenu(OPMODE_FM);
#ifdef CONFIG_M17
    out[n].mode = OPMODE_M17;
    out[n++].qm = qm_findMenu(OPMODE_M17);
#endif
#ifdef CONFIG_TETRA
    out[n].mode = OPMODE_TETRA;
    out[n++].qm = qm_findMenu(OPMODE_TETRA);
#endif
#ifdef CONFIG_DMR
    out[n].mode = OPMODE_DMR;
    out[n++].qm = qm_findMenu(OPMODE_DMR);
#endif
#ifdef UI_MODE_NXDN
    out[n].mode = OPMODE_NXDN;
    out[n++].qm = qm_findMenu(OPMODE_NXDN);
#endif
#ifdef UI_MODE_DPMR
    out[n].mode = OPMODE_DPMR;
    out[n++].qm = qm_findMenu(OPMODE_DPMR);
#endif
#ifdef UI_MODE_P25
    out[n].mode = OPMODE_P25;
    out[n++].qm = qm_findMenu(OPMODE_P25);
#endif
#ifdef UI_MODE_YSF
    out[n].mode = OPMODE_YSF;
    out[n++].qm = qm_findMenu(OPMODE_YSF);
#endif
#ifdef UI_MODE_DSTAR
    out[n].mode = OPMODE_DSTAR;
    out[n++].qm = qm_findMenu(OPMODE_DSTAR);
#endif
#ifdef UI_MODE_AIRBAND
    out[n].mode = OPMODE_AIRBAND;
    out[n++].qm = qm_findMenu(OPMODE_AIRBAND);
#endif
#ifdef UI_MODE_WFM
    out[n].mode = OPMODE_WFM_BC;
    out[n++].qm = qm_findMenu(OPMODE_WFM_BC);
#endif
    return n;
}

TEST_CASE("Quick menu has a table per operating mode", "[ui][quickmenu]")
{
    const qm_menu_t *fm = qm_findMenu(OPMODE_FM);
    REQUIRE(fm != NULL);
    REQUIRE(fm->mode == OPMODE_FM);
    REQUIRE(fm->count > 0);
    REQUIRE(std::strcmp(fm->items[0].label, "SQL") == 0);

    /*
     * The mode selector is gone: the side key changes the mode, and a row that
     * rebuilt the table under the finger was one of the ways the popup broke.
     */
    REQUIRE(rowOf(fm, "MODE") == -1);

    /* Sending a message is reachable from every mode's table */
    tableRef tables[16];
    uint8_t n = allTables(tables);

    for (uint8_t t = 0; t < n; t++) {
        int msg = rowOf(tables[t].qm, "MSG");

        REQUIRE(rowOf(tables[t].qm, "MODE") == -1);
        REQUIRE(msg >= 0);
        REQUIRE(tables[t].qm->items[msg].type == QM_GOTO);
        REQUIRE(tables[t].qm->items[msg].goto_screen == MSG_COMPOSE);
    }

    /* The inbox shortcut is still on the FM table */
    REQUIRE(rowOf(fm, "MSGS") >= 0);

#ifdef CONFIG_APRS
    REQUIRE(rowOf(fm, "APRS") >= 0);
#else
    REQUIRE(rowOf(fm, "APRS") == -1);
#endif

    /* No mode specific junk: an unknown mode falls back to FM */
    const qm_menu_t *fallback = qm_findMenu(0x7f);
    REQUIRE(fallback->mode == OPMODE_FM);

#ifdef CONFIG_M17
    const qm_menu_t *m17 = qm_findMenu(OPMODE_M17);
    REQUIRE(m17->mode == OPMODE_M17);
    /* The callsign row is "CALL" and lands on the shared Identity page */
    REQUIRE(rowOf(m17, "CALL") == 0);
    REQUIRE(m17->items[rowOf(m17, "CALL")].goto_screen == SETTINGS_IDENTITY);
    REQUIRE(m17->items[rowOf(m17, "CALL")].goto_item == ID_CALLSIGN);
#endif

#ifdef CONFIG_TETRA
    const qm_menu_t *tetra = qm_findMenu(OPMODE_TETRA);
    REQUIRE(tetra->mode == OPMODE_TETRA);
    REQUIRE(rowOf(tetra, "AFC") == 0);
#endif

#ifdef CONFIG_DMR
    const qm_menu_t *dmr = qm_findMenu(OPMODE_DMR);
    REQUIRE(dmr->mode == OPMODE_DMR);
    /* The colour code and the timeslot are state (main screen), so the first
       row that can be changed is the call destination */
    REQUIRE(rowOf(dmr, "CALL") == 0);
    REQUIRE(rowOf(dmr, "CC") == -1);
    REQUIRE(rowOf(dmr, "TS") == -1);
#endif

#ifdef UI_MODE_NXDN
    const qm_menu_t *nxdn = qm_findMenu(OPMODE_NXDN);
    REQUIRE(nxdn->mode == OPMODE_NXDN);
    REQUIRE(rowOf(nxdn, "RAN") == 0);
#endif

#ifdef UI_MODE_AIRBAND
    const qm_menu_t *airband = qm_findMenu(OPMODE_AIRBAND);
    REQUIRE(airband->mode == OPMODE_AIRBAND);
    REQUIRE(rowOf(airband, "SQL") == 0);
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

    /* A long press uses the row's coarse step: TONE declares 10 */
    int tone = rowOf(qm_state.qm, "TONE");
    qm_select((uint8_t)tone);
    state.channel.fm.txTone = 0;
    qm_adjust_step(+1, true);
    REQUIRE(state.channel.fm.txTone == 10);
    qm_adjust_step(-1, true);
    REQUIRE(state.channel.fm.txTone == 0);

    /* Green never closes the popup, not even on a read-only row */
    qm_close();
    openMode(OPMODE_FM);
    qm_select(0);
    qm_enter();
    REQUIRE(qm_state.open == true);

    /* ESC is the only key that closes it */
    qm_cancel();
    REQUIRE(qm_state.open == false);
}

TEST_CASE("Quick menu rows fit their line", "[ui][quickmenu][layout]")
{
    /*
     * Geometry of the 6 pt popup. The reported bug was the 5 pt layout: the
     * baseline sat one pixel below the row top and the inverted rectangle
     * started one pixel above it, so the black-on-white text fell outside the
     * rectangle and only its bottom slice was visible.
     */
    openMode(OPMODE_FM);

    /* The C62 layout: 16 px top bar, 11 px hints. The host build has the same
       160x128 panel, only layout is not initialised by ui_init() in a test. */
    layout.top_h = 16;

    const uint8_t rowH = qm_rowHeight();
    const uint8_t baseline = qm_rowBaseline();

    REQUIRE(rowH == 12);
    REQUIRE(baseline == 9);

    /* The ink of the row font has to fit inside the row, with air on both
       sides of it */
    REQUIRE((int)baseline - (int)ui_row_ascent() >= 1);
    REQUIRE((int)baseline + (int)ui_row_descent() <= (int)rowH - 1);

    /* Six or seven rows visible: the ask was 6-7, this layout fits 7 */
    uint8_t rows = qm_visibleRows();
    REQUIRE(rows >= 6);
    REQUIRE(rows <= 7);
    REQUIRE(rows == 7);

    for (uint8_t r = 0; r < rows; r++) {
        int16_t top = qm_rowTop(r);
        int16_t ink_top = (int16_t)(top + baseline - ui_row_ascent());
        int16_t ink_bottom = (int16_t)(top + baseline + ui_row_descent());

        REQUIRE(top >= (int16_t)layout.top_h);
        REQUIRE((int16_t)(top + rowH)
                <= (int16_t)(CONFIG_SCREEN_HEIGHT - UI_HINTS_H));
        /* The text is inside its own rectangle, not above it */
        REQUIRE(ink_top > top);
        REQUIRE(ink_bottom < (int16_t)(top + rowH));
    }

    /* The last row does not touch the hint bar */
    REQUIRE((int16_t)(qm_rowTop(rows - 1) + rowH)
            <= (int16_t)(CONFIG_SCREEN_HEIGHT - UI_HINTS_H));

    /* Scroll keeps the selection visible on both ends */
    for (uint8_t i = 0; i < qm_state.qm->count; i++)
        qm_select(i);
    qm_select((uint8_t)(qm_state.qm->count - 1));
    REQUIRE(qm_state.selected >= qm_state.first);
    REQUIRE(qm_state.selected < qm_state.first + rows);

    qm_select(0);
    REQUIRE(qm_state.first == 0);
}

TEST_CASE("Quick menu labels and values fit the row width",
          "[ui][quickmenu][layout]")
{
    tableRef tables[16];
    uint8_t n = allTables(tables);

    openMode(OPMODE_FM);

    for (uint8_t t = 0; t < n; t++) {
        for (uint8_t i = 0; i < tables[t].qm->count; i++) {
            const qm_item_t *item = &tables[t].qm->items[i];
            char value[QM_VALUE_BUF] = "";
            uint16_t label_w = gfx_getTextWidth(QM_FONT, item->label);
            uint16_t value_x = (uint16_t)(QM_TEXT_X + label_w + 6);

            /* Label on the left, at least 40 px left for the value */
            REQUIRE(label_w + QM_TEXT_X < CONFIG_SCREEN_WIDTH);
            REQUIRE(value_x + 40 < CONFIG_SCREEN_WIDTH);

            qm_select(i);
            if (qm_value_text(i, value, sizeof(value)) && value[0] != '\0') {
                uint16_t value_w = gfx_getTextWidth(QM_FONT, value);
                REQUIRE(value_w < CONFIG_SCREEN_WIDTH - QM_VALUE_X);
            }
        }
    }

    qm_close();
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

    /* The side key changes the mode behind the popup's back */
    state.channel.mode = OPMODE_M17;
    REQUIRE(qm_tick(getTick()) == true);
    REQUIRE(qm_state.mode == OPMODE_M17);
    REQUIRE(qm_state.selected == 0);

    /* No further change: nothing to redraw */
    REQUIRE(qm_tick(getTick()) == false);

    /* Closing makes the popup invisible to everything else */
    qm_close();
    REQUIRE(qm_state.open == false);
    REQUIRE(qm_tick(getTick()) == false);
}

TEST_CASE("Quick menu survives a mode with no table of its own",
          "[ui][quickmenu][regression]")
{
    /*
     * Regression: qm_tick() used to compare the table's own mode with the
     * channel mode. A mode without a table falls back to the FM one, whose
     * mode field is OPMODE_FM, so the comparison never matched: the table was
     * rebuilt on every UI pass and the cursor snapped back to the first row as
     * soon as it was moved. That is the "the quick menu stops working with
     * some options" report.
     */
    openMode(OPMODE_FM);
    state.channel.mode = 0x7e;              /* no table for this one */
    REQUIRE(qm_tick(getTick()) == true);    /* rebuild, once */
    REQUIRE(qm_tick(getTick()) == false);   /* and never again */

    qm_state.selected = 0;
    qm_move(+1);
    uint8_t after = qm_state.selected;
    REQUIRE(after == 1);

    /* Ten UI passes must not move the cursor */
    for (int i = 0; i < 10; i++) {
        REQUIRE(qm_tick(getTick()) == false);
        REQUIRE(qm_state.selected == after);
    }

    /* And the table is still there to draw */
    REQUIRE(qm_state.qm != NULL);
    REQUIRE(qm_state.qm->count > 0);
    qm_close();
}

TEST_CASE("Quick menu walks every row of every table",
          "[ui][quickmenu][regression]")
{
    /*
     * The walk the operator does on hardware: open, go down the whole table,
     * change each value both ways, close, open again. Three rounds, because
     * the failure mode was a state that only showed up on the second pass.
     */
    tableRef tables[16];
    uint8_t n = allTables(tables);

    for (uint8_t t = 0; t < n; t++) {
        for (int round = 0; round < 3; round++) {
            openMode(tables[t].mode);
            REQUIRE(qm_state.qm == tables[t].qm);

            uint8_t count = qm_state.qm->count;
            for (uint8_t row = 0; row < count; row++) {
                const qm_item_t *item = &qm_state.qm->items[row];

                qm_select(row);
                REQUIRE(qm_state.selected == row);

                qm_adjust(+1);
                qm_adjust(-1);
                qm_adjust_step(+1, true);
                qm_adjust_step(-1, true);

                /* Nothing here closes the popup or changes the table */
                REQUIRE(qm_state.open == true);
                REQUIRE(qm_state.qm == tables[t].qm);

                /* The value is still inside its own limits */
                if ((item->get != NULL) && (item->type != QM_GOTO)) {
                    int v = item->get();
                    REQUIRE(v >= item->min);
                    REQUIRE(v <= item->max);
                }
            }

            /* Red closes it and the state is clean for the next round */
            qm_cancel();
            REQUIRE(qm_state.open == false);
            REQUIRE(qm_state.qm == NULL);
        }
    }
}

TEST_CASE("Quick menu opens its screens with the right state",
          "[ui][quickmenu][regression]")
{
    /* MSG opens the composer with the transport of the current mode */
    openMode(OPMODE_FM);
    int msg = rowOf(qm_state.qm, "MSG");
    REQUIRE(msg >= 0);
    qm_select((uint8_t)msg);
    state.ui_screen = MAIN_VFO;
    qm_enter();
    REQUIRE(qm_state.open == false);
    REQUIRE(state.ui_screen == MSG_COMPOSE);

    /* MSGS opens the inbox, and the radio does not transmit from there */
    openMode(OPMODE_FM);
    int msgs = rowOf(qm_state.qm, "MSGS");
    REQUIRE(msgs >= 0);
    qm_select((uint8_t)msgs);
    state.ui_screen = MAIN_VFO;
    state.txDisable = false;
    qm_enter();
    REQUIRE(qm_state.open == false);
    REQUIRE(state.ui_screen == MENU_MESSAGES);
    REQUIRE(state.txDisable == true);

    /* Back to a clean main screen for the rest of the suite */
    state.ui_screen = MAIN_VFO;
    state.txDisable = false;
    qm_close();
}
