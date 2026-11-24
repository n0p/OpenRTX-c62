/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/vfo_search_service.h"

#include <string.h>

#include "core/airband.h"
#include "core/scan_service.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/wfm.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "rtx/ModeReport.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"

/*
 * The radio side of the carrier search.  Everything here exists to keep
 * core/vfo_search.h free of the firmware:
 *
 *   - the squelch is rtx_rxSquelchOpen(), the analog detector in FM and AM;
 *   - the digital side is modeSyncCounters() (rtx/ModeReport.h), which reads
 *     the per-mode status modules: sync plus the frames whose check passed;
 *   - the frequency correction is modeReportedFrequency(), the generic hook a
 *     mode implements when its air interface carries a frequency (TETRA);
 *   - a retune is state.channel plus ui_rtxSyncRequest, the same door the
 *     quick menu and the scan use, so the UI thread assembles the RTX
 *     configuration and the mode reinitialises itself.  There is no second
 *     place that knows what a channel means;
 *   - the band swept is the plan the frequency belongs to (2 m, 70 cm, PMR446,
 *     23 cm, the airband, broadcast FM) and, for everything else, the tuning
 *     sub-band of the receiver that contains it.
 *
 * The search never changes the mode: it moves the VFO frequency inside the
 * band of the mode already in use, which is why no mode-specific code is
 * needed here.
 *
 * Locking: this file takes no lock, on purpose.  vfoSearchService_poll() runs
 * in the main thread from state_task(), where state_mutex is free, but
 * vfoSearchService_start() and vfoSearchService_stop() are called from the UI
 * thread *inside* the state_mutex it holds around ui_updateFSM() -- taking it
 * again there would deadlock on a non-recursive mutex.  The fields written are
 * one aligned word at a time (two frequencies and `ui_rtxSyncRequest`), so a
 * reader under the mutex never sees half of them.
 */

/// After a retune, before the squelch or the sync is believed, ms.  It covers
/// the 25 ms the UI thread takes to pick the request up and the mode's own
/// reinitialisation.
#define VFO_SEARCH_SETTLE_MS       100

/// Time on each frequency before stepping, analog, ms.
#define VFO_SEARCH_DWELL_ANALOG_MS 120

/// Time on each frequency before stepping, digital, ms.
#define VFO_SEARCH_DWELL_DIGITAL_MS 600

/// TETRA drops half a second of samples after a retune before it decodes.
#define VFO_SEARCH_DWELL_TETRA_MS  2500

/// Squelch stability required to believe a change, ms.
#define VFO_SEARCH_DEBOUNCE_MS     20

/// Valid frames that make a digital carrier (a sync word alone is not one).
#define VFO_SEARCH_SYNC_FRAMES     2

/// How long the sync has to hold for those frames to count, ms.
#define VFO_SEARCH_SYNC_HOLD_MS    100

/// Sync time for a digital mode with no frame counter, ms.  Longer than any
/// mode keeps its sync flag set without decoding (300 ms in DMR/NXDN/P25, two
/// frames in YSF), so a carrier left behind on the previous frequency cannot
/// stop the sweep.
#define VFO_SEARCH_SYNC_MS         400

/// Smallest carrier offset worth a retune, Hz.
#define VFO_SEARCH_OFFSET_MIN      200

/// Largest carrier offset accepted as a correction, Hz (the TETRA AFC range).
#define VFO_SEARCH_OFFSET_MAX      4000

/// A published frequency further than this from the tuning is another channel.
#define VFO_SEARCH_REPORTED_MAX    25000U

/// One band the search can sweep.
struct vfoBand
{
    uint32_t low;
    uint32_t high;
};

/*
 * The plans, in Hz.  144-146 and 430-440 MHz are the IARU Region 1 amateur
 * bands (BOE 137/2006 §5.1-5.2, see docs/ZONES.md), 446.0-446.2 is PMR446 and
 * 1240-1300 the 23 cm band; the airband and broadcast FM bring their own limits
 * from core/airband.h and core/wfm.h.  A frequency outside all of them sweeps
 * the receiver sub-band it is in (VHF 18-660 MHz or UHF 840-1300 MHz), which
 * is what CONFIG_C62_UNLOCKED_TUNING opens.
 */
