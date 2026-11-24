/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_CHANNELS_H
#define UI_CHANNELS_H

#include <stdbool.h>
#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Channel list and channel editor drawn on top of the current UI screen.
 *
 * The module is self contained: it owns two screens (the list and the editor)
 * and, while open, consumes the keypad. The main UI only has to
 *
 *   - forward key events while ui_channels_isOpen() is true,
 *   - draw with ui_channels_draw() instead of the current screen,
 *   - call ui_channels_open() to enter the list.
 *
 * See docs/CHANNELS.md for the exact integration points in ui.c.
 */

/**
 * Open the channel list, selecting the channel currently tuned if it is one of
 * the stored ones. Call once the channel bank has been initialised.
 */
void ui_channels_open(void);

/** True while the list, the editor or the delete confirmation are shown. */
bool ui_channels_isOpen(void);

/**
 * Handle a key event.
 *
 * @param msg: keyboard message.
 * @param sync_rtx: set to true when the radio configuration has to be updated.
 * @return true if the event was consumed (always, while the module is open).
 */
bool ui_channels_handleEvent(kbd_msg_t *msg, bool *sync_rtx);

/** Draw the current channel screen. */
void ui_channels_draw(void);

/**
 * Bottom hint bar. Kept separate so that the shared hint bar of the UI branch
 * can replace its body without touching anything else.
 *
 * @param green: text for the green (ENTER) key.
 * @param p1: text for the P1 (F1) key, NULL to leave it out.
 * @param p2: text for the P2 (F2) key, NULL to leave it out.
 * @param red: text for the red (ESC) key.
 */
void ui_channels_drawHints(const char *green, const char *p1, const char *p2,
                           const char *red);

#ifdef __cplusplus
}
#endif

#endif /* UI_CHANNELS_H */
