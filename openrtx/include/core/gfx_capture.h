/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GFX_CAPTURE_H
#define GFX_CAPTURE_H

/**
 * @file gfx_capture.h
 * @brief A text copy of what a screen drew, for the host simulator.
 *
 * The simulator has to answer "what is on the screen" without decoding the
 * framebuffer, so that a script can assert on a title, a row list, the row the
 * cursor is on and the value of a settings entry (docs/SIMULATOR.md). The
 * strings are the ones the firmware produced from its own structures, recorded
 * at the one place every screen draws text:
 *
 *  - gfx_printBufferClipped(), in core/graphics.c, for the text;
 *  - ui_list_header() and ui_row_selection(), in ui/ui_layout.c, for the title
 *    band and the inverted row, which is the selection of every list screen;
 *  - gfx_capture_caret(), called by the screens that edit inside a field, for
 *    the caret the operator moves with P1/P2 on a keypad with no arrows.
 *
 * The recorder is not part of the firmware build: the call sites are compiled
 * only when UI_CAPTURE is defined, which the simulator target does and no other
 * target does (see meson.build). Nothing is allocated, nothing is locked on the
 * radio.
 *
 * Frames are double buffered: the UI thread fills the frame being drawn and
 * swaps it for the reader at the start of the next one, so a dump taken from
 * the control thread always sees one complete screen and never a half-drawn
 * one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/graphics.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of one recorded string, NUL included. */
#define GFX_CAPTURE_TEXT 48

/** Recorded strings per frame, hints row and top bar included. */
#define GFX_CAPTURE_SLOTS 96

/** One string, as it was drawn. */
typedef struct {
    int16_t x;     /**< x passed to gfx_print()          */
    int16_t y;     /**< baseline y passed to gfx_print() */
    uint8_t font;  /**< enum fontSize_t                  */
    uint8_t align; /**< enum textAlign_t                 */
    uint16_t width; /**< width of the text in that font, px */
    uint32_t rgb;  /**< 0x00RRGGBB of the text colour    */
    char text[GFX_CAPTURE_TEXT];
} gfx_capture_line_t;

/** One screen, as it was drawn. */
typedef struct {
    uint16_t lines;   /**< Recorded strings                 */
    uint32_t dropped; /**< Strings that did not fit         */
    bool header;      /**< The screen drew a header band    */
    char title[24];   /**< Header title, "" when there is none */
    char right[16];   /**< Header right-hand status, or ""  */
    bool caret;       /**< The screen drew an edit caret    */
    int16_t caret_x;  /**< Left of the caret rectangle      */
    int16_t caret_y;  /**< Top of the caret rectangle       */
    uint16_t caret_w; /**< Width of the caret rectangle     */
    bool selection;   /**< The screen drew a row selection  */
    int16_t sel_top;  /**< Top of the selection rectangle   */
    uint8_t sel_h;    /**< Height of the selection          */
    bool sel_filled;  /**< true: selected, false: editing   */
    gfx_capture_line_t line[GFX_CAPTURE_SLOTS];
} gfx_capture_frame_t;

/**
 * Start recording a new frame. Called once per redraw, before any drawing.
 *
 * The frame being filled is the one no reader can see; gfx_capture_commit()
 * publishes it when the screen is complete, so a reader never copies a screen
 * that is half drawn and never needs to know whether the UI is redrawing.
 */
void gfx_capture_begin(void);

/** Publish the frame that has just been drawn. */
void gfx_capture_commit(void);

/** Record one drawn string. */
void gfx_capture_text(point_t start, fontSize_t size, textAlign_t align,
                      color_t color, const char *text);

/** Record the header band of a list screen. */
void gfx_capture_header(const char *title, const char *right);

/** Record the inverted (or hollow) selection rectangle of a list row. */
void gfx_capture_selection(int16_t top, uint8_t row_h, bool filled);

/**
 * Record the caret of a screen that edits inside a field (the channel editor,
 * the contact book, the APRS composer).
 *
 * The caret is a filled rectangle, not a string: without this the dump cannot
 * tell "the cursor is on character 3" from "the cursor is on character 1", and
 * a test of the keys that move it would call them dead (tests/unit/ui_keys.cpp).
 */
void gfx_capture_caret(int16_t x, int16_t y, uint16_t w);

/**
 * Copy the last complete frame.
 *
 * @param out: destination, zeroed and filled.
 * @return true when a frame had been drawn at all.
 */
bool gfx_capture_last(gfx_capture_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GFX_CAPTURE_H */