static const struct vfoBand bands[] = {
    { 144000000U, 146000000U },     /* 2 m                                    */
    { 430000000U, 440000000U },     /* 70 cm                                  */
    { 446000000U, 446200000U },     /* PMR446                                 */
    { 1240000000U, 1300000000U }    /* 23 cm                                  */
};

static vfoSearch_t engine;
static uint8_t     searchMode;      ///< Mode in use when the search began
static int32_t     searchShift;     ///< TX - RX of the VFO when it began
static uint32_t    bandLow;
static uint32_t    bandHigh;
static uint32_t    bandStep;
static uint32_t    lastStop;        ///< Tick of the last stop, for the message
static bool        syncPending;     ///< A retune owed once the call ends

/* ------------------------------------------------------------- the receiver */

static bool txActive(void)
{
    return rtx_getCurrentStatus().opStatus == TX;
}

/** Analog modes stop on the squelch; every other mode is a digital one. */
static bool isDigital(uint8_t mode)
{
    switch (mode) {
        case OPMODE_NONE:
        case OPMODE_FM:
        case OPMODE_AIRBAND:
        case OPMODE_WFM_BC:
            return false;

        default:
            return true;
    }
}

/**
 * One sample of the receiver, in the terms the machine asks.
 *
 * The digital synchronism comes from the mode's status module; M17 and YSF
 * report theirs through rxSquelchOpen(), so that is the union.  Neither
 * publishes a frame counter, and neither does dPMR or D-STAR: for them
 * `haveFrames` stays false and the machine uses the time criterion.
 */
static void sampleRadio(uint8_t mode, vfoSearchSample_t *s)
{
    memset(s, 0, sizeof(*s));

    s->squelch = rtx_rxSquelchOpen();

    struct modeCarrierReport report;
    if (modeSyncCounters(mode, &report)) {
        s->synced     = report.synced;
        s->haveFrames = report.haveFrames;
        s->frames     = report.validFrames;
    }

    if (isDigital(mode) && !s->synced && s->squelch)
        s->synced = true;

    struct modeReportedFrequency published;
    if (modeReportedFrequency(mode, &published)) {
        s->reported    = published.absolute;
        s->reportedHz  = published.frequency;
        s->offsetValid = published.offsetValid;
        s->offsetHz    = published.offsetHz;
    }
}

/* ---------------------------------------------------------------- the radio */

static void tune(uint32_t frequency)
{
    state.channel.rx_frequency = frequency;
    /* The VFO keeps the shift it had: the search moves RX and TX together */
    state.channel.tx_frequency = (uint32_t)((int32_t)frequency + searchShift);

    ui_rtxSyncRequest = true;
}

/**
 * Carry out what the machine asked for.
 *
 * A retune goes through state.channel + ui_rtxSyncRequest, so the receiver is
 * left in a complete configuration at every step and at the end: the search
 * never leaves a stream open or a half-applied profile behind, because every
 * frequency it visits is a full RTX configuration built by threads.c.
 */
static void apply(const vfoSearchAction_t *a)
{
    if (a->target != VFOS_TARGET_FREQUENCY)
        return;

    if (txActive()) {
        /* Never rebuild the configuration in the middle of a call */
        syncPending = true;
        return;
    }

    tune(a->frequency);
}

/* ---------------------------------------------------------------- the band */

/**
 * Limits of the band that contains a frequency.
 *
 * @return false when the frequency is in no band at all (the 660-840 MHz gap
 *         of the BK4819), where a sweep has nowhere to go.
 */
