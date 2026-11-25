/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_DUMP_H
#define UI_DUMP_H

/**
 * @file ui_dump.h
 * @brief A readable description of the screen and of the radio, for the host.
 *
 * The simulator's control channel answers `dump` with one of these: what the
 * screen shows (title, rows, selected row, the value being edited) and the
 * state behind it (mode, frequencies, channel, settings). The rows and the
 * selection come from the frame recorder in core/gfx_capture.h, which sees the
 * strings the firmware drew from its own structures; everything else is read
 * from `state`, the quick menu's tables and the module accessors. Nothing is
 * decoded from the framebuffer, so a dump is cheap and exact.
 *
 * Two formats, one snapshot: text for a person reading a log and JSON for
 * tools/simulator/tour.py. docs/SIMULATOR.md describes both.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Name of a screen of enum uiScreen ("MENU_SETTINGS", "SPECTRUM", ...). */
const char *ui_dump_screenName(uint8_t screen);

/**
 * Text dump of the current screen and state.
 *
 * @param buf, len: destination and its size.
 * @return the number of characters written, without the terminator.
 */
size_t ui_dump_text(char *buf, size_t len);

/**
 * The same snapshot as one JSON object.
 *
 * @param buf, len: destination and its size.
 * @return the number of characters written, without the terminator.
 */
size_t ui_dump_json(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* UI_DUMP_H */
