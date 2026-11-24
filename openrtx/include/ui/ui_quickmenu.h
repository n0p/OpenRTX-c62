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
    QM_GOTO,      /**< ENTER opens another screen (mode specific editor) */
    QM_MODE       /**< Operating mode selector, rebuilds the table       */
};

/** One quick menu row. */
typedef struct
{
    const char *label;          /**< Left column, <= 10 uppercase chars    */
    uint8_t type;               /**< enum qm_type                          */
    int min;
    int max;
    int step;

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

/** Popup state. */
typedef struct
{
    bool open;                  /**< Popup visible                         */
    bool latched;               /**< Kept open after the key is released   */
    bool editing;               /**< Editing the selected value            */
    uint8_t selected;           /**< Cursor, index into qm->items          */
    uint8_t first;              /**< First visible row                     */
    long long lastKeyTick;      /**< Auto-close timer                      */
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

/**
 * Move the cursor, or change the value while editing.
 *
 * @param delta: -1 up, +1 down.
 */
void qm_move(int delta);

/** ENTER: start editing, commit the value, toggle, or open a screen. */
void qm_enter(void);

/** ESC while editing: leave the value as it was. */
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
