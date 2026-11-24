/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/scan_service.h"

#include <string.h>

#include "core/channel_store.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/utils.h"
#include "interfaces/delays.h"
#include "rtx/rtx.h"

/*
 * The radio side of the scan.  Everything here exists to keep core/scanner.h
 * free of the firmware:
 *
 *   - the list the machine walks is the channel bank, read through
 *     chstore_get() (two reads per step, none while listening);
 *   - the squelch is rtx_rxSquelchOpen(), which is the analog squelch in FM and
 *     the carrier/sync detector in DMR (dmrSpeech_playing / dmr_demod) and
 *     M17 (LSF valid);
 *   - a retune is a channel of state.channel plus ui_rtxSyncRequest, the same
 *     door the quick menu and the remote control use, so the UI thread builds
 *     the RTX configuration and the mode reinitialises itself;
 *   - the transmit state is rtx_getCurrentStatus().opStatus: a scan never gets
 *     in the way of a call.
 *
 * Why the retune goes through state.channel and not rtx_configure() directly:
 * the RTX configuration carries the mode, the tones, the DMR colour code and
 * destination, the power and the TX frequency, and threads.c is the one place
 * that assembles it from the state.  Duplicating that here would mean a second
 * copy of the rules that decide what a channel means, which is exactly the bug
 * the channel store was written to avoid.
 */

/// Depth of the event queue the interface drains.
#define SCAN_EVENT_QUEUE 8

/// How long the receiver is given before the squelch is believed, ms.
#define SCAN_SETTLE_MS 60

/// Squelch stability required to believe a change (chatter filter), ms.
#define SCAN_DEBOUNCE_MS 20

/// Fallback VFO range when the settings carry something unusable.
#define SCAN_VFO_LOW_DEFAULT  144000000U
#define SCAN_VFO_HIGH_DEFAULT 146000000U

/// Where the scan came from, to put everything back when it stops.
struct scanHome
{
    channel_t channel;
    uint16_t  index;
    uint8_t   tunerMode;
    int32_t   shift;    ///< TX - RX of the channel the scan started from
    uint8_t   dmrType;
    uint32_t  dmrId;
    uint8_t   dmrTs;
    bool      onBank;   ///< The radio was on a channel, not on the VFO
    bool      valid;
};

static scanner_t engine;

static struct scanHome home;
static bool restorePending;   ///< A stop owes the receiver a retune

static struct scanEventMsg events[SCAN_EVENT_QUEUE];
static uint8_t eventHead;     ///< Next to read
static uint8_t eventCount;

/* What the radio is tuned to, kept for the status and the events */
static uint16_t currentChannel   = SCAN_CHANNEL_NONE;
static uint32_t currentFrequency = 0;
static uint8_t  currentMode      = OPMODE_FM;

/* --------------------------------------------------------------- the list */

static int bankCount(void *ctx)
{
    (void)ctx;
    return chstore_count();
}

static int bankInfo(void *ctx, uint16_t idx, scanEntry_t *out)
{
    (void)ctx;

    chstore_channel_t ch;
    if (chstore_get(idx, &ch) != 0)
        return -1;

    out->frequency = ch.rx_freq;
    out->mode      = ch.mode;
    out->skip      = (ch.flags & CHSTORE_FLAG_SKIP) ? 1 : 0;

    return 0;
}

static const scanList_t bankList = {
    .count = bankCount,
    .info  = bankInfo,
    .ctx   = NULL
};

/* ---------------------------------------------------------- radio sampling */

static bool txActive(void)
{
    return rtx_getCurrentStatus().opStatus == TX;
}

/**
 * Any indication of activity on the frequency being scanned.
 *
 * The squelch of the mode in use is the answer for FM, DMR and M17, which
 * publish one (the analog squelch, the DMR sync and the M17 LSF).  TETRA
 * reports no carrier detect at all, so it falls back to the RSSI against the
 * operator's squelch level: it is an approximation (it does not check the
 * channel coding) and it is the reason a TETRA scan may stop on noise that is
 * as strong as a call.
 */
static bool signalPresent(void)
{
    if (rtx_rxSquelchOpen())
        return true;

    if (currentMode == OPMODE_TETRA)
    {
        rssi_t threshold = sLevelToRssi(state.settings.sqlLevel);
        return rtx_getRssi() > threshold;
    }

    return false;
}

