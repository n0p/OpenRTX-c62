/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "core/spectrum.h"

/*
 * The step ladder. A step that is asked for is moved up to the next entry: the
 * analyser is a receiver, and a step that is not a channel raster is a step
 * that will never land on a channel. 12.5 and 25 kHz are the rasters the
 * analogue modes use, 50 and 100 kHz the broadcast ones, and the rest exist to
 * keep a wide span inside the 160 bins of the panel.
 */
static const uint32_t stepLadder[] = {
    10000U, 12500U, 25000U, 50000U, 100000U,
    250000U, 500000U, 1000000U, 2500000U, 5000000U
};

static const int16_t NO_LEVEL = -160;

static uint32_t clampStep(uint32_t step)
{
    for (size_t i = 0; i < sizeof(stepLadder) / sizeof(stepLadder[0]); i++)
    {
        if (step <= stepLadder[i])
            return stepLadder[i];
    }

    return stepLadder[(sizeof(stepLadder) / sizeof(stepLadder[0])) - 1];
}

void spectrum_reset(spectrum_t *s)
{
    if (s == NULL)
        return;

    memset(s, 0, sizeof(*s));

    for (uint16_t i = 0; i < SPECTRUM_MAX_BINS; i++)
    {
        s->trace[i] = NO_LEVEL;
        s->hold[i]  = NO_LEVEL;
    }

    s->peakDbm = NO_LEVEL;
}

bool spectrum_configure(spectrum_t *s, uint32_t centre, uint32_t span,
                        uint32_t step)
{
    if ((s == NULL) || (span == 0) || (step == 0))
        return false;

    if (span > SPECTRUM_MAX_SPAN)
        span = SPECTRUM_MAX_SPAN;

    step = clampStep(step);

    /*
     * Raise the step until the span fits the panel. The loop is bounded by the
     * ladder, whose last entry is SPECTRUM_MAX_STEP, so it always ends.
     */
    uint32_t bins = (span / step) + 1;
    size_t   ladder = 0;

    while ((bins > SPECTRUM_MAX_BINS) && (ladder < (sizeof(stepLadder) /
                                                   sizeof(stepLadder[0])) - 1))
    {
        ladder++;
        step = stepLadder[ladder];
        bins = (span / step) + 1;
    }

    if (bins > SPECTRUM_MAX_BINS)
        bins = SPECTRUM_MAX_BINS;

    if (bins < 3)
        bins = 3;

    /* Odd, so that the centre of the span is a bin of the raster */
    if ((bins & 1U) == 0)
        bins++;

    /* The raster is absolute: every bin is a multiple of the step */
    const uint32_t half = (bins / 2) * step;

    if (centre < half)
        centre = half;

    centre = ((centre + (step / 2)) / step) * step;

    if (centre < half)
        centre = half;

    /*
     * A different raster means the old trace is meaningless: bin 7 of the new
     * one is another frequency. Same raster, same trace, so a UI that only
     * changes the span of the plot does not restart the analyser.
     */
    bool sameRaster = (s->scan.bins == bins) && (s->scan.step == step)
                   && (s->scan.centre == centre);

    s->scan.centre = centre;
    s->scan.step   = step;
    s->scan.bins   = (uint16_t)bins;
    s->scan.low    = centre - half;
    s->scan.high   = s->scan.low + ((bins - 1) * step);
    s->scan.span   = s->scan.high - s->scan.low;

    if (sameRaster == false)
    {
        for (uint16_t i = 0; i < SPECTRUM_MAX_BINS; i++)
        {
            s->trace[i] = NO_LEVEL;
            s->hold[i]  = NO_LEVEL;
        }

        s->peakDbm = NO_LEVEL;
        s->peakBin = 0;
        s->valid   = false;
        s->index   = 0;
    }

    return true;
}

uint16_t spectrum_bins(const spectrum_t *s)
{
    return (s != NULL) ? s->scan.bins : 0;
}

