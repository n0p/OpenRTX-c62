/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_TOOLS_H
#define UI_TOOLS_H

/*
 * The tools menu: P1 held down on a main screen.
 *
 * It is the door to the things that are neither a mode setting (the quick
 * menu) nor a screen of their own (the channel list), and it replaced the
 * one-shot shortcut that opened the channel list from P1 long: the same key
 * now opens a list whose last row is the channel list, so that shortcut is
 * kept and the scanner, the VFO carrier search, the analyser and the mode
 * selector get the door they did not have.
 *
 * It is a screen of its own (TOOLS_MENU in state.ui_screen) so the rules that
 * already exist -- the PTT is only armed on the two main screens, a screen
 * that owns the keypad answers every key -- apply without a second copy, and
 * so the bottom hint bar and the dump can name it.
 *
 * Rows are compile-time: an image without CONFIG_SCAN has no "Scan" row, so no
 * row is ever a key that does nothing. See docs/UI.md, "Herramientas".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open the tools menu on top of the main screen.
 *
 * Remembers the screen it was opened from (MAIN_VFO or MAIN_MEM), takes the
 * keypad and disables the transmitter, the way the channel list does.
 */
void ui_tools_open(void);

/** Close it, back to the screen it was opened from. */
void ui_tools_close(void);

/** True while the list or the mode submenu is on screen. */
bool ui_tools_isOpen(void);

/**
 * Handle a key event.
 *
 * @param msg: keyboard message.
 * @param sync_rtx: set to true when the radio configuration has to be updated.
 * @return true if the event was consumed (always, while the module is open).
 */
bool ui_tools_handleEvent(kbd_msg_t *msg, bool *sync_rtx);

/** Draw the current screen of the module (list or mode submenu). */
void ui_tools_draw(void);

/* ------------------------------------------------------------------------- */
/* Read-only view, for the simulator's dump and for the host tests           */
/* ------------------------------------------------------------------------- */

/** True when the mode submenu is the one on screen. */
bool ui_tools_inModes(void);

/** Row under the cursor of the screen that is on (list or modes). */
uint8_t ui_tools_selected(void);

/** Rows of the screen that is on: five in the list, one per mode. */
uint8_t ui_tools_rowCount(void);

/** Label of a row of the screen that is on, NULL past the end. */
const char *ui_tools_rowLabel(uint8_t row);

/**
 * Right-hand value of a row of the screen that is on (""), written into buf.
 * Never NULL: the list's rows always have something to say (the mode in use,
 * the bank count, "RX" for a mode that cannot transmit).
 */
const char *ui_tools_rowValue(uint8_t row, char *buf, size_t len);

/** Title of the header of the screen that is on. */
const char *ui_tools_title(void);

/** Screen the menu was opened from (MAIN_VFO or MAIN_MEM). */
uint8_t ui_tools_backScreen(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_TOOLS_H */
