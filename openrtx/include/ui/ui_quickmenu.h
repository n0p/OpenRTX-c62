/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_QUICKMENU_H
#define UI_QUICKMENU_H

/*
 * Contextual quick menu: the popup opened with a short press of the C62's
 * second programmable key (P2, KEY_F2). It lists the settings worth changing
 * without leaving the main screen, one table per operating mode, and applies
 * every change live through the same path the menus use (state + resync).
 *
 * See docs/UI.md for the layout and the reasoning behind each item.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QM_VALUE_BUF 20

/** Item kinds. */
enum qm_type
{
    QM_INT = 0,   /**< Numeric value, min..max in steps of step          */
    QM_ENUM,      /**< Index into names[]                                */
    QM_TOGGLE,    /**< QM_ENUM with the OFF/ON names                     */
    QM_INFO,      /**< Read only, rendered by fmt()                      */
    QM_GOTO       /**< ENTER leaves the popup for another screen         */
};

/** One quick menu row. */
typedef struct
{
    const char *label;          /**< Left column, <= 10 uppercase chars    */
    uint8_t type;               /**< enum qm_type                          */
    int min;
    int max;
    int step;
    int fast_step;              /**< Step of a long press, 0 = same as step */

    int (*get)(void);           /**< Current value, NULL for QM_INFO       */
    void (*set)(int);           /**< Store a new value, NULL for QM_INFO   */

    const char *const *names;   /**< QM_ENUM/QM_TOGGLE labels              */
    void (*fmt)(char *buf, int len); /**< QM_INFO / QM_INT value text      */
    uint8_t goto_screen;        /**< QM_GOTO: destination screen           */
    uint8_t goto_item;          /**< QM_GOTO: item selected there          */
} qm_item_t;

/** One mode's quick menu. */
typedef struct
{
    uint8_t mode;               /**< OPMODE_* (rtx/rtx.h)                  */
    const char *title;          /**< Header text                           */
    const qm_item_t *items;
    uint8_t count;
} qm_menu_t;

/** Popup state.
 *
 * There is no "edit mode": on a keypad without arrow keys a modal editor is
 * useless, so P1/P2 change the value of the highlighted row directly and
 * UP/DOWN only move the cursor. `latched` is only set by a long P2/MONI press;
 * an unlatched popup closes itself after QM_TIMEOUT_MS without keys.
 */
typedef struct
{
    bool open;                  /**< Popup visible                         */
    bool latched;               /**< Kept open after the key is released   */
    uint8_t selected;           /**< Cursor, index into qm->items          */
    uint8_t first;              /**< First visible row                     */
    long long lastKeyTick;      /**< Auto-close timer                      */
    uint8_t mode;               /**< Operating mode the table was chosen for */
    const qm_menu_t *qm;        /**< Active table, NULL while closed       */
} qm_state_t;

extern qm_state_t qm_state;

/**
 * Quick menu of an operating mode.
 *
 * @param opmode: state.channel.mode.
 * @return the table to show; the FM one for modes without a table.
 */
const qm_menu_t *qm_findMenu(uint8_t opmode);

/** Open the popup on the table of the current mode. */
void qm_open(void);

/** Close the popup. */
void qm_close(void);

/** Toggle the latch (P2/MONI long press). */
void qm_toggleLatch(void);

/** Time without keys after which an unlatched popup closes itself, ms. */
#define QM_TIMEOUT_MS 15000

/**
 * Move the cursor, or change the value with qm_adjust().
 *
 * @param delta: -1 up, +1 down. Never changes a value: use qm_adjust().
 */
void qm_move(int delta);

/**
 * Change the value of the highlighted row by one step.
 *
 * @param delta: -1 (P1) or +1 (P2). Integers clamp at min/max, enumerations
 *        and toggles wrap; read-only rows do nothing but beep.
 */
void qm_adjust(int delta);

/**
 * Same, with the coarse step (a long press of P1/P2).
 *
 * A 24-bit DMR destination or a CTCSS tone cannot be walked one unit at a time
 * on a keypad: rows that declare a fast_step move by it while the key is held.
 *
 * @param delta: -1 or +1.
 * @param coarse: true for a long press.
 */
void qm_adjust_step(int delta, bool coarse);

/** ENTER: cycle a value, toggle it, or open the screen of a QM_GOTO row. */
void qm_enter(void);

/** ESC: close the popup. Values are applied as they are changed, no undo. */
void qm_cancel(void);

/**
 * Auto-close after a period without keys, and follow mode changes.
 *
 * @param now: current tick.
 * @return true if the popup changed state (closed or switched table).
 */
bool qm_tick(long long now);

/** Jump to a row (digit keys 1..9), 0-based. */
void qm_select(uint8_t index);

/** Draw the popup over the current screen. */
void qm_draw(void);

/*
 * Row geometry, for the layout test and for anything that needs to line up
 * with the popup. All values are pixels; QM_ROW_BASELINE is measured from the
 * top of the row and is where gfx_print() puts the 6 pt baseline.
 *
 * The row is 12 px because the font says so: UbuntuRegular6pt7b draws a cap
 * from baseline-7 to baseline-1 and a descender to baseline+1, so 7 px of
 * ascent plus 2 of descent leave 3 px of slack in 12. The baseline sits at
 * +9, which puts the ink at +2..+8 and the descenders at +10, inside the
 * selection rectangle with one pixel of air above and below. Seven rows fit
 * between the 14 px header and the 11 px hint bar (87 / 12 = 7.25).
 */
#define QM_HEADER_H   14   /* header band, 8 pt title + frequency        */
#define QM_HEADER_BASELINE 11 /* baseline of the header text             */
#define QM_ROW_H      12   /* one row, 6 pt text                         */
#define QM_ROW_BASELINE 9  /* baseline offset inside a row               */
#define QM_TEXT_X      2   /* left margin of the labels                  */
#define QM_VALUE_X     2   /* right margin of the values                 */
#define QM_FONT        FONT_SIZE_6PT

uint8_t  qm_rowHeight(void);
uint8_t  qm_rowBaseline(void);
uint8_t  qm_visibleRows(void);
int16_t  qm_rowTop(uint8_t row);

/** Value text of a row, for tests and voice prompts. */
bool qm_value_text(uint8_t index, char *buf, int len);

/*
 * Implemented in ui.c: the keypad lock lives in the UI state.
 */
bool ui_getInputLock(void);
void ui_setInputLock(bool on);

/** Ask the UI thread to send the state to the rtx thread. */
void qm_requestSync(void);

/*
 * Helpers also used by the menu code (ui_menu.c): the same value text must
 * read the same in the quick menu and in Settings > Radio.
 */
int qm_get_power(void);
void qm_set_power(int mw);
void qm_fmt_power(char *buf, int len);
int qm_get_brightness(void);
void qm_set_brightness(int pct);
int qm_get_rx_fine(void);
void qm_set_rx_fine(int hz);
void qm_fmt_fine(char *buf, int len);
int qm_fm_get_sql(void);
void qm_fm_set_sql(int level);

#ifdef __cplusplus
}
#endif

#endif /* UI_QUICKMENU_H */