/* --------------------------------------------------------------- the radio */

static void noteChannel(uint16_t idx, const channel_t *ch)
{
    currentChannel   = idx;
    currentFrequency = ch->rx_frequency;
    currentMode      = ch->mode;
}

static void tuneChannel(uint16_t idx)
{
    channel_t ch;

    if (chstore_toChannel(idx, &ch) != 0)
        return;

    pthread_mutex_lock(&state_mutex);
    state.channel       = ch;
    state.channel_index = idx;

    /*
     * The DMR call destination has no field in channel_t: it reaches the RTX
     * through the settings, and the channel store is the one that knows how to
     * copy it.  This only touches RAM; the settings debouncer in state_task()
     * is what decides whether it reaches flash, and a scan that comes back to
     * the channel it started from leaves the record as it was.
     */
    if (ch.mode == OPMODE_DMR)
        (void)chstore_applySettings(idx, &state.settings);

    pthread_mutex_unlock(&state_mutex);

    noteChannel(idx, &ch);
    ui_rtxSyncRequest = true;
}

static void tuneFrequency(uint32_t frequency)
{
    pthread_mutex_lock(&state_mutex);
    state.channel.rx_frequency = frequency;
    state.channel.tx_frequency = (uint32_t)((int32_t)frequency + home.shift);
    pthread_mutex_unlock(&state_mutex);

    currentChannel   = SCAN_CHANNEL_NONE;
    currentFrequency = frequency;
    ui_rtxSyncRequest = true;
}

/**
 * Take the channel the scan stopped on as the new "home": pressing PTT on a
 * signal keeps the operator on that channel, which is the whole point of
 * stopping there.
 */
static void adoptCurrent(void)
{
    pthread_mutex_lock(&state_mutex);
    home.channel   = state.channel;
    home.index     = state.channel_index;
    home.shift     = (int32_t)state.channel.tx_frequency -
                     (int32_t)state.channel.rx_frequency;
    home.dmrType   = state.settings.dmr_dest_type;
    home.dmrId     = state.settings.dmr_dest_id;
    home.dmrTs     = state.settings.dmr_timeslot;
    home.onBank    = state.bank_enabled;
    pthread_mutex_unlock(&state_mutex);

    home.valid = true;
}

/** Put back the channel, the frequency and the mode the scan started from. */
static void finishRestore(void)
{
    restorePending = false;
    if (!home.valid)
        return;

    pthread_mutex_lock(&state_mutex);
    state.channel       = home.channel;
    state.channel_index = home.index;
    state.tuner_mode    = home.tunerMode;
    state.settings.dmr_dest_type = home.dmrType;
    state.settings.dmr_dest_id   = home.dmrId;
    state.settings.dmr_timeslot  = home.dmrTs;
    pthread_mutex_unlock(&state_mutex);

    currentChannel   = home.onBank ? home.index : SCAN_CHANNEL_NONE;
    currentFrequency = home.channel.rx_frequency;
    currentMode      = home.channel.mode;
    ui_rtxSyncRequest = true;
}

/* -------------------------------------------------------------- the events */

static void pushEvent(const scanAction_t *a)
{
    if (a->event == SCAN_EVENT_NONE)
        return;

    if (eventCount >= SCAN_EVENT_QUEUE)
    {
        /* Drop the oldest: a display that cannot keep up must not grow */
        eventHead = (uint8_t)((eventHead + 1) % SCAN_EVENT_QUEUE);
        eventCount--;
    }

    uint8_t slot = (uint8_t)((eventHead + eventCount) % SCAN_EVENT_QUEUE);
    events[slot].event     = a->event;
    events[slot].channel   = (a->channel != SCAN_CHANNEL_NONE) ? a->channel
                                                              : currentChannel;
    events[slot].frequency = (a->frequency != 0) ? a->frequency
                                                 : currentFrequency;
    eventCount++;
}

/* ------------------------------------------------------------------ config */

