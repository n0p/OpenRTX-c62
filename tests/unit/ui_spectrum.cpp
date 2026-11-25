/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The spectrum analyser as the keypad and the RTX thread see it, driven through
 * the real UI FSM and the real service.
 *
 * What is asserted here is what spectrum.cpp cannot reach: that the screen
 * opens on the VFO frequency, that the receiver is given back when it closes,
 * that the keys of the screen do what the hints row says, that the quick menu
 * of the screen carries the span and the step, and that the menu of the FM mode
 * leads to it. The sweep itself is driven here the way the RTX thread drives
 * it, one poll at a time.
 *
 * The service answers with a synthetic band in a host build (see
 * spectrum_service.c), so the trace this test reads is the one the desktop
 * application would show, with three carriers at fixed offsets from the centre.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" {
#include "core/event.h"
#include "core/graphics.h"
#include "core/input.h"
#include "core/spectrum.h"
#include "core/spectrum_service.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "interfaces/cps_io.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_spectrum.h"
}

extern state_t state;

namespace
{

constexpr freq_t FREQ = 145500000;

/* The carriers of the synthetic front end, relative to the centre */
constexpr int32_t CARRIER_1 = -325000;
constexpr int32_t CARRIER_2 = 0;

void press(uint32_t keys, bool isLong)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = isLong ? 1 : 0;

    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

void resetUi(void)
{
    /* Leave whatever the previous test case left open */
    if (ui_spectrum_isOpen())
        ui_spectrum_leave();

    qm_close();

    /* The raster is a setting of the screen: start every case from the default */
    spectrumService_setSpanIndex(2);   /* 1 MHz */
    spectrumService_setStepIndex(1);   /* 25 kHz */

    state.channel.mode          = OPMODE_FM;
    state.channel.rx_frequency  = FREQ;
    state.channel.tx_frequency  = FREQ;
    state.channel.bandwidth     = BW_25;
    state.channel.power         = 1000;
    state.channel.rx_only       = false;
    state.ui_screen             = MAIN_VFO;
    state.tuner_mode            = VFO;
    state.txDisable             = false;
    state.charge                = 100;
    state.emergency             = false;
    state.step_index            = 1;
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;      /* silent in the harness */

    ui_rtxSyncRequest = false;
}

/**
 * Drive the service the way the RTX thread does, for long enough that a sweep
 * and at least one listen window have gone past.
 */
void runSweeps(int ms)
{
    const auto until = std::chrono::steady_clock::now()
                     + std::chrono::milliseconds(ms);

    while (std::chrono::steady_clock::now() < until)
    {
        (void)spectrumService_poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} /* namespace */

TEST_CASE("The screen opens on the VFO frequency and gives the radio back")
{
    resetUi();

    REQUIRE_FALSE(spectrumService_active());

    ui_open_screen(SPECTRUM, 0);

    REQUIRE(state.ui_screen == SPECTRUM);
    REQUIRE(ui_spectrum_isOpen());
    REQUIRE(spectrumService_active());

    /* A screen that owns the keypad never transmits, and the RTX has to know */
    REQUIRE(state.txDisable);
    REQUIRE(ui_rtxSyncRequest);

    /* The span is centred on the frequency the radio was on */
    spectrumStatus_t st;
    spectrumService_status(&st);
    REQUIRE(st.centre == FREQ);
    REQUIRE(st.bins > 0);
    REQUIRE(st.step > 0);

    /* Leaving puts the screen back and re-arms the transmitter */
    ui_rtxSyncRequest = false;
    ui_spectrum_leave();

    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(ui_spectrum_isOpen());
    REQUIRE_FALSE(spectrumService_active());
    REQUIRE_FALSE(state.txDisable);
    REQUIRE(ui_rtxSyncRequest);
}

TEST_CASE("The keys of the screen do what the hints row says")
{
    resetUi();
    ui_open_screen(SPECTRUM, 0);
    runSweeps(80);

    spectrumStatus_t st;
    spectrumService_status(&st);
    const uint32_t centre = st.centre;
    const uint32_t step = st.step;
    const uint8_t span = spectrumService_spanIndex();

    /* UP and DOWN move the span, one step of the raster each */
    press(KEY_UP, false);
    runSweeps(80);
    spectrumService_status(&st);
    REQUIRE(st.centre == (centre + step));

    press(KEY_DOWN, false);
    runSweeps(80);
    spectrumService_status(&st);
    REQUIRE(st.centre == centre);

    /* P1 walks the span, and the receiver is not retuned by it */
    press(KEY_F1, false);
    REQUIRE(spectrumService_spanIndex() == (uint8_t)((span + 1) % SPECTRUM_SPAN_COUNT));

    /* ENTER puts the span back on the frequency the radio is tuned to */
    press(KEY_ENTER, false);
    runSweeps(80);
    spectrumService_status(&st);
    REQUIRE(st.centre == FREQ);

    /* The radio itself never moved: the analyser only tunes the receiver */
    REQUIRE(state.channel.rx_frequency == FREQ);
    REQUIRE(state.channel.tx_frequency == FREQ);

    /* RED leaves */
    press(KEY_ESC, false);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE_FALSE(spectrumService_active());
}

TEST_CASE("The sweep produces a trace with the carrier in the right bin")
{
    resetUi();
    ui_open_screen(SPECTRUM, 0);

    /* Default raster: 1 MHz of span in 25 kHz bins around 145.5 MHz */
    spectrumStatus_t st;
    spectrumService_status(&st);
    REQUIRE(st.span == 1000000U);
    REQUIRE(st.step == 25000U);
    REQUIRE(st.bins == 41);

    runSweeps(200);

    spectrumService_status(&st);
    REQUIRE(st.valid);
    REQUIRE(st.sweeps >= 1);
    REQUIRE(st.peakDbm == -62);                    /* the strongest carrier */
    REQUIRE(st.peakFreq == (FREQ + CARRIER_1));

    const spectrum_t *scan = spectrumService_trace();
    REQUIRE(spectrum_level(scan, spectrum_binOf(scan, FREQ + CARRIER_2))
            == -78);

    ui_spectrum_leave();
}

TEST_CASE("The quick menu of the screen carries the span and the step")
{
    resetUi();
    ui_open_screen(SPECTRUM, 0);
    runSweeps(80);

    /* P2 opens the screen's own table, not the mode's */
    press(KEY_F2, false);
    REQUIRE(qm_state.open);
    REQUIRE(qm_state.qm == qm_spectrumMenu());

    const qm_menu_t *qm = qm_spectrumMenu();
    REQUIRE(qm->count >= 4);

    /* The first two rows are the span and the step, as enumerations */
    REQUIRE(qm->items[0].type == QM_ENUM);
    REQUIRE(qm->items[1].type == QM_ENUM);
    REQUIRE(qm->items[0].names != nullptr);
    REQUIRE(qm->items[1].names != nullptr);

    /* Every row reads a value or an action, and every label fits a row */
    char text[QM_VALUE_BUF];

    for (uint8_t row = 0; row < qm->count; row++)
    {
        REQUIRE(qm->items[row].label != nullptr);
        REQUIRE(strlen(qm->items[row].label) <= 10);

        if (qm->items[row].type == QM_INFO)
            REQUIRE(qm_value_text_of(qm, row, text, sizeof(text)));
    }

    /* P2 on the first row changes the span of the analyser */
    const uint8_t span = spectrumService_spanIndex();
    qm_adjust(+1);
    REQUIRE(spectrumService_spanIndex() == (uint8_t)((span + 1) % SPECTRUM_SPAN_COUNT));

    /* The settle row cycles the wait the sweep uses (AUTO, 50, 100, 200, 400) */
    spectrumService_setSettleIndex(0);
    REQUIRE(spectrumService_settleUs() == 0);

    qm_select(2);
    qm_adjust(+1);
    REQUIRE(spectrumService_settleIndex() == 1);
    REQUIRE(spectrumService_settleUs() == 50);

    qm_adjust(-1);
    REQUIRE(spectrumService_settleIndex() == 0);
    REQUIRE(spectrumService_settleUs() == 0);

    /* The clear row is an action, wherever it sits: ENTER runs it and does not
       close the popup */
    uint8_t clearRow = 0xFF;

    for (uint8_t row = 0; row < qm->count; row++)
    {
        if (strcmp(qm->items[row].label, "BORRAR MAX") == 0)
            clearRow = row;
    }

    REQUIRE(clearRow != 0xFF);
    qm_select(clearRow);
    qm_enter();
    REQUIRE(qm_state.open);

    /* And the step row really moves the raster */
    qm_select(1);
    qm_adjust(+1);
    runSweeps(60);

    spectrumStatus_t st;
    spectrumService_status(&st);
    REQUIRE(st.step == spectrumService_trace()->scan.step);

    qm_cancel();
    REQUIRE_FALSE(qm_state.open);

    ui_spectrum_leave();
}

TEST_CASE("The mode's quick menu leads to the analyser")
{
    const qm_menu_t *fm = qm_findMenu(OPMODE_FM);
    REQUIRE(fm != nullptr);

    bool found = false;

    for (uint8_t row = 0; row < fm->count; row++)
    {
        if ((fm->items[row].type == QM_GOTO)
            && (fm->items[row].goto_screen == SPECTRUM))
            found = true;
    }

    REQUIRE(found);
}

TEST_CASE("The analyser refuses a digital mode")
{
    resetUi();
    state.channel.mode = OPMODE_M17;

    /*
     * A digital mode needs every sample of the discriminator and takes its
     * squelch from it: the analyser stays out instead of breaking the mode.
     * (The check is a radio-side one, so it only exists where there is a
     * receiver to sweep: a host build has none and answers with its synthetic
     * band, which is what the tests above use.)
     */
#if defined(PLATFORM_C62)
    REQUIRE_FALSE(spectrumService_open(FREQ));
#endif

    state.channel.mode = OPMODE_FM;
}

namespace
{

/** The framebuffer as the panel sees it: RGB565, one word per pixel. */
struct panel
{
    const uint16_t *px;

    explicit panel()
    {
        px = static_cast<const uint16_t *>(gfx_framebuffer());
    }

    uint16_t at(int x, int y) const { return px[x + y * CONFIG_SCREEN_WIDTH]; }

    /** Pixels that are not black in a rectangle, inclusive of both ends. */
    unsigned lit(int x0, int y0, int x1, int y1) const
    {
        unsigned n = 0;

        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                if (at(x, y) != 0)
                    n++;

        return n;
    }
};

/** Write the framebuffer as a PPM, for a human to look at the screen. */
void dumpScreen(const char *path)
{
    const uint16_t *px = static_cast<const uint16_t *>(gfx_framebuffer());
    FILE *f = fopen(path, "w");

    if (f == NULL)
        return;

    fprintf(f, "P3\n%d %d\n255\n", CONFIG_SCREEN_WIDTH, CONFIG_SCREEN_HEIGHT);

    for (int y = 0; y < CONFIG_SCREEN_HEIGHT; y++)
    {
        for (int x = 0; x < CONFIG_SCREEN_WIDTH; x++)
        {
            const uint16_t v = px[x + y * CONFIG_SCREEN_WIDTH];
            fprintf(f, "%d %d %d ", ((v >> 11) & 0x1F) * 8, ((v >> 5) & 0x3F) * 4,
                    (v & 0x1F) * 8);
        }

        fprintf(f, "\n");
    }

    fclose(f);
}

} /* namespace */

TEST_CASE("The screen draws a trace where it says it does")
{
    resetUi();
    ui_open_screen(SPECTRUM, 0);
    runSweeps(200);

    ui_spectrum_draw();

    /* ui_updateGUI() draws the shared hints row after the screen, not the
       screen itself: the same two calls, in the same order */
    ui_hints_draw_for_screen(SPECTRUM);

    const panel fb;

    /* The plot has ink: the trace of a band with three carriers is not empty */
    REQUIRE(fb.lit(1, UI_SPEC_PLOT_TOP, CONFIG_SCREEN_WIDTH - 2,
                   UI_SPEC_PLOT_BOTTOM) > 200);

    /* The header band and the footer carry text, and the hints row exists */
    REQUIRE(fb.lit(0, 0, CONFIG_SCREEN_WIDTH - 1, UI_HEADER_RULE_Y) > 50);
    REQUIRE(fb.lit(0, UI_SPEC_PLOT_BOTTOM + 1, CONFIG_SCREEN_WIDTH - 1,
                   CONFIG_SCREEN_HEIGHT - UI_HINTS_H - 1) > 30);

    /* The hints row has the four keys of the screen, and it is the last thing
       on the panel: nothing is drawn below it */
    REQUIRE(fb.lit(0, CONFIG_SCREEN_HEIGHT - UI_HINTS_H,
                   CONFIG_SCREEN_WIDTH - 1, CONFIG_SCREEN_HEIGHT - 1) > 30);

    /* A picture, when somebody wants to look at it */
    const char *path = getenv("SPECTRUM_DUMP");
    if (path != NULL)
        dumpScreen(path);

    ui_spectrum_leave();
}
