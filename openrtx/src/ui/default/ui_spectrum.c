/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Spectrum analyser screen. See docs/SPECTRUM.md.
 *
 * The screen is a plot of the trace the sweep service keeps, plus the two
 * numbers that are useless on the plot itself: the frequency the span is
 * centred on and the strongest bin with its frequency. Everything else - the
 * span, the step, the maximum hold - is one short press of P2 away, in the
 * quick menu of the screen, because those are settings and the screen is an
 * instrument.
 *
 * The plot is drawn into the framebuffer and the whole screen is pushed once
 * per frame by ui_updateGUI()'s caller, so a sweep in progress never shows a
 * half-cleared plot. The refresh is limited to UI_SPEC_FRAME_MS: the panel can
 * take a frame every 16 ms and the sweep can produce one every 13 ms, but 20
 * frames per second is the point where the eye reads a trace instead of a
 * flicker, and the rest of the frame time is the UI thread's.
 */

#include <stdio.h>
#include <string.h>

#include "core/graphics.h"
#include "core/input.h"
#include "core/spectrum.h"
#include "core/spectrum_service.h"
#include "core/state.h"
#include "ui/ui_default.h"
#include "ui/ui_hints.h"
#include "ui/ui_layout.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_spectrum.h"

/* Horizontal inset of the plot: room for the 1 px frame on both sides */
#define SPEC_X      1
#define SPEC_W      (CONFIG_SCREEN_WIDTH - 2 * SPEC_X)

/* Baselines of the footer (5 pt: ink from baseline-5 to baseline-1) */
#define SPEC_FOOTER_BASE (UI_SPEC_PLOT_BOTTOM + 5)

/* A tick every 25 dB inside the plot, so a level can be read to +-3 dB */
#define SPEC_GRID_DB    25

static uint32_t lastSweeps;      ///< Sweeps already on screen
static int64_t  lastDraw;        ///< Tick of the last redraw
static bool     lastPaused;      ///< Pause state already on screen
static bool     open;            ///< The screen is ours
static uint8_t  backScreen;      ///< Screen the analyser was opened from
static uint8_t  openMode;        ///< Operating mode the sweep was started in

/* ---------------------------------------------------------------- the maths */

int16_t ui_spectrum_levelToY(int16_t dbm, int16_t top, int16_t height)
{
    const int32_t range = (int32_t)UI_SPEC_DB_TOP - (int32_t)UI_SPEC_DB_BOTTOM;

    if (height <= 0)
        return top;

    if (dbm >= UI_SPEC_DB_TOP)
        return top;

    if (dbm <= UI_SPEC_DB_BOTTOM)
        return (int16_t)(top + height - 1);

    const int32_t rows = ((int32_t)UI_SPEC_DB_TOP - (int32_t)dbm) * (height - 1);
    return (int16_t)(top + rows / range);
}

/* --------------------------------------------------------------- the screen */

bool ui_spectrum_enter(void)
{
    if (open)
        return true;

    if (spectrumService_open(state.channel.rx_frequency) == false)
        return false;

    open       = true;
    backScreen = (state.ui_screen == SPECTRUM) ? MAIN_VFO : state.ui_screen;
    openMode   = state.channel.mode;
    lastSweeps = 0;
    lastPaused = false;
    lastDraw   = 0;

    /* The screen owns the keypad: no PTT from here, as in every other menu */
    state.txDisable = true;
    state.ui_screen = SPECTRUM;

    /* txDisable travels in rtxStatus_t, so the RTX thread has to be told */
    ui_rtxSyncRequest = true;

    return true;
}

void ui_spectrum_leave(void)
{
    if (!open)
        return;

    open = false;
    spectrumService_close();

    state.ui_screen = (backScreen == SPECTRUM) ? MAIN_VFO : backScreen;
    state.txDisable = false;

    /* The receiver changes profile: let the RTX thread build the full one */
    ui_rtxSyncRequest = true;
    qm_close();
}

bool ui_spectrum_isOpen(void)
{
    return open;
}