static bool bandLimits(uint8_t mode, uint32_t freq, uint32_t *low,
                       uint32_t *high)
{
    if ((mode == OPMODE_AIRBAND) || airband_inBand(freq)) {
        *low  = AIRBAND_FREQ_MIN;
        *high = AIRBAND_FREQ_MAX;
        return true;
    }

    if (mode == OPMODE_WFM_BC) {
        *low  = WFM_LOW_HZ;
        *high = WFM_HIGH_HZ;
        return true;
    }

    for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
        if ((freq >= bands[i].low) && (freq <= bands[i].high)) {
            *low  = bands[i].low;
            *high = bands[i].high;
            return true;
        }
    }

    /* Everything else: the receiver sub-band the frequency falls in */
    const hwInfo_t *hw = platform_getHwInfo();
    uint32_t vhfLow  = (uint32_t)hw->vhf_minFreq * 1000000U;
    uint32_t vhfHigh = (uint32_t)hw->vhf_maxFreq * 1000000U;
    uint32_t uhfLow  = (uint32_t)hw->uhf_minFreq * 1000000U;
    uint32_t uhfHigh = (uint32_t)hw->uhf_maxFreq * 1000000U;

    if ((freq >= vhfLow) && (freq <= vhfHigh)) {
        *low  = vhfLow;
        *high = vhfHigh;
        return true;
    }

    if ((freq >= uhfLow) && (freq <= uhfHigh)) {
        *low  = uhfLow;
        *high = uhfHigh;
        return true;
    }

    return false;
}

/** The step of the mode in use, snapped to the raster of its band. */
static uint32_t stepFor(uint8_t mode)
{
    uint32_t step = freq_steps[state.step_index % n_freq_steps];

    if (mode == OPMODE_AIRBAND) {
        /* The airband walks its own two rasters and neither of them is FM's */
        if ((step != AIRBAND_SPACING_25K) && (step != 8333U))
            step = AIRBAND_SPACING_25K;
    }
    else if (mode == OPMODE_WFM_BC) {
        if ((step != WFM_STEP_HZ) && (step != WFM_STEP_FINE_HZ))
            step = WFM_STEP_HZ;
    }

    if (step == 0)
        step = 12500U;

    return step;
}

/* --------------------------------------------------------------------- API */

void vfoSearchService_init(void)
{
    vfoSearch_init(&engine);
    searchMode  = OPMODE_FM;
    searchShift = 0;
    bandLow     = 0;
    bandHigh    = 0;
    bandStep    = 12500U;
    lastStop    = (uint32_t)getTick();
    syncPending = false;
}

bool vfoSearchService_start(uint8_t direction)
{
    uint32_t now = (uint32_t)getTick();

    if (vfoSearch_isRunning(&engine))
        return false;

    struct scanStatus scan;
    bool scanning = scan_service_status(&scan) && scan.running;

    /* Only the VFO main screen, never a call, never a running scan */
    if (!vfoSearch_mayStart(state.ui_screen == MAIN_VFO, txActive(), scanning))
        return false;

    uint32_t frequency;
    uint8_t  mode;

    frequency   = state.channel.rx_frequency;
    mode        = state.channel.mode;
    searchShift = (int32_t)state.channel.tx_frequency -
                  (int32_t)state.channel.rx_frequency;

    uint32_t low, high;
    if (!bandLimits(mode, frequency, &low, &high))
        return false;

    vfoSearchConfig_t cfg;
    vfoSearch_defaultConfig(&cfg);
    bool digital = isDigital(mode);

    cfg.direction   = (direction == VFOS_DOWN) ? VFOS_DOWN : VFOS_UP;
    cfg.digital     = digital ? 1 : 0;
    cfg.step        = stepFor(mode);
    cfg.low         = low;
    cfg.high        = high;
    cfg.settleMs    = VFO_SEARCH_SETTLE_MS;
    cfg.debounceMs  = VFO_SEARCH_DEBOUNCE_MS;
    cfg.syncFrames  = VFO_SEARCH_SYNC_FRAMES;
    cfg.syncHoldMs  = VFO_SEARCH_SYNC_HOLD_MS;
    cfg.syncMs      = VFO_SEARCH_SYNC_MS;
    cfg.offsetMin   = VFO_SEARCH_OFFSET_MIN;
    cfg.offsetMax   = VFO_SEARCH_OFFSET_MAX;
    cfg.reportedMax = VFO_SEARCH_REPORTED_MAX;
    cfg.dwellMs     = VFO_SEARCH_DWELL_ANALOG_MS;

    if (digital) {
        cfg.dwellMs = VFO_SEARCH_DWELL_DIGITAL_MS;
        if (mode == OPMODE_TETRA)
            cfg.dwellMs = VFO_SEARCH_DWELL_TETRA_MS;
    }

    /* Applied before anything else: a refused configuration changes nothing */
    if (!vfoSearch_configure(&engine, &cfg))
        return false;

    vfoSearchSample_t sample;
    sampleRadio(mode, &sample);

    vfoSearchAction_t action;
    if (!vfoSearch_start(&engine, frequency, now, &sample, &action))
        return false;

    searchMode = mode;
    bandLow    = low;
    bandHigh   = high;
    bandStep   = cfg.step;

    apply(&action);

    return true;
}

