/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/spectrum_service.h"
#include "core/psram_state.h"

#include <stdio.h>
#include <string.h>

#include "core/state.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "rtx/rtx.h"

#if defined(PLATFORM_C62)
/* The only receiver that can be swept: see docs/SPECTRUM.md §2 */
#define SPECTRUM_HW 1
#else
/*
 * Every other build -- the Linux application and the host tests -- has no
 * receiver to sweep. Instead of refusing to work, the sweep answers with a
 * synthetic band (hostLevel() below): that is what lets the screen, its quick
 * menu and the raster arithmetic be exercised, and its scaling checked, on a
 * desktop, which is the software equivalent of a signal generator on the bench.
 */
#define SPECTRUM_HW 0
#endif

/*
 * Time the receiver is given to settle after a PLL write, before the RSSI is
 * believed. Zero means "ask the chip": the driver polls the glitch indicator
 * (REG_63), which saturates while the IF recovers from a retune, and reads the
 * RSSI as soon as it comes down. That is the method of the UV-K5 spectrum
 * firmware (egzumer's GetRssi(), which replaced an 800 us fixed delay with this
 * poll), and it is the only way to wait exactly as long as this particular chip
 * needs instead of as long as the datasheet's worst case.
 *
 * The datasheet figure -- "on power up or channel reselection, the synthesizer
 * takes less than 0.3 msec to settle", BK4819 V1.0 §1.2.3 -- is kept as the
 * ceiling of the gate and as the fallback of the table below, because it is the
 * only number the chip documents and it is the one a sweep that cannot use the
 * gate has to believe. docs/SPECTRUM.md §3 has the measurements.
 */
#ifndef CONFIG_C62_SPECTRUM_SETTLE_US
#define CONFIG_C62_SPECTRUM_SETTLE_US 0
#endif

/** Settling times the quick menu offers, us: 0 is the chip's own indicator. */
static const uint32_t settleTable[] = { 0U, 50U, 100U, 200U, 400U };
#define SPECTRUM_SETTLE_COUNT (sizeof(settleTable) / sizeof(settleTable[0]))

/* How long the receiver listens on the centre frequency between two sweeps. */
#define SPECTRUM_LISTEN_MS 40U

/* Time budget of one sweep pass, so a wide span cannot hold the RTX thread. */
#define SPECTRUM_PASS_MS 30U

/*
 * The last entry is the widest span the engine accepts (SPECTRUM_MAX_SPAN): on
 * the panel it is a band-survey view, 600 kHz per pixel, where a carrier is a
 * bump to zoom into rather than something to measure. It only became usable
 * when the sweep stopped costing 694 us per point: a 96 MHz span needs the whole
 * 160-bin raster, which is what now fits in one pass (docs/SPECTRUM.md §3.3).
 */
static const uint32_t spanTable[SPECTRUM_SPAN_COUNT] = {
    200000U, 500000U, 1000000U, 2000000U,
    5000000U, 10000000U, 20000000U, 50000000U, 96000000U
};

static const char *const spanNames[SPECTRUM_SPAN_COUNT] = {
    "200k", "500k", "1M", "2M", "5M", "10M", "20M", "50M", "96M"
};

static const uint32_t stepTable[] = {
    12500U, 25000U, 50000U, 100000U, 250000U
};

static const char *const stepNames[] = { "12.5k", "25k", "50k", "100k", "250k" };

#define SPECTRUM_STEP_COUNT (sizeof(stepTable) / sizeof(stepTable[0]))

/* --------------------------------------------------------------- the state */

static spectrum_t scan PSRAM_STATE;

static volatile bool     active;      ///< The screen is open
static volatile bool     entered;     ///< The driver holds the RX profile
static volatile bool     restart;     ///< Raster changed, rebuild at the next sweep
static volatile uint32_t centreHz;    ///< Centre requested, Hz
/* 1 MHz of span in 25 kHz bins: 41 of them, the width of the panel at 3 px
   per bar, and a sweep the receiver can do 35 times a second */
static volatile uint8_t  spanIndex = 2;   ///< Entry of spanTable
static volatile uint8_t  stepIndex = 1;   ///< Entry of stepTable
static volatile uint8_t  settleIndex = 0xFF; ///< Entry of settleTable
static volatile uint8_t  smooth;      ///< Sweeps averaged per bin

static bool     parked;               ///< Receiver on the centre, audio on
static int64_t  parkTick;             ///< When it parked
static uint32_t pointCount;           ///< Points swept since the screen opened
static uint32_t pointSumUs;           ///< Sum of their measured times
static uint32_t lastSweepUs;

/* ------------------------------------------------------------------ helpers */