static void configFromSettings(scanConfig_t *cfg, uint8_t mode)
{
    const settings_t *st = &state.settings;

    scan_defaultConfig(cfg);

    cfg->mode      = (mode == SCAN_MODE_VFO) ? SCAN_MODE_VFO : SCAN_MODE_MEMORY;
    cfg->resume    = settings_scanResumeValid(st->scan_resume)
                   ? st->scan_resume : (uint8_t)SCAN_RESUME_CARRIER;
    cfg->direction = (st->scan_flags & SETTINGS_SCAN_DOWN) ? SCAN_DOWN : SCAN_UP;
    cfg->loop      = (st->scan_flags & SETTINGS_SCAN_VFO_LOOP) ? 1 : 0;

    cfg->priorityEnabled = (st->scan_flags & SETTINGS_SCAN_PRIORITY) ? 1 : 0;
    if (st->scan_priority == 0)
        cfg->priority = SCAN_CHANNEL_NONE;
    else
    {
        uint16_t idx = (uint16_t)(st->scan_priority - 1);
        cfg->priority = (idx < (uint16_t)chstore_count()) ? idx : SCAN_CHANNEL_NONE;
    }

    cfg->settleMs   = SCAN_SETTLE_MS;
    cfg->debounceMs = SCAN_DEBOUNCE_MS;

    cfg->dwellMs = (st->scan_dwell != 0) ? (uint16_t)(st->scan_dwell * 10U)
                                         : (uint16_t)150;

    /*
     * One knob for the resume delay, two meanings: with SCAN_RESUME_CARRIER it
     * is the hang after the carrier drops, with SCAN_RESUME_TIMEOUT the time
     * the scan stays on a signal it found.  A zero is read as "the default",
     * because a zero hang would make the scan walk off in the middle of a
     * syllable.
     */
    uint16_t delay = (st->scan_hold != 0) ? (uint16_t)(st->scan_hold * 100U)
                                          : (uint16_t)2000;
    if (cfg->resume == SCAN_RESUME_TIMEOUT)
    {
        cfg->holdMs = delay;
        cfg->hangMs = 0;
    }
    else
    {
        cfg->hangMs = delay;
        cfg->holdMs = 0;
    }

    /* Zero means "every step": the operator asked for the tightest check */
    cfg->priorityIntervalMs = (uint16_t)(st->scan_prioInterval * 100U);

    cfg->vfoStep = settings_scanStepValid(st->scan_step)
                 ? scan_steps[st->scan_step] : 12500U;
    cfg->vfoLow  = st->scan_vfoLow;
    cfg->vfoHigh = st->scan_vfoHigh;

    if (!chstore_freqValid(cfg->vfoLow) || !chstore_freqValid(cfg->vfoHigh) ||
        (cfg->vfoLow >= cfg->vfoHigh))
    {
        cfg->vfoLow  = SCAN_VFO_LOW_DEFAULT;
        cfg->vfoHigh = SCAN_VFO_HIGH_DEFAULT;
    }

    /* A range narrower than the step cannot be walked: use the smallest one */
    if (cfg->vfoStep > (cfg->vfoHigh - cfg->vfoLow))
        cfg->vfoStep = scan_steps[0];
}

/* --------------------------------------------------------------------- API */

void scan_service_init(void)
{
    scan_init(&engine);
    scan_setList(&engine, &bankList);

    memset(&home, 0, sizeof(home));
    memset(events, 0, sizeof(events));
    eventHead      = 0;
    eventCount     = 0;
    restorePending = false;

    currentChannel   = SCAN_CHANNEL_NONE;
    currentFrequency = 0;
    currentMode      = OPMODE_FM;
}

