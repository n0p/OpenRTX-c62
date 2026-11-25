/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_SPECTRUM_H
#define UI_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#include "core/input.h"
#include "ui/ui_default.h"
#include "ui/ui_hints.h"
#include "ui/ui_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The spectrum analyser screen (docs/SPECTRUM.md).
 *
 * A full screen of its own, drawn with the shared layout: the same 14 px header
 * band with the title on the left and one datum on the right, the same 11 px
 * hints row at the bottom and the same 5 pt footer as the other value screens.
 * Everything between them is the plot.
 *
 * The panel is fed from the RAM framebuffer in one write by the caller of
 * ui_updateGUI(), so a new trace replaces the old one without ever showing a
 * half-drawn sweep: there is no direct drawing anywhere in this file.
 */

/** Top of the plot, right under the header rule. */
#define UI_SPEC_PLOT_TOP (UI_HEADER_H + 1)

/** Height of the footer line with the centre, the span and the peak. */
#define UI_SPEC_FOOTER_H 12

/** Last row of the plot: left free above the footer and the hints row. */
#define UI_SPEC_PLOT_BOTTOM \
    (CONFIG_SCREEN_HEIGHT - UI_HINTS_H - UI_SPEC_FOOTER_H - 1)

/** Rows of the plot. */
#define UI_SPEC_PLOT_H (UI_SPEC_PLOT_BOTTOM - UI_SPEC_PLOT_TOP + 1)

/** Top of the dB window drawn in the plot, dBm. */
#define UI_SPEC_DB_TOP (-30)

/** Bottom of the dB window drawn in the plot, dBm. */
#define UI_SPEC_DB_BOTTOM (-130)

/** Refresh period of the plot, ms: the panel and the sweep both allow more. */
#define UI_SPEC_FRAME_MS 50

/**
 * Open the analyser and show it.
 *
 * @return false if it cannot run on this radio now (receiving in a digital
 *         mode, or not receiving at all); the caller stays where it was.
 */
bool ui_spectrum_enter(void);

/** Close the analyser, give the receiver back and return to the main screen. */
void ui_spectrum_leave(void);

/** True while the screen is on. */
bool ui_spectrum_isOpen(void);

/**
 * Periodic part: refreshes the plot when a new sweep has arrived and leaves the
 * analyser if something else has changed the screen.
 *
 * @param now: current tick, ms.
 * @return true if the screen has to be redrawn.
 */
bool ui_spectrum_tick(long long now);

/**
 * Keys while the screen is on.
 *
 * @param msg: keypad event.
 * @param sync_rtx: set when the radio has to be reconfigured.
 * @return always true: the screen owns every key.
 */
bool ui_spectrum_handleEvent(const kbd_msg_t *msg, bool *sync_rtx);

/** Draw the whole screen into the framebuffer. */
void ui_spectrum_draw(void);

/**
 * Pixel row of a level in the plot.
 *
 * The mapping is fixed, not automatic, on purpose: a trace that rescales itself
 * hides a fading signal behind a moving scale. -30 dBm at the top, -130 at the
 * bottom, linear in dB.
 *
 * @param dbm: level.
 * @param top: first row of the plot.
 * @param height: rows of the plot.
 * @return the row, clamped to the plot.
 */
int16_t ui_spectrum_levelToY(int16_t dbm, int16_t top, int16_t height);

#ifdef __cplusplus
}
#endif

#endif /* UI_SPECTRUM_H */
