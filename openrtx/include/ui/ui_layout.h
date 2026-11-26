/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

/*
 * One geometry for every list screen (docs/UI.md, "Layout").
 *
 * The C62 is 160x128. Every screen that shows a header and a list of rows -
 * the main menu, the settings pages, the quick menu, the channel list, the
 * contact book, the messages folders, the APRS lists and the zone browser -
 * takes its numbers from here, so that a row is the same height, the text sits
 * on the same baseline inside its selection rectangle and a list scrolls the
 * same way whichever screen it is.
 *
 * The numbers are derived from the font metrics, not guessed: gfx_print() takes
 * the *baseline* as its y coordinate and a glyph is drawn at
 * `baseline + yOffset`, so a 6 pt glyph with yOffset -7 and height 8 fills
 * baseline-7 .. baseline. That is 7 px of ascent and 2 px of descent (the 'g'
 * and 'p' descenders), and a row of 12 px leaves 2 px above and 1 px below the
 * ink, which is what makes the inverted selection rectangle look like it holds
 * the text instead of cutting it.
 */

#include <stdint.h>
#include <stdbool.h>
#include "core/graphics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Header band: the title of the screen and the rule under it. */
#define UI_HEADER_H         14   /**< Band height, top of the screen      */
#define UI_HEADER_BASELINE  11   /**< Baseline of the 8 pt title          */
#define UI_HEADER_RULE_Y    13   /**< Grey rule, last line of the band    */
#define UI_HEADER_RIGHT_FONT FONT_SIZE_5PT /**< Count/status on the right  */

/* List rows: 6 pt text, 12 px high, 8 of them fit above the hints row. */
#define UI_ROW_H        12
#define UI_ROW_BASELINE  9
#define UI_ROW_ASCENT    7   /**< 6 pt cap height above the baseline       */
#define UI_ROW_DESCENT   2   /**< 6 pt descender below the baseline        */
#define UI_ROW_FONT      FONT_SIZE_6PT

/* Left margin shared by headers, labels and values. */
#define UI_TEXT_X       2

/** Y of the first list row: right under the header rule. */
#define UI_LIST_TOP     (UI_HEADER_H + 1)

/** Row height, from the real 6 pt metric (ascent + descent + slack). */
uint8_t ui_row_height(void);

/** Baseline inside a row, measured from the top of the row. */
uint8_t ui_row_baseline(void);

/** Ascent of the row font: ink starts at baseline - ascent. */
uint8_t ui_row_ascent(void);

/** Descent of the row font: descenders reach baseline + descent. */
uint8_t ui_row_descent(void);

/** Y of the first row of a list. */
int16_t ui_list_top(void);

/**
 * How many rows of `row_h` pixels fit between the first row and the hints row.
 */
uint8_t ui_rows_fit(uint8_t row_h);

/** Top of row `row` (0-based) in a list of the default height. */
int16_t ui_row_top(uint8_t row);

/**
 * Baseline of a row of height `row_h` whose top is `top`.
 *
 * Centres the glyph box: the ascent gets one pixel more slack than the ascent
 * needs when the row is tall (the channel list's two-line rows), so the ink of
 * the first line never touches the line above.
 */
int16_t ui_row_baseline_at(int16_t top, uint8_t row_h);

/**
 * Draw the header band: clear screen, title on the left in 8 pt, optional
 * right-hand status in 5 pt, grey rule under both.
 */
void ui_list_header(const char *title, const char *right);

/**
 * Draw the header without clearing the screen (for screens that paint their own
 * background first).
 */
void ui_list_header_only(const char *title, const char *right);

/**
 * Inverted selection rectangle of one row, filling the width of the screen.
 *
 * @param top: top of the row.
 * @param row_h: height of the row.
 * @param filled: false for a hollow rectangle (edit mode, as in the settings).
 */
void ui_row_selection(int16_t top, uint8_t row_h, bool filled);

/**
 * Text of a row in the row colour: black on the inverted selection, white
 * otherwise.
 */
void ui_row_text(int16_t baseline, fontSize_t font, textAlign_t align,
                 bool selected, const char *fmt, ...);

/**
 * Scroll window shared by every list.
 *
 * Keeps `sel` inside `[0, count)` and moves `first` so that the selection is
 * visible, without ever leaving blank lines above the last entries. The same
 * function scrolls the menu, the channel list, the contacts, the messages and
 * the zones, so UP on the first row and DOWN on the last one always do the same
 * thing in all of them.
 *
 * @param sel:   in/out, selected index.
 * @param first: in/out, first visible index.
 * @param count: number of entries.
 * @param rows:  rows that fit on screen.
 */
void ui_list_clamp(uint16_t *sel, uint16_t *first, uint16_t count, uint8_t rows);

#ifdef __cplusplus
}
#endif

#endif /* UI_LAYOUT_H */