void vfoSearchService_stop(void)
{
    uint32_t now = (uint32_t)getTick();

    vfoSearchAction_t action;
    if (!vfoSearch_stop(&engine, now, &action))
        return;

    lastStop = now;
    apply(&action);
}

void vfoSearchService_poll(void)
{
    uint32_t now = (uint32_t)getTick();

    /* A retune the search owed while a call was in progress */
    if (syncPending && !txActive()) {
        syncPending = false;
        ui_rtxSyncRequest = true;
    }

    if (!vfoSearch_isRunning(&engine))
        return;

    if (txActive()) {
        /*
         * The PTT always stops the sweep, and the receiver stays on the
         * frequency it was listening to: that is the channel the operator
         * wants to talk on.
         */
        vfoSearchAction_t action;
        vfoSearch_stop(&engine, now, &action);
        lastStop = now;
        return;
    }

    vfoSearchSample_t sample;
    sampleRadio(searchMode, &sample);

    vfoSearchAction_t action;
    vfoSearch_step(&engine, &sample, now, &action);

    if ((action.event == VFOS_EV_FOUND) ||
        (action.event == VFOS_EV_CANCELLED) ||
        (action.event == VFOS_EV_BAND_END))
        lastStop = now;

    apply(&action);
}

bool vfoSearchService_active(void)
{
    return vfoSearch_isRunning(&engine);
}

bool vfoSearchService_status(struct vfoSearchStatus *out)
{
    if (out == NULL)
        return false;

    memset(out, 0, sizeof(*out));

    out->active    = vfoSearch_isRunning(&engine) ? true : false;
    out->state     = vfoSearch_state(&engine);
    out->direction = engine.cfg.direction;
    out->result    = vfoSearch_result(&engine);
    out->frequency = vfoSearch_frequency(&engine);
    out->offsetHz  = vfoSearch_offset(&engine);
    out->corrected = (out->offsetHz != 0);
    out->visited   = vfoSearch_visited(&engine);
    out->age       = out->active ? 0
                                 : (uint32_t)getTick() - lastStop;

    /* Tenths of a percent is all the screen can show and all it needs */
    if ((bandHigh > bandLow) && (bandStep != 0)) {
        uint32_t span = (bandHigh - bandLow) / bandStep + 1;
        uint32_t done = out->visited;
        out->progress = (uint16_t)((done >= span)
                                   ? 1000U
                                   : (done * 1000U) / span);
    }

    return true;
}

const char *vfoSearchService_reasonName(uint8_t result, bool corrected)
{
    /* A search that ended by retuning to the network's own frequency says so
     * instead of "sync": that correction is the interesting part */
    if (corrected &&
        ((result == VFOS_RESULT_CARRIER) || (result == VFOS_RESULT_SYNC)))
        return "net";

    switch (result) {
        case VFOS_RESULT_CARRIER:   return "carrier";
        case VFOS_RESULT_SYNC:      return "sync";
        case VFOS_RESULT_CANCELLED: return "cancel";
        case VFOS_RESULT_BAND_END:  return "band end";
        default:                    return "-";
    }
}