static uint32_t requestedSpan(void)
{
    return spanTable[spanIndex % SPECTRUM_SPAN_COUNT];
}

static uint32_t requestedStep(void)
{
    return stepTable[stepIndex % SPECTRUM_STEP_COUNT];
}

/** Settling time in use, us: 0 means the driver waits on the chip itself. */
static uint32_t settleUs(void)
{
    return settleTable[settleIndex % SPECTRUM_SETTLE_COUNT];
}

/**
 * Table entry closest to the settling time the build was configured with.
 *
 * The quick menu walks the same table, so the configured value only has to pick
 * a starting point; the operator can move from there without a rebuild.
 */
static uint8_t settleDefaultIndex(void)
{
    const uint32_t want = CONFIG_C62_SPECTRUM_SETTLE_US;
    uint8_t best = 0;
    uint32_t bestDiff = 0xFFFFFFFFU;

    for (uint8_t i = 0; i < SPECTRUM_SETTLE_COUNT; i++)
    {
        const uint32_t diff = (settleTable[i] > want) ? (settleTable[i] - want)
                                                     : (want - settleTable[i]);

        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = i;
        }
    }

    return best;
}

/**
 * Keep a span inside one tuning band of the receiver.
 *
 * The C62 tunes 18-660 MHz and 840-1300 MHz and nothing between: a sweep whose
 * span crosses the gap would spend half its bins on frequencies the VCO cannot
 * reach, and it would read them as noise at the bottom of the scale. The span
 * is shrunk to the band the centre belongs to and the centre is moved back
 * inside it, so the trace is always a real measurement.
 */
static void clampToBand(uint32_t *centre, uint32_t *span)
{
    const hwInfo_t *hw = platform_getHwInfo();
    uint32_t low = 0;
    uint32_t high = 0;
    const uint32_t c = *centre;

    if (hw == NULL)
        return;

    const uint32_t vhfLow  = (uint32_t)hw->vhf_minFreq * 1000000UL;
    const uint32_t vhfHigh = (uint32_t)hw->vhf_maxFreq * 1000000UL;
    const uint32_t uhfLow  = (uint32_t)hw->uhf_minFreq * 1000000UL;
    const uint32_t uhfHigh = (uint32_t)hw->uhf_maxFreq * 1000000UL;

    if (hw->vhf_band && (c >= vhfLow) && (c <= vhfHigh))
    {
        low  = vhfLow;
        high = vhfHigh;
    }
    else if (hw->uhf_band && (c >= uhfLow) && (c <= uhfHigh))
    {
        low  = uhfLow;
        high = uhfHigh;
    }
    else if (hw->vhf_band)
    {
        low  = vhfLow;
        high = vhfHigh;
    }
    else if (hw->uhf_band)
    {
        low  = uhfLow;
        high = uhfHigh;
    }
    else
    {
        return;
    }

    if (*span > (high - low))
        *span = high - low;

    const uint32_t half = *span / 2;

    if (*centre < (low + half))
        *centre = low + half;

    if (*centre > (high - half))
        *centre = high - half;
}

static void rebuildRaster(void)
{
    uint32_t centre = centreHz;
    uint32_t span   = requestedSpan();

    clampToBand(&centre, &span);

    if (spectrum_configure(&scan, centre, span, requestedStep()))
    {
        spectrum_setSmooth(&scan, smooth);
        spectrum_setHold(&scan, spectrumService_hold());
    }
}

/* -------------------------------------------------------------------- API */

bool spectrumService_open(uint32_t centre)
{
    if (active)
        return true;

#if SPECTRUM_HW
    /* The analyser needs a receiver that is receiving, and no transmitter */
    const rtxStatus_t status = rtx_getCurrentStatus();

    if ((status.opStatus != RX) || (platform_getPttStatus() != false))
        return false;

    /*
     * A digital mode receives its squelch from the demodulator, which needs
     * every sample of the discriminator: sweeping under it would break the mode
     * for as long as the screen is open. The analyser is an analogue
     * instrument, so it only runs in the analogue receive modes.
     */
    if ((status.opMode != OPMODE_FM) && (status.opMode != OPMODE_AIRBAND))
        return false;
#endif

    spectrum_reset(&scan);

    centreHz  = centre;
    spanIndex = (spanIndex < SPECTRUM_SPAN_COUNT) ? spanIndex : 2U;
    stepIndex = (stepIndex < SPECTRUM_STEP_COUNT) ? stepIndex : 1U;
    smooth    = (smooth > 3) ? 3 : smooth;

    if (settleIndex >= SPECTRUM_SETTLE_COUNT)
        settleIndex = settleDefaultIndex();

    rebuildRaster();

    pointCount  = 0;
    pointSumUs  = 0;
    lastSweepUs = 0;
    parked      = false;
    restart     = false;
    entered     = false;
    active      = true;

    spectrum_startSweep(&scan);

    return true;
}