bool ui_spectrum_tick(long long now)
{
    if (!open)
        return false;

    /*
     * Anything that changes the screen behind our back (a message arriving and
     * opening its own screen, the emergency battery screen) must take the
     * receiver back with it: the sweep is not allowed to outlive the plot.
     */
    if (state.ui_screen != SPECTRUM)
    {
        ui_spectrum_leave();
        return true;
    }

    /*
     * A mode change made with the side key while the plot is up has to take the
     * receiver with it: the sweep forces the front end into a wideband profile
     * and the digital modes need every discriminator sample for their own
     * demodulator, which is why spectrumService_open() refuses to start in one
     * of them. Leaving the plot running after the mode changed put the radio in
     * a mode nobody could decode until the screen was closed by hand.
     */
    if (state.channel.mode != openMode)
    {
        ui_spectrum_leave();
        return true;
    }

    const spectrum_t *scan = spectrumService_trace();
    spectrumStatus_t st;
    spectrumService_status(&st);

    if ((scan->sweeps == lastSweeps) && (st.paused == lastPaused))
        return false;

    if ((now - lastDraw) < UI_SPEC_FRAME_MS)
        return false;

    lastSweeps = scan->sweeps;
    lastPaused = st.paused;
    lastDraw   = now;

    return true;
}

bool ui_spectrum_handleEvent(const kbd_msg_t *msg, bool *sync_rtx)
{
    (void)sync_rtx;

    if (!open)
        return true;

    if (msg->keys & KEY_ESC)
    {
        ui_spectrum_leave();
    }
    else if (msg->keys & KEY_ENTER)
    {
        /* Centre the span on the frequency the radio is tuned to */
        spectrumService_centreOn(state.channel.rx_frequency);
    }
    else if (msg->keys & KEY_F1)
    {
        /* The span, one entry up, wrapping: the quick way to zoom out */
        uint8_t next = (uint8_t)(spectrumService_spanIndex() + 1);
        spectrumService_setSpanIndex(next);
    }
    else if (msg->keys & KEY_F2)
    {
        if (msg->long_press)
        {
            qm_openTable(qm_spectrumMenu());
            qm_toggleLatch();
        }
        else
            qm_openTable(qm_spectrumMenu());
    }
    else if (msg->keys & KEY_UP)
    {
        spectrumService_nudge(+1);
    }
    else if (msg->keys & KEY_DOWN)
    {
        spectrumService_nudge(-1);
    }

    return true;
}

/* ---------------------------------------------------------------- the plot */

static void formatFreq(uint32_t hz, char *buf, size_t len)
{
    snprintf(buf, len, "%lu.%04lu", (unsigned long)(hz / 1000000UL),
             (unsigned long)((hz % 1000000UL) / 100UL));
}

static void drawBars(const spectrum_t *scan, uint16_t bins)
{
    for (uint16_t i = 0; i < bins; i++)
    {
        const int16_t level = spectrum_level(scan, i);
        const int16_t y = ui_spectrum_levelToY(level, UI_SPEC_PLOT_TOP,
                                               UI_SPEC_PLOT_H);

        const int16_t x0 = (int16_t)(SPEC_X + ((int32_t)i * SPEC_W) / bins);
        int16_t x1 = (int16_t)(SPEC_X + ((int32_t)(i + 1) * SPEC_W) / bins);

        int16_t w = (int16_t)(x1 - x0);

        /* One pixel of air between bars, unless the raster is denser than that */
        if (w > 1)
            w--;

        const int16_t bottom = (int16_t)(UI_SPEC_PLOT_TOP + UI_SPEC_PLOT_H - 1);
        const int16_t h = (int16_t)(bottom - y + 1);

        if ((w > 0) && (h > 0))
            gfx_drawRect((point_t){ x0, y }, (uint16_t)w, (uint16_t)h,
                         yellow_fab413, true);
    }
}

static void drawHold(const spectrum_t *scan, uint16_t bins)
{
    if (scan->holdOn == false)
        return;

    for (uint16_t i = 0; i < bins; i++)
    {
        const int16_t y = ui_spectrum_levelToY(spectrum_holdLevel(scan, i),
                                               UI_SPEC_PLOT_TOP,
                                               UI_SPEC_PLOT_H);
        const int16_t x0 = (int16_t)(SPEC_X + ((int32_t)i * SPEC_W) / bins);
        int16_t x1 = (int16_t)(SPEC_X + ((int32_t)(i + 1) * SPEC_W) / bins);

        int16_t w = (int16_t)(x1 - x0);

        if (w > 1)
            w--;

        if (w > 0)
            gfx_drawRect((point_t){ x0, y }, (uint16_t)w, 1, color_white, true);
    }
}

