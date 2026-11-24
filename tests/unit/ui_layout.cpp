/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The one geometry of the list screens (ui/ui_layout.h), on the 160x128 panel:
 * the header band, the 6 pt row and the scroll window every list shares.
 */

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "core/graphics.h"
#include "ui/ui_default.h"
#include "ui/ui_layout.h"
#include "ui/ui_hints.h"
}

TEST_CASE("List geometry comes from the 6 pt metric", "[ui][layout]")
{
    /* UbuntuRegular6pt7b: cap from baseline-7 to baseline-1, descender +1 */
    REQUIRE(ui_row_ascent() == 7);
    REQUIRE(ui_row_descent() == 2);
    REQUIRE(ui_row_height() == 12);
    REQUIRE(ui_row_baseline() == 9);

    /* The ink fits in the row with air above and below */
    REQUIRE(ui_row_baseline() > ui_row_ascent());
    REQUIRE((int)ui_row_baseline() + (int)ui_row_descent()
            < (int)ui_row_height());

    /* The header band holds the 8 pt title (ascent 10) and the rule */
    REQUIRE(UI_HEADER_BASELINE - 10 >= 0);
    REQUIRE(UI_HEADER_BASELINE <= UI_HEADER_RULE_Y);
    REQUIRE(UI_HEADER_RULE_Y < UI_HEADER_H);
}

TEST_CASE("Lists fit their rows above the hint bar", "[ui][layout]")
{
    /* Menus and quick menu: 12 px rows */
    uint8_t menus = ui_rows_fit(ui_row_height());
    REQUIRE(menus == 8);

    /* Channel list and zone entries with two-line rows (20 px): 5 */
    REQUIRE(ui_rows_fit(20) == 5);

    /* Every row of a full list stays above the hints */
    for (uint8_t row = 0; row < menus; row++) {
        int16_t top = ui_row_top(row);

        REQUIRE(top >= ui_list_top());
        REQUIRE((int16_t)(top + ui_row_height())
                <= (int16_t)(CONFIG_SCREEN_HEIGHT - UI_HINTS_H));
    }

    /* The next one would not: that is what the scroll window is for */
    REQUIRE((int16_t)(ui_row_top(menus) + ui_row_height())
            > (int16_t)(CONFIG_SCREEN_HEIGHT - UI_HINTS_H));
}

TEST_CASE("Row baselines centre the text at any row height", "[ui][layout]")
{
    /* 12 px: the 6 pt baseline lands at +9, the same as the quick menu */
    REQUIRE(ui_row_baseline_at(0, 12) == 9);
    REQUIRE(ui_row_baseline_at(20, 12) == 29);

    /* 10 px (the old message row) is the tightest that still holds the ink */
    int16_t b10 = ui_row_baseline_at(0, 10);
    REQUIRE(b10 == 7);
    REQUIRE(b10 - ui_row_ascent() >= 0);
    REQUIRE(b10 + ui_row_descent() <= 9);

    /* 16 px (contacts editor): the text does not stick to the top */
    int16_t b16 = ui_row_baseline_at(0, 16);
    REQUIRE(b16 > ui_row_ascent());
    REQUIRE(b16 + ui_row_descent() < 16);

    /* A taller row never puts the baseline higher than a shorter one */
    REQUIRE(ui_row_baseline_at(0, 20) >= ui_row_baseline_at(0, 12));
}

TEST_CASE("Every list scrolls the same way", "[ui][layout]")
{
    uint16_t sel = 0;
    uint16_t first = 0;

    /* Empty list: both indices stay at zero */
    ui_list_clamp(&sel, &first, 0, 8);
    REQUIRE(sel == 0);
    REQUIRE(first == 0);

    /* Selection past the end is pulled back to the last entry */
    sel = 40;
    first = 0;
    ui_list_clamp(&sel, &first, 10, 8);
    REQUIRE(sel == 9);
    REQUIRE(first <= sel);
    REQUIRE(sel < first + 8);

    /* Walking down scrolls one row at a time and never leaves a blank line
       above the last entries */
    first = 0;
    for (uint16_t i = 0; i < 10; i++) {
        sel = i;
        ui_list_clamp(&sel, &first, 10, 8);
        REQUIRE(sel < first + 8);
        REQUIRE(first <= sel);
        REQUIRE((first + 8 <= 10 || first == 0));
    }
    REQUIRE(first == 2);   /* 10 entries, 8 rows: the window ends at 2 */

    /* Walking back up moves the window up again */
    sel = 0;
    ui_list_clamp(&sel, &first, 10, 8);
    REQUIRE(first == 0);

    /* A list shorter than the window never scrolls */
    sel = 3;
    first = 0;
    ui_list_clamp(&sel, &first, 5, 8);
    REQUIRE(first == 0);
    REQUIRE(sel == 3);

    /* Zero rows on screen (a pathological small panel) must not divide by 0 */
    sel = 2;
    first = 0;
    ui_list_clamp(&sel, &first, 5, 0);
    REQUIRE(first <= sel);
}
