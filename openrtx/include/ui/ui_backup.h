/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_BACKUP_H
#define UI_BACKUP_H

#include <stdbool.h>
#include <stdint.h>

#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The two screens behind MENU_BACKUP_RESTORE: the backup of the radio's own
 * stores and its restore (docs/BACKUP.md).
 *
 * They are drawn and handled here, not in ui.c, because the restore is a
 * destructive action with a state of its own (ask, run, report) and that is the
 * shape the other modules of this UI already have (ui_channels.c asks before it
 * deletes a channel, with the same YES/NO hints).
 *
 * The main UI only has to forward the key events of the two screens, draw with
 * ui_backup_draw() and call ui_backup_open() when it opens one of them; the
 * hint bar comes from ui_hints_for_screen() like every other screen.
 */

/** Enter one of the two screens: MENU_BACKUP or MENU_RESTORE. */
void ui_backup_open(uint8_t screen);

/**
 * Handle a key event of MENU_BACKUP or MENU_RESTORE.
 *
 * @param screen: the screen the event belongs to.
 * @param msg: keyboard message.
 * @param sync_rtx: set to true when the radio has to be reconfigured, which a
 *                  restore always needs (settings and channel change under it).
 * @return true if the event was consumed (always).
 */
bool ui_backup_handleEvent(uint8_t screen, kbd_msg_t *msg, bool *sync_rtx);

/** Draw MENU_BACKUP or MENU_RESTORE. */
void ui_backup_draw(uint8_t screen);

#ifdef __cplusplus
}
#endif

#endif /* UI_BACKUP_H */
