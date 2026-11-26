/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file gfx_capture.c
 * @brief Recorder of the text and the selection a screen drew.
 *
 * See gfx_capture.h. Two frames, one being filled by the UI thread and one
 * complete, swapped by gfx_capture_frame(). The reader copies the complete one
 * under a mutex; the writer never waits for the reader, it only takes the
 * mutex for the swap.
 */

#include <pthread.h>
#include <string.h>

#include "core/gfx_capture.h"
#include "core/graphics.h"

static gfx_capture_frame_t frames[2];
static gfx_capture_frame_t *building = &frames[0];
static gfx_capture_frame_t *complete = NULL;
static bool have_frame = false;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_text(char *dst, size_t len, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t i = 0;
    for (; (i + 1) < len && src[i] != '\0'; i++) {
        /* A multi-line string keeps its lines on one row of the dump: the
           layout of the block is what the PNG is for. */
        dst[i] = (src[i] == '\n') ? ' ' : src[i];
    }
    dst[i] = '\0';
}

void gfx_capture_begin(void)
{
    /* Never fill the frame a reader could be copying */
    pthread_mutex_lock(&lock);
    gfx_capture_frame_t *published = complete;
    pthread_mutex_unlock(&lock);

    building = (published == &frames[0]) ? &frames[1] : &frames[0];
    memset(building, 0, sizeof(*building));
}

void gfx_capture_commit(void)
{
    pthread_mutex_lock(&lock);

    complete = building;
    have_frame = true;

    pthread_mutex_unlock(&lock);
}

void gfx_capture_text(point_t start, fontSize_t size, textAlign_t align,
                      color_t color, const char *text)
{
    if (text == NULL || text[0] == '\0')
        return;

    if (building->lines >= GFX_CAPTURE_SLOTS) {
        building->dropped++;
        return;
    }

    gfx_capture_line_t *l = &building->line[building->lines++];
    l->x = start.x;
    l->y = start.y;
    l->font = (uint8_t)size;
    l->align = (uint8_t)align;
    l->rgb = ((uint32_t)color.r << 16) | ((uint32_t)color.g << 8) | color.b;
    copy_text(l->text, sizeof(l->text), text);
    /*
     * The width in pixels, measured in the font it was drawn with: the dump is
     * how a test checks that a line fits the panel and that two lines on one
     * baseline do not draw over each other (tools/simulator/mode_state_shots.py).
     */
    l->width = gfx_getTextWidth(size, l->text);
}

void gfx_capture_header(const char *title, const char *right)
{
    building->header = true;
    copy_text(building->title, sizeof(building->title), title);
    copy_text(building->right, sizeof(building->right), right);
}

void gfx_capture_selection(int16_t top, uint8_t row_h, bool filled)
{
    building->selection = true;
    building->sel_top = top;
    building->sel_h = row_h;
    building->sel_filled = filled;
}

bool gfx_capture_last(gfx_capture_frame_t *out)
{
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&lock);

    bool ok = have_frame && (complete != NULL);
    if (ok)
        memcpy(out, complete, sizeof(*out));

    pthread_mutex_unlock(&lock);

    return ok;
}
