/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Packet modem screens: the configuration page (mode, TXDELAY, persistence,
 * queue) and the status page (frames sent and received, errors, carrier).
 *
 * Same arrangement as the APRS screens: its own file with four hooks in the
 * default UI (the enum values, the draw dispatch, the key dispatch and the FM
 * status line), the shared row geometry of ui_layout.h for every line, and the
 * shared mode registry for the names it prints (ui_modes.h, whose
 * chstore_modeName() is the one place a mode is spelled).
 */
#ifndef UI_PACKET_H
#define UI_PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True if the screen belongs to the packet modem set. */
bool ui_packet_isScreen(uint8_t screen);

/** Draw one of the packet modem screens. */
void ui_packet_draw(uint8_t screen);

/**
 * Handle a key press on a packet modem screen.
 *
 * @return true if the key was consumed.
 */
bool ui_packet_fsm(kbd_msg_t msg, bool *sync_rtx);

/**
 * Text for the FM main screen: "PKT 12" (frames received) or "PKT TX" while a
 * frame is on the air, empty when the modem is off.
 */
void ui_packet_statusLine(char *buf, size_t cap);

/**
 * Open the configuration page (quick menu "Packet" row). Red returns to the
 * screen it was opened from.
 */
void ui_packet_config(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_PACKET_H */
