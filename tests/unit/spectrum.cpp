/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The spectrum analyser's arithmetic, against a synthetic receiver.
 *
 * There is no I/Q anywhere in this radio: a trace is a list of RSSI readings
 * taken one frequency at a time, so what can be tested without hardware is
 * exactly what the screen depends on -- that a bin is where the sweep says it
 * is, that the level of a bin is the level of the signal that is there, that
 * the peak and the maximum hold point at the right column, and that a level
 * becomes the row of the plot it should.
 *
 * "Synthetic receiver" is a signal generator in software: a noise floor and a
 * few carriers with a triangular skirt 15 dB per bin either side, which is
 * what one IF filter of a carrier looks like when the raster is coarser than
 * the filter. Feeding it to the engine is the software equivalent of pointing
 * a generator at the antenna socket and stepping the receiver across it.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

extern "C" {
#include "core/spectrum.h"
#include "ui/ui_spectrum.h"
}

namespace
{

constexpr uint32_t MHZ = 1000000U;
constexpr int16_t  FLOOR_DBM = -105;

struct carrier
{
    uint32_t hz;
    int16_t  dbm;
};

/** Level the synthetic front end reports at one frequency, dBm. */
int16_t syntheticLevel(const carrier *peaks, size_t count, uint32_t hz,
                       uint32_t step)
{
    int16_t level = FLOOR_DBM;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t distance = (hz > peaks[i].hz) ? (hz - peaks[i].hz)
                                                    : (peaks[i].hz - hz);
        const uint32_t bins = (step > 0) ? (distance / step) : 0;

        if (bins > 3)
            continue;

        int16_t candidate = (int16_t)(peaks[i].dbm - (int16_t)(15 * bins));

        if (candidate < FLOOR_DBM)
            candidate = FLOOR_DBM;

        if (candidate > level)
            level = candidate;
    }

    return level;
}

/** One sweep of the synthetic front end over the engine's raster. */
void sweep(spectrum_t *s, const carrier *peaks, size_t count)
{
    spectrum_startSweep(s);

    while (!spectrum_done(s))
    {
        const uint32_t hz = spectrum_binFreq(s, s->index);
        spectrum_store(s, syntheticLevel(peaks, count, hz, s->scan.step));
    }
}

} // namespace

TEST_CASE("The raster is absolute and centred on a bin", "[spectrum]")
{
    spectrum_t s;
    spectrum_reset(&s);

    REQUIRE(spectrum_configure(&s, 145500000U, 1000000U, 25000U));

    REQUIRE(s.scan.step == 25000U);
    REQUIRE(s.scan.bins == 41);              /* 1 MHz, 25 kHz, +-20 bins */
    REQUIRE(s.scan.centre == 145500000U);
    REQUIRE(s.scan.low == 145000000U);
    REQUIRE(s.scan.high == 146000000U);
    REQUIRE(s.scan.span == 1000000U);

    /* Every bin sits on a multiple of the step: two sweeps are comparable */
    for (uint16_t i = 0; i < s.scan.bins; i++)
        REQUIRE((spectrum_binFreq(&s, i) % 25000U) == 0);

    /* And the centre of the span is a bin of it */
    REQUIRE(spectrum_binFreq(&s, s.scan.bins / 2) == 145500000U);
}

TEST_CASE("A step that does not fit the panel is raised", "[spectrum]")
{
    spectrum_t s;
    spectrum_reset(&s);

    /*
     * 5 MHz at 12.5 kHz would be 401 bins: the panel is 160 pixels wide, so the
     * engine serves the span with a coarser step instead of hiding bins.
     */
    REQUIRE(spectrum_configure(&s, 145500000U, 5000000U, 12500U));

    REQUIRE(s.scan.bins <= SPECTRUM_MAX_BINS);
    REQUIRE(s.scan.step == 50000U);
    REQUIRE(s.scan.bins == 101);

    /* A span of a few steps is widened to at least three bins */
    REQUIRE(spectrum_configure(&s, 145500000U, 10000U, 25000U));
    REQUIRE(s.scan.bins == 3);
}

TEST_CASE("The centre snaps to the raster", "[spectrum]")
{
    spectrum_t s;
    spectrum_reset(&s);

    /* 145.512 MHz is not a multiple of 25 kHz: the raster moves it to 145.500 */
    REQUIRE(spectrum_configure(&s, 145512000U, 100000U, 25000U));
    REQUIRE(s.scan.centre == 145500000U);
    REQUIRE(s.scan.low == 145450000U);
    REQUIRE(s.scan.high == 145550000U);

    /* A frequency maps to the bin that contains it, both ends clamped */
    REQUIRE(spectrum_binOf(&s, 145450000U) == 0);
    REQUIRE(spectrum_binOf(&s, 145463000U) == 0);
    REQUIRE(spectrum_binOf(&s, 145487000U) == 1);
    REQUIRE(spectrum_binOf(&s, 145550000U) == 4);
    REQUIRE(spectrum_binOf(&s, 100000000U) == 0);
    REQUIRE(spectrum_binOf(&s, 999000000U) == 4);
}

