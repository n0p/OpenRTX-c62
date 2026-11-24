/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_CONTACTS_H
#define UI_CONTACTS_H

#include <stdbool.h>
#include <stdint.h>
#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * In-radio contact book, on the MENU_CONTACTS screen.
 *
 * The CPS contact list the menu used to read is empty on the C62 (no codeplug,
 * cps_io_stub), which made Contacts a read-only, blank screen. This module
 * owns the store instead: list, add, edit and delete, with the same key
 * conventions as the channel editor.
 *
 * It is also the address book of the message composer: the peer of a message
 * can be picked from here.
 */

/** Load the store and show the list (called on entering MENU_CONTACTS). */
void ui_contacts_open(void);

/** True while the contact screens own the keypad. */
bool ui_contacts_isOpen(void);

/** Handle one key event. Always consumes it. */
bool ui_contacts_handleEvent(kbd_msg_t *msg, bool *sync_rtx);

/** Draw the current contact screen. */
void ui_contacts_draw(void);

/** Number of contacts, for the message composer. */
uint16_t ui_contacts_count(void);

/** Fill `buf` with the callsign of contact idx. False if idx is invalid. */
bool ui_contacts_callAt(uint16_t idx, char *buf, unsigned len);

/** Index of the contact with this callsign, or 0xFFFF. */
uint16_t ui_contacts_find(const char *call);

#ifdef __cplusplus
}
#endif

#endif /* UI_CONTACTS_H */