uint32_t spectrum_binFreq(const spectrum_t *s, uint16_t bin)
{
    if ((s == NULL) || (bin >= s->scan.bins))
        return 0;

    return s->scan.low + ((uint32_t)bin * s->scan.step);
}

uint16_t spectrum_binOf(const spectrum_t *s, uint32_t freq)
{
    if ((s == NULL) || (s->scan.bins == 0) || (s->scan.step == 0))
        return 0;

    if (freq <= s->scan.low)
        return 0;

    if (freq >= s->scan.high)
        return (uint16_t)(s->scan.bins - 1);

    return (uint16_t)((freq - s->scan.low) / s->scan.step);
}

void spectrum_startSweep(spectrum_t *s)
{
    if (s == NULL)
        return;

    s->index   = 0;
    s->peakDbm = NO_LEVEL;
    s->peakBin = 0;
}

void spectrum_store(spectrum_t *s, int16_t dbm)
{
    if ((s == NULL) || (s->scan.bins == 0) || (s->index >= s->scan.bins))
        return;

    const uint16_t bin = s->index;
    int32_t level = dbm;

    if ((s->smooth > 0) && (s->trace[bin] != NO_LEVEL))
    {
        /*
         * Exponential average of the last 2^smooth sweeps, in dB. Averaging
         * power in dB is what the eye wants on a noise floor: it removes the
         * per-sweep flicker of the RSSI without slowing a carrier's rise more
         * than one sweep.
         */
        int32_t previous = s->trace[bin];
        previous += (level - previous) >> s->smooth;

        if (previous < NO_LEVEL)
            previous = NO_LEVEL;

        level = previous;
    }

    if (level < NO_LEVEL)
        level = NO_LEVEL;

    s->trace[bin] = (int16_t)level;

    if ((level > s->peakDbm) || (s->peakDbm == NO_LEVEL))
    {
        s->peakDbm = (int16_t)level;
        s->peakBin = bin;
    }

    if (s->holdOn && (level > s->hold[bin]))
        s->hold[bin] = (int16_t)level;

    s->index++;

    if (s->index >= s->scan.bins)
    {
        s->index = s->scan.bins;
        s->valid = true;
        s->sweeps++;
    }
}

bool spectrum_done(const spectrum_t *s)
{
    return ((s != NULL) && (s->scan.bins > 0) && (s->index >= s->scan.bins));
}

int16_t spectrum_level(const spectrum_t *s, uint16_t bin)
{
    if ((s == NULL) || (bin >= s->scan.bins))
        return NO_LEVEL;

    return s->trace[bin];
}

int16_t spectrum_holdLevel(const spectrum_t *s, uint16_t bin)
{
    if ((s == NULL) || (bin >= s->scan.bins))
        return NO_LEVEL;

    return s->hold[bin];
}

int16_t spectrum_peak(const spectrum_t *s)
{
    return (s != NULL) ? s->peakDbm : NO_LEVEL;
}

uint32_t spectrum_peakFreq(const spectrum_t *s)
{
    if (s == NULL)
        return 0;

    return spectrum_binFreq(s, s->peakBin);
}

void spectrum_setHold(spectrum_t *s, bool on)
{
    if (s == NULL)
        return;

    s->holdOn = on;

    /* Enabling it starts from what is already on screen, not from nothing */
    if ((on == true) || (s->valid == false))
    {
        for (uint16_t i = 0; i < SPECTRUM_MAX_BINS; i++)
            s->hold[i] = s->trace[i];
    }
}

void spectrum_clearHold(spectrum_t *s)
{
    if (s == NULL)
        return;

    for (uint16_t i = 0; i < SPECTRUM_MAX_BINS; i++)
        s->hold[i] = (s->valid) ? s->trace[i] : NO_LEVEL;
}

void spectrum_setSmooth(spectrum_t *s, uint8_t shift)
{
    if (s == NULL)
        return;

    s->smooth = (shift > 3) ? 3 : shift;
}