bool scan_service_start(uint8_t mode)
{
    if (scan_isRunning(&engine))
        return false;

    /* Never take the receiver away from a call in progress */
    if (txActive())
        return false;

    if ((mode != SCAN_MODE_VFO) && (chstore_count() <= 0))
        return false;

    scanConfig_t cfg;
    configFromSettings(&cfg, mode);

    /* Applied before anything is saved: a refused configuration changes
     * nothing on the radio */
    if (!scan_configure(&engine, &cfg))
        return false;

    pthread_mutex_lock(&state_mutex);
    home.channel    = state.channel;
    home.index      = state.channel_index;
    home.tunerMode  = state.tuner_mode;
    home.shift      = (int32_t)state.channel.tx_frequency -
                      (int32_t)state.channel.rx_frequency;
    home.dmrType    = state.settings.dmr_dest_type;
    home.dmrId      = state.settings.dmr_dest_id;
    home.dmrTs      = state.settings.dmr_timeslot;
    home.onBank     = state.bank_enabled;
    /* The UI hook: the previous mode is restored when the scan stops */
    state.tuner_mode = (cfg.mode == SCAN_MODE_MEMORY) ? CHSCAN : SCAN;
    pthread_mutex_unlock(&state_mutex);
    home.valid = true;

    uint16_t startChannel = (cfg.mode == SCAN_MODE_MEMORY) ? home.index
                                                           : SCAN_CHANNEL_NONE;
    if ((cfg.mode == SCAN_MODE_MEMORY) && !home.onBank)
        startChannel = SCAN_CHANNEL_NONE;   /* parked on the VFO: from the top */

    uint32_t startFrequency = (cfg.mode == SCAN_MODE_VFO)
                            ? home.channel.rx_frequency : 0;

    scanAction_t action;
    uint32_t now = (uint32_t)getTick();
    if (!scan_start(&engine, startChannel, startFrequency, now, &action))
        return false;

    currentChannel   = scan_channel(&engine);
    currentFrequency = scan_frequency(&engine);

    pushEvent(&action);
    if (action.target == SCAN_TARGET_CHANNEL)
        tuneChannel(action.channel);
    else if (action.target == SCAN_TARGET_FREQUENCY)
        tuneFrequency(action.frequency);

    return true;
}

void scan_service_stop(void)
{
    scanAction_t action;
    uint32_t now = (uint32_t)getTick();

    if (!scan_isRunning(&engine))
    {
        /* Nothing to stop, but a stop owed a retune may still be pending */
        if (restorePending && !txActive())
            finishRestore();
        return;
    }

    scan_stop(&engine, now, &action);
    pushEvent(&action);

    /*
     * The retune is deferred while the radio is transmitting: rebuilding the
     * RTX configuration in the middle of a call would cut the call, which is
     * the one thing the scan must never do.  state_task() keeps calling
     * scan_service_poll() and the restore happens on the first tick after the
     * transmission ends.
     */
    if (txActive())
        restorePending = true;
    else
        finishRestore();
}

void scan_service_poll(void)
{
    if (!scan_isRunning(&engine))
    {
        if (restorePending && !txActive())
            finishRestore();
        return;
    }

    bool tx = txActive();
    bool signal = tx ? false : signalPresent();
    uint8_t before = scan_state(&engine);

    scanAction_t action;
    scan_step(&engine, signal, tx, (uint32_t)getTick(), &action);

    if (action.event == SCAN_EVENT_STOPPED)
    {
        /*
         * PTT cancelled the scan.  If it was stopped on a signal, that channel
         * is the one the operator wants to talk on; if it was walking, it goes
         * back to where the scan started.
         */
        if ((before == SCAN_STATE_SIGNAL) || (before == SCAN_STATE_HANG))
            adoptCurrent();

        pushEvent(&action);
        restorePending = true;
        return;
    }

    pushEvent(&action);

    if (action.target == SCAN_TARGET_CHANNEL)
        tuneChannel(action.channel);
    else if (action.target == SCAN_TARGET_FREQUENCY)
        tuneFrequency(action.frequency);
}

bool scan_service_status(struct scanStatus *out)
{
    if (out == NULL)
        return false;

    out->running   = scan_isRunning(&engine);
    out->state     = scan_state(&engine);
    out->mode      = engine.cfg.mode;
    out->channel   = currentChannel;
    out->frequency = currentFrequency;
    out->visited   = scan_visited(&engine);
    out->stops     = scan_stops(&engine);

    return true;
}

bool scan_service_popEvent(struct scanEventMsg *out)
{
    if ((out == NULL) || (eventCount == 0))
        return false;

    *out = events[eventHead];
    eventHead = (uint8_t)((eventHead + 1) % SCAN_EVENT_QUEUE);
    eventCount--;

    return true;
}

const char *scan_service_stateName(void)
{
    return scan_stateName(scan_state(&engine));
}