void spectrumService_close(void)
{
    active = false;
}

bool spectrumService_active(void)
{
    return active;
}

const spectrum_t *spectrumService_trace(void)
{
    return &scan;
}

void spectrumService_status(spectrumStatus_t *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));

    out->centre    = scan.scan.centre;
    out->span      = scan.scan.span;
    out->step      = scan.scan.step;
    out->low       = scan.scan.low;
    out->high      = scan.scan.high;
    out->bins      = scan.scan.bins;
    out->index     = scan.index;
    out->peakDbm   = spectrum_peak(&scan);
    out->peakFreq  = spectrum_peakFreq(&scan);
    out->sweeps    = scan.sweeps;
    out->valid     = scan.valid;
    out->hold      = scan.holdOn;
    out->paused    = parked && active;
    out->sweepUs   = lastSweepUs;
    out->pointUs   = (pointCount > 0) ? (pointSumUs / pointCount) : 0;
    out->settleUs  = settleUs();
    out->gateOn    = true;
    out->gatePolls = 0;

#if SPECTRUM_HW
    /* What the driver had to wait for: its own indicator, or the clock */
    out->gateOn = radio_spectrumGate(&out->gatePolls);
#endif

    out->tuned     = spectrum_binFreq(&scan, (scan.index < scan.scan.bins)
                                                  ? scan.index
                                                  : (uint16_t)(scan.scan.bins - 1));
}

/**
 * Take the receiver over: mute it, widen the IF to the widest filter the chip
 * has and keep it receiving. Runs in the RTX thread, which is the owner of the
 * radio.
 */
static void enterReceiver(void)
{
    if (entered)
        return;

#if SPECTRUM_HW
    if (radio_spectrumEnter() == false)
        return;
#endif


    entered = true;
    parked  = false;
}

/** Give the receiver back: filters, audio and frequency as they were. */
static void leaveReceiver(void)
{
    if (!entered)
        return;

#if SPECTRUM_HW
    radio_spectrumLeave();
#endif

    entered = false;
    parked  = false;
}

#if SPECTRUM_HW == 0
/**
 * The band a build without a receiver sweeps.
 *
 * Three carriers at fixed offsets from the centre of the span, with the 15 dB
 * per bin skirt of one IF filter, over a floor that moves a little with the bin
 * so the trace looks like a trace. Deterministic on purpose: the host tests
 * assert on it.
 */
static int16_t hostLevel(uint32_t hz)
{
    static const struct { int32_t offset; int16_t dbm; } carriers[] = {
        { -325000, -62 },   /* 145.175 MHz on the 25 kHz raster */
        {       0, -78 },   /* the centre                        */
        {  425000, -95 },
    };

    int16_t level = (int16_t)(-101 + (int16_t)((hz / 25U) % 3U));

    for (size_t i = 0; i < sizeof(carriers) / sizeof(carriers[0]); i++)
    {
        const int32_t d = (int32_t)hz - ((int32_t)scan.scan.centre + carriers[i].offset);
        const uint32_t bins = (uint32_t)((d < 0) ? (-d) : d) / (scan.scan.step ? scan.scan.step : 1U);

        if (bins > 3)
            continue;

        const int16_t candidate = (int16_t)(carriers[i].dbm - (int16_t)(15U * bins));

        if (candidate > level)
            level = candidate;
    }

    return level;
}
#endif

/**
 * Sweep up to the end of the raster or the pass budget.
 *
 * @return true if the sweep finished in this pass.
 */
static bool sweepPass(void)
{
    if (restart)
    {
        restart = false;
        rebuildRaster();
        spectrum_startSweep(&scan);
    }

    const int64_t start = getTick();

    while (spectrum_done(&scan) == false)
    {
        const uint16_t bin = scan.index;
        const uint32_t freq = spectrum_binFreq(&scan, bin);
        uint32_t pointUs = 0;
        int16_t dbm;

#if SPECTRUM_HW
        dbm = radio_spectrumPoint(freq, settleUs(), &pointUs);
#else
        dbm = hostLevel(freq);
        pointUs = 0;
#endif

        spectrum_store(&scan, dbm);

        pointCount++;
        pointSumUs += pointUs;

        if ((getTick() - start) >= SPECTRUM_PASS_MS)
            break;
    }

    return spectrum_done(&scan);
}

