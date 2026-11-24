/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Shared geometry of the list screens. See ui/ui_layout.h for the contract and
 * docs/UI.md for why the numbers are what they are: they come from the real
 * font metrics (a 6 pt glyph is drawn from baseline-7 to baseline) and the
 * 160x128 panel, not from a table copied by hand into five files.
 */

#include <stdarg.h>
#include <stdio.h>
#include "core/graphics.h"
#include "ui/ui_default.h"
#include "ui/ui_hints.h"
#include "ui/ui_layout.h"

uint8_t ui_row_height(void)
{
#if CONFIG_SCREEN_HEIGHT > 63
    return UI_ROW_H;
#else
    /* 64 px panels have no room for a 12 px row plus a header */
    return 10;
#endif
}

uint8_t ui_row_ascent(void)
{
    /*
     * UbuntuRegular6pt7b: the tallest cap ('A', '0') has yOffset -7, so its ink
     * runs from baseline-7 to baseline-1. TomThumb (5 pt) would be 5.
     */
    return (UI_ROW_FONT == FONT_SIZE_6PT) ? UI_ROW_ASCENT : 5;
}

uint8_t ui_row_descent(void)
{
    return (UI_ROW_FONT == FONT_SIZE_6PT) ? UI_ROW_DESCENT : 0;
}

uint8_t ui_row_baseline(void)
{
    return (uint8_t)(ui_row_ascent() + 2);
}

int16_t ui_list_top(void)
{
    return (int16_t)UI_LIST_TOP;
}

uint8_t ui_rows_fit(uint8_t row_h)
{
    int16_t avail = (int16_t)(CONFIG_SCREEN_HEIGHT - UI_HINTS_H - ui_list_top());

    if ((row_h == 0) || (avail < (int16_t)row_h))
        return 0;

    return (uint8_t)(avail / row_h);
}

int16_t ui_row_top(uint8_t row)
{
    return (int16_t)(ui_list_top() + (int16_t)row * (int16_t)ui_row_height());
}

int16_t ui_row_baseline_at(int16_t top, uint8_t row_h)
{
    /* Centre the ink box, biased one pixel down so the descenders of the 6 pt
       font have a line to live in: a 12 px row gets baseline top+9, which is
       also QM_ROW_BASELINE. Shorter rows pull the baseline up again, because a
       10 px row is only just tall enough for the 9 px of ink plus descender. */
    int16_t slack = (int16_t)(row_h - ui_row_ascent() - ui_row_descent());
    int16_t above = (slack > 0) ? (int16_t)((slack + 1) / 2) : 0;
    int16_t baseline = (int16_t)(top + above + ui_row_ascent());

    if (baseline + (int16_t)ui_row_descent() > (int16_t)(top + row_h) - 1)
        baseline = (int16_t)(top + row_h - 1 - ui_row_descent());

    if (baseline - (int16_t)ui_row_ascent() < top)
        baseline = (int16_t)(top + ui_row_ascent());

    return baseline;
}

void ui_list_header_only(const char *title, const char *right)
{
    if (title != NULL)
        gfx_print((point_t){ UI_TEXT_X, UI_HEADER_BASELINE }, FONT_SIZE_8PT,
                  TEXT_ALIGN_LEFT, yellow_fab413, "%s", title);

    if (right != NULL)
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH - UI_TEXT_X,
                             UI_HEADER_BASELINE }, UI_HEADER_RIGHT_FONT,
                  TEXT_ALIGN_RIGHT, color_white, "%s", right);

    gfx_drawHLine(UI_HEADER_RULE_Y, 1, color_grey);
}

void ui_list_header(const char *title, const char *right)
{
    gfx_clearScreen();
    ui_list_header_only(title, right);
}

void ui_row_selection(int16_t top, uint8_t row_h, bool filled)
{
    gfx_drawRect((point_t){ 0, top }, CONFIG_SCREEN_WIDTH, row_h, color_white,
                 filled);
}

void ui_row_text(int16_t baseline, fontSize_t font, textAlign_t align,
                 bool selected, const char *fmt, ...)
{
    char buf[64];
    va_list ap;

    va_start(ap, fmt);
    vsniprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);

    gfx_print((point_t){ (align == TEXT_ALIGN_RIGHT)
                             ? (int16_t)(CONFIG_SCREEN_WIDTH - UI_TEXT_X)
                             : UI_TEXT_X,
                         baseline },
              font, align, selected ? color_black : color_white, "%s", buf);
}

void ui_list_clamp(uint16_t *sel, uint16_t *first, uint16_t count, uint8_t rows)
{
    if ((sel == NULL) || (first == NULL))
        return;

    if (count == 0) {
        *sel = 0;
        *first = 0;
        return;
    }

    if (rows == 0)
        rows = 1;

    if (*sel >= count)
        *sel = (uint16_t)(count - 1);

    if (*sel < *first)
        *first = *sel;

    if ((uint16_t)(*sel - *first) >= rows)
        *first = (uint16_t)(*sel - rows + 1);

    /* Never leave blank lines above the last entries */
    if (count > rows && *first > (uint16_t)(count - rows))
        *first = (uint16_t)(count - rows);
}
