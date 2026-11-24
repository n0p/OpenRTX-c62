/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_MESSAGES_H
#define UI_MESSAGES_H

#include <stdbool.h>
#include <stdint.h>
#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Messaging screens: inbox, message view, sent folder, compose and delivery
 * status. They are transport agnostic: the compose screen picks one of the
 * transports the build and the current mode provide (APRS over FM, DMR SMS,
 * TETRA SDS) and msg_service does the rest.
 *
 * Screens: MENU_MESSAGES (inbox), MSG_VIEW, MSG_SENT, MSG_COMPOSE, MSG_STATUS.
 */

/** Open the inbox. */
void ui_msgs_open(void);

/** Show the composer, optionally addressed to `peer` (NULL for the draft). */
void ui_msgs_compose(const char *peer);

/** True for every screen this module owns. */
bool ui_msgs_isScreen(uint8_t screen);

/** Handle one key event. Always consumes it. */
bool ui_msgs_handleEvent(kbd_msg_t *msg, bool *sync_rtx);

/** Draw the screen currently in state.ui_screen. */
void ui_msgs_draw(uint8_t screen);

/**
 * Housekeeping: poll the transports and flush the compose draft.
 * Call once per UI pass with the current tick.
 */
void ui_msgs_tick(long long now);

/** "!3" when there are unread messages, "" otherwise. For the menu entry. */
void ui_msgs_fmt_unread(char *buf, int len);

#ifdef __cplusplus
}
#endif

#endif /* UI_MESSAGES_H */
