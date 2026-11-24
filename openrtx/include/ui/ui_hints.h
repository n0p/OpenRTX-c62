/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_UI_HINTS_H
#define UI_UI_HINTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Height reserved at the bottom of the screen for the hints row, in pixels.
 *
 * The C62 keypad has four keys worth documenting: the green MENU key
 * (KEY_ENTER), the red A/B key (KEY_ESC) and the two programmable keys of the
 * matrix's second column, P1 (KEY_F1) and P2 (KEY_F2). They are drawn in that
 * order, in the 5 pt font: four short hints fit in the 152 px of the C62's
 * 160 px width.
 */
#define UI_HINTS_H 11

/**
 * What the four keys do on one screen. A NULL entry hides that key.
 */
typedef struct
{
    const char *green;  /**< KEY_ENTER: the green MENU key */
    const char *p1;     /**< KEY_F1: P1, matrix row 0 col 1  */
    const char *p2;     /**< KEY_F2: P2, matrix row 1 col 1  */
    const char *red;    /**< KEY_ESC: the red A/B key     */
} ui_hints_t;

/**
 * Hints for a UI screen.
 *
 * @param screen: state.ui_screen value.
 * @return the hints, or NULL for screens without a hints row (the main
 *         screens, whose bottom row is the S-meter).
 */
const ui_hints_t *ui_hints_for_screen(uint8_t screen);

/**
 * Draw the hints row at the bottom of the screen.
 *
 * @param hints: hints to draw, NULL draws nothing.
 */
void ui_hints_draw(const ui_hints_t *hints);

/**
 * Draw the hints row of a screen, if it has any.
 *
 * @param screen: state.ui_screen value.
 */
void ui_hints_draw_for_screen(uint8_t screen);

#ifdef __cplusplus
}
#endif

#endif /* UI_UI_HINTS_H */