bool spectrumService_poll(void)
{
    if (!active)
    {
        leaveReceiver();
        return false;
    }

    if (!entered)
    {
        enterReceiver();
        if (!entered)
        {
            /* The driver refused: do not sweep a receiver we do not own */
            active = false;
            return false;
        }

        spectrum_startSweep(&scan);
    }

    if (parked)
    {
        /*
         * The receiver is on the centre frequency with its audio open, exactly
         * as it was before the screen opened, and the normal RTX path feeds the
         * mode's squelch. Two reasons to stay parked: the listen window has not
         * elapsed, or there is a carrier on the centre -- which is the pause
         * the screen shows.
         */
        if ((getTick() - parkTick) < SPECTRUM_LISTEN_MS)
            return false;

        if (rtx_rxSquelchOpen())
            return false;

        /* No carrier: the receiver is ours again, and it goes quiet */
#if SPECTRUM_HW
        radio_spectrumSweep(true);
#endif

        parked = false;
        spectrum_startSweep(&scan);
        return true;
    }

    /* Timed with the tick (100 us at 10 kHz) and averaged over the sweep */
    const int64_t sweepStart = getTick();

    bool done = sweepPass();

    if (done == false)
        return true;

    lastSweepUs = (uint32_t)((getTick() - sweepStart) * 1000);

    /*
     * Park: back to the centre, audio open, and the normal RTX path runs from
     * here, so the mode's squelch decides what is heard on the centre
     * frequency exactly as it does outside the analyser.
     */
#if SPECTRUM_HW
    radio_spectrumPark();
#endif

    parked   = true;
    parkTick = getTick();

    return true;
}

/* ---- quick menu --------------------------------------------------------- */

uint8_t spectrumService_spanIndex(void)
{
    return spanIndex % SPECTRUM_SPAN_COUNT;
}

void spectrumService_setSpanIndex(uint8_t index)
{
    if (index >= SPECTRUM_SPAN_COUNT)
        index = SPECTRUM_SPAN_COUNT - 1;

    if (index == spanIndex)
        return;

    spanIndex = index;
    restart   = true;
}

uint8_t spectrumService_stepIndex(void)
{
    return stepIndex % SPECTRUM_STEP_COUNT;
}

void spectrumService_setStepIndex(uint8_t index)
{
    if (index >= SPECTRUM_STEP_COUNT)
        index = SPECTRUM_STEP_COUNT - 1;

    if (index == stepIndex)
        return;

    stepIndex = index;
    restart   = true;
}

uint32_t spectrumService_effectiveStep(void)
{
    return scan.scan.step;
}

bool spectrumService_hold(void)
{
    return scan.holdOn;
}

uint8_t spectrumService_settleIndex(void)
{
    return settleIndex % SPECTRUM_SETTLE_COUNT;
}

void spectrumService_setSettleIndex(uint8_t index)
{
    if (index >= SPECTRUM_SETTLE_COUNT)
        index = SPECTRUM_SETTLE_COUNT - 1;

    settleIndex = index;
}

uint32_t spectrumService_settleUs(void)
{
    return settleUs();
}

void spectrumService_setHold(bool on)
{
    spectrum_setHold(&scan, on);
}

void spectrumService_clearHold(void)
{
    spectrum_clearHold(&scan);
}

void spectrumService_resetPeak(void)
{
    spectrum_startSweep(&scan);
}

void spectrumService_centreOn(uint32_t freq)
{
    centreHz = freq;
    restart  = true;
}

void spectrumService_nudge(int32_t steps)
{
    const uint32_t step = scan.scan.step;

    if (step == 0)
        return;

    int64_t freq = (int64_t)centreHz + ((int64_t)steps * (int64_t)step);

    if (freq < 0)
        freq = 0;

    centreHz = (uint32_t)freq;
    restart  = true;
}

const char *spectrumService_spanName(uint8_t index)
{
    return spanNames[index % SPECTRUM_SPAN_COUNT];
}

const char *spectrumService_stepName(uint8_t index)
{
    return stepNames[index % SPECTRUM_STEP_COUNT];
}

bool spectrumService_traceLine(char *buf, size_t len)
{
    if ((buf == NULL) || (len == 0) || (scan.scan.bins == 0) || (!scan.valid))
        return false;

    int n = snprintf(buf, len, "sp %lu %lu %u",
                     (unsigned long)scan.scan.centre,
                     (unsigned long)scan.scan.step,
                     (unsigned)scan.scan.bins);

    if ((n <= 0) || ((size_t)n >= len))
        return false;

    for (uint16_t i = 0; i < scan.scan.bins; i++)
    {
        int w = snprintf(buf + n, len - (size_t)n, " %d",
                         (int)spectrum_level(&scan, i));

        if ((w <= 0) || ((size_t)(n + w) >= len))
            return false;

        n += w;
    }

    return true;
}