void ui_spectrum_draw(void)
{
    const spectrum_t *scan = spectrumService_trace();
    spectrumStatus_t st;
    spectrumService_status(&st);

    const uint16_t bins = (scan->scan.bins > 0) ? scan->scan.bins : 1;

    gfx_clearScreen();

    /*
     * Header: the same band, rule and fonts as every other screen. The right
     * field is how the sweep is waiting for the receiver, which is what this
     * pass changed: AUTO is the chip's own glitch indicator, a number is a fixed
     * wait the operator chose, and 400us with the gate abandoned is the sweep
     * saying the indicator did not behave and it went back to the datasheet's
     * worst case (docs/SPECTRUM.md §3.2c and §3.4). The span is in the footer
     * with the centre frequency, so nothing is lost by using the header here.
     */
    char right[10];

    if (st.paused) {
        strcpy(right, "PAUSA");
    }
    else if ((st.settleUs == 0) && !st.gateOn) {
        strcpy(right, "400us");
    }
    else if (st.settleUs > 0) {
        snprintf(right, sizeof(right), "%luus", (unsigned long)st.settleUs);
    }
    else {
        strcpy(right, "AUTO");
    }

    ui_list_header_only("ESPECTRO", right);

    /* The plot: a 1 px frame and a tick every 25 dB */
    gfx_drawRect((point_t){ SPEC_X, UI_SPEC_PLOT_TOP }, SPEC_W,
                 UI_SPEC_PLOT_H, color_grey, false);

    for (int16_t db = UI_SPEC_DB_TOP - SPEC_GRID_DB; db > UI_SPEC_DB_BOTTOM;
         db -= SPEC_GRID_DB)
    {
        const int16_t y = ui_spectrum_levelToY((int16_t)db, UI_SPEC_PLOT_TOP,
                                               UI_SPEC_PLOT_H);
        gfx_drawHLine(y, 1, color_grey);
    }

    /*
     * The bin being swept, under the trace: it is what tells an operator that
     * the analyser is running rather than showing a frozen picture.
     */
    if ((st.index > 0) && (st.index < bins))
    {
        const int16_t x = (int16_t)(SPEC_X + ((int32_t)st.index * SPEC_W) / bins);
        gfx_drawVLine(x, 1, color_grey);
    }

    drawBars(scan, bins);
    drawHold(scan, bins);

    /* The strongest bin: a white marker on the top edge of its column */
    if (scan->valid && (st.peakDbm > UI_SPEC_DB_BOTTOM))
    {
        const uint16_t peakBin = spectrum_binOf(scan, st.peakFreq);
        const int16_t x = (int16_t)(SPEC_X + ((int32_t)peakBin * SPEC_W) / bins);

        int16_t w = (int16_t)((SPEC_W * 1) / bins);
        if (w < 3)
            w = 3;

        gfx_drawRect((point_t){ x, UI_SPEC_PLOT_TOP }, (uint16_t)w, 2,
                     color_white, true);
    }

    /* Footer: centre and span on the left, the peak with its frequency right */
    char left[24];
    char peak[24];
    char freq[16];

    formatFreq(st.centre, freq, sizeof(freq));
    snprintf(left, sizeof(left), "%s +-%s", freq,
             spectrumService_spanName(spectrumService_spanIndex()));

    if (scan->valid)
    {
        formatFreq(st.peakFreq, freq, sizeof(freq));
        snprintf(peak, sizeof(peak), "%d %s", (int)st.peakDbm, freq);
    }
    else
    {
        snprintf(peak, sizeof(peak), "--");
    }

    gfx_print((point_t){ UI_TEXT_X, SPEC_FOOTER_BASE }, FONT_SIZE_5PT,
              TEXT_ALIGN_LEFT, color_white, "%s", left);

    gfx_print((point_t){ CONFIG_SCREEN_WIDTH - UI_TEXT_X, SPEC_FOOTER_BASE },
              FONT_SIZE_5PT, TEXT_ALIGN_RIGHT, color_white, "%s", peak);

    /* The hints row is the shared one: ui_hints.c owns what the keys mean */
}