TEST_CASE("A synthetic carrier lands in its own bin", "[spectrum]")
{
    spectrum_t s;
    spectrum_reset(&s);
    REQUIRE(spectrum_configure(&s, 145500000U, 1000000U, 25000U));

    const carrier peaks[] = {
        { 145525000U, -70 },   /* 145.0 + 21 x 25 kHz */
        { 145400000U, -90 },   /* 145.0 + 16 x 25 kHz */
    };

    sweep(&s, peaks, 2);

    REQUIRE(s.valid);
    REQUIRE(s.sweeps == 1);

    /* The strongest carrier is the peak, with its frequency */
    REQUIRE(spectrum_peak(&s) == -70);
    REQUIRE(spectrum_peakFreq(&s) == 145525000U);

    /* Its bin carries its level, and its skirt falls 15 dB per bin */
    const uint16_t bin = spectrum_binOf(&s, 145525000U);
    REQUIRE(spectrum_level(&s, bin) == -70);
    REQUIRE(spectrum_level(&s, (uint16_t)(bin + 1)) == -85);
    REQUIRE(spectrum_level(&s, (uint16_t)(bin + 2)) == -100);

    /* A bin far from every carrier is the noise floor */
    REQUIRE(spectrum_level(&s, 0) == FLOOR_DBM);

    /* The weaker carrier is right too */
    REQUIRE(spectrum_level(&s, spectrum_binOf(&s, 145400000U)) == -90);

    /* A second sweep counts, and the peak follows the signal that is there */
    const carrier only[] = { { 145400000U, -90 } };
    sweep(&s, only, 1);

    REQUIRE(s.sweeps == 2);
    REQUIRE(spectrum_peakFreq(&s) == 145400000U);
    REQUIRE(spectrum_level(&s, spectrum_binOf(&s, 145525000U)) == FLOOR_DBM);
}

TEST_CASE("The maximum hold keeps what went away", "[spectrum]")
{
    spectrum_t s;
    spectrum_reset(&s);
    REQUIRE(spectrum_configure(&s, 145500000U, 200000U, 25000U));

    const carrier peaks[] = { { 145500000U, -60 } };
    sweep(&s, peaks, 1);

    spectrum_setHold(&s, true);
    REQUIRE(s.holdOn);

    /* The carrier disappears: the trace drops, the hold does not */
    sweep(&s, nullptr, 0);

    const uint16_t bin = spectrum_binOf(&s, 145500000U);
    REQUIRE(spectrum_level(&s, bin) == FLOOR_DBM);
    REQUIRE(spectrum_holdLevel(&s, bin) == -60);

    /* The hold is per bin: a bin that never had a carrier stays at the floor */
    REQUIRE(spectrum_holdLevel(&s, 0) == FLOOR_DBM);

    /* Clearing it starts again from the trace that is on screen */
    spectrum_clearHold(&s);
    REQUIRE(spectrum_holdLevel(&s, bin) == FLOOR_DBM);

    /* And switching it off stops tracking: a new carrier is not held */
    spectrum_setHold(&s, false);
    sweep(&s, peaks, 1);
    REQUIRE(spectrum_holdLevel(&s, bin) == FLOOR_DBM);
    REQUIRE(spectrum_level(&s, bin) == -60);
}

TEST_CASE("Smoothing averages the sweeps and nothing else", "[spectrum]")
{
    spectrum_t s;
    spectrum_reset(&s);
    REQUIRE(spectrum_configure(&s, 145500000U, 200000U, 25000U));

    const carrier peaks[] = { { 145500000U, -60 } };
    const uint16_t bin = spectrum_binOf(&s, 145500000U);

    /* Off by default: one sweep is one reading */
    sweep(&s, peaks, 1);
    REQUIRE(spectrum_level(&s, bin) == -60);

    /* Four-sweep average: the first reading is raw, then it converges */
    spectrum_setSmooth(&s, 2);
    sweep(&s, nullptr, 0);
    REQUIRE(spectrum_level(&s, bin) < -60);
    REQUIRE(spectrum_level(&s, bin) > FLOOR_DBM);

    for (int i = 0; i < 12; i++)
        sweep(&s, nullptr, 0);

    REQUIRE(spectrum_level(&s, bin) <= (FLOOR_DBM + 1));
}

TEST_CASE("A level becomes the row of the plot", "[spectrum]")
{
    const int16_t top = 15;
    const int16_t height = 90;

    /* The window is fixed: -30 dBm on the top row, -130 on the bottom one */
    REQUIRE(ui_spectrum_levelToY(UI_SPEC_DB_TOP, top, height) == top);
    REQUIRE(ui_spectrum_levelToY(0, top, height) == top);
    REQUIRE(ui_spectrum_levelToY(UI_SPEC_DB_BOTTOM, top, height)
            == (top + height - 1));
    REQUIRE(ui_spectrum_levelToY(-160, top, height) == (top + height - 1));

    /* Linear in dB: -80 dBm is half way down a 100 dB window (+-1 row) */
    const int16_t mid = ui_spectrum_levelToY(-80, top, height);
    REQUIRE(mid >= (top + height / 2 - 1));
    REQUIRE(mid <= (top + height / 2 + 1));

    /* Monotonic: a stronger signal is never lower on the screen */
    int16_t previous = -1;

    for (int16_t dbm = UI_SPEC_DB_TOP; dbm >= UI_SPEC_DB_BOTTOM; dbm--)
    {
        const int16_t y = ui_spectrum_levelToY(dbm, top, height);
        REQUIRE(y >= previous);
        REQUIRE(y >= top);
        REQUIRE(y < (top + height));
        previous = y;
    }
}

TEST_CASE("A new raster throws the old trace away", "[spectrum]")
{
    spectrum_t s;
    spectrum_reset(&s);
    REQUIRE(spectrum_configure(&s, 145500000U, 200000U, 25000U));

    const carrier peaks[] = { { 145500000U, -60 } };
    sweep(&s, peaks, 1);
    REQUIRE(s.valid);

    /* Same raster, same trace: the screen keeps it while nothing changes */
    REQUIRE(spectrum_configure(&s, 145500000U, 200000U, 25000U));
    REQUIRE(s.valid);

    /* Another span is another set of frequencies */
    REQUIRE(spectrum_configure(&s, 145500000U, 500000U, 25000U));
    REQUIRE_FALSE(s.valid);
    REQUIRE(spectrum_level(&s, 0) == -160);
}
