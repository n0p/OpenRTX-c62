/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * APRS screens for the default UI: station list, station detail, message
 * inbox, message view, message editor and the APRS configuration page.
 *
 * The screens live in their own files on purpose: the default UI is being
 * reworked (menus, hints row, quick menu) in a separate branch, and this file
 * only needs four hooks in it:
 *
 *   1. enum uiScreen: the APRS_* values                (ui_default.h)
 *   2. ui_updateGUI(): case APRS_* -> ui_aprs_draw()   (ui.c)
 *   3. ui_updateFSM(): early dispatch to ui_aprs_fsm() (ui.c)
 *   4. the FM main screen status line                  (ui_main.c)
 */
#ifndef UI_APRS_H
#define UI_APRS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True if the screen belongs to the APRS set. */
bool ui_aprs_isScreen(uint8_t screen);

/** Draw one of the APRS screens. */
void ui_aprs_draw(uint8_t screen);

/**
 * Handle a key press on an APRS screen.
 *
 * @return true if the key was consumed.
 */
bool ui_aprs_fsm(kbd_msg_t msg, bool *sync_rtx);

/**
 * Text for the FM main screen: "APRS 12" (packets received) or "APRS !2"
 * (two unread messages), empty when decoding is off.
 */
void ui_aprs_statusLine(char *buf, size_t cap);

/** Enter the APRS screens from the FM main screen (green key long press). */
void ui_aprs_enter(void);

/**
 * Open the APRS configuration page directly (quick menu "APRS" row).
 * Red returns to the screen it was opened from.
 */
void ui_aprs_config(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_APRS_H */
