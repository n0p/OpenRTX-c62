/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/scanner.h"

#include <string.h>

/*
 * Scan and vigilance, with no dependency at all: no clock, no radio, no flash.
 * The caller samples the squelch and passes the time in, and gets back an
 * action saying where to tune and what happened (core/scanner.h has the state
 * and event tables).
 *
 * Why a machine and not a loop with sleeps: a scan spends its life waiting on
 * three timers that overlap (settle, dwell, hang) while a fourth condition
 * (squelch, PTT) can preempt any of them, and the failure a user notices is
 * never a missed channel but a radio that stayed stuck on one.  Written as a
 * state machine driven by one 5 ms sample, "stuck" is a state that is not in
 * the table, so it cannot happen; and the whole thing runs on a host with a
 * synthetic squelch, which is how it is tested (tests/unit/scanner.cpp).
 */

#define ELAPSED(now, start) ((uint32_t)((now) - (start)))

static const char *const stateNames[SCAN_STATE_COUNT] = {
    "idle", "tune", "listen", "signal", "hang", "priority", "end"
};

static const char *const eventNames[SCAN_EVENT_COUNT] = {
    "none", "started", "tuned", "signal", "resume", "range", "priority",
    "stopped"
};

void scan_defaultConfig(scanConfig_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->mode              = SCAN_MODE_MEMORY;
    cfg->resume            = SCAN_RESUME_CARRIER;
    cfg->direction         = SCAN_UP;
    cfg->loop              = 1;
    cfg->priorityEnabled   = 0;
    cfg->priority          = SCAN_CHANNEL_NONE;

    /*
     * 60 ms is the time the RTX needs to come out of a mode change and hand
     * back a squelch that means something; 150 ms of dwell is the shortest
     * time in which an analog squelch reliably opens on a carrier, and 2 s of
     * hang is what every scanner ships with.  The priority channel is looked
     * at every 3 s: checking it every step would spend more time on the
     * priority channel than on the scan.
     */
    cfg->settleMs           = 60;
    cfg->dwellMs            = 150;
    cfg->hangMs             = 2000;
    cfg->holdMs             = 2000;
    cfg->debounceMs         = 20;
    cfg->priorityIntervalMs = 3000;

    cfg->vfoLow             = 144000000U;
    cfg->vfoHigh            = 146000000U;
    cfg->vfoStep            = 12500U;
}

/* ------------------------------------------------------------------ squelch */

static void squelchReset(scanSquelch_t *q, uint32_t now)
{
    q->raw    = 0;
    q->stable = 0;
    q->since  = now;
}

/**
 * Chatter filter: a change of the squelch is believed once it has been seen
 * unchanged for debounceMs.  A retune resets the tracker, so the squelch of the
 * previous channel cannot leak into the new one.
 */
static bool squelchUpdate(scanSquelch_t *q, bool raw, uint32_t now,
                          uint16_t debounceMs)
{
    uint8_t level = raw ? 1 : 0;

    if (q->raw != level) {
        q->raw   = level;
        q->since = now;
    }

    if (q->stable != level && ELAPSED(now, q->since) >= debounceMs)
        q->stable = level;

    return q->stable != 0;
}

/* --------------------------------------------------------------------- list */

static int listCount(const scanner_t *s)
{
    if ((s->list == NULL) || (s->list->count == NULL))
        return 0;

    return s->list->count(s->list->ctx);
}

static bool listInfo(const scanner_t *s, uint16_t idx, scanEntry_t *out)
{
    if ((s->list == NULL) || (s->list->info == NULL))
        return false;

    return s->list->info(s->list->ctx, idx, out) == 0;
}

/* ------------------------------------------------------------------ targets */

static void enterState(scanner_t *s, uint8_t state, uint32_t now)
{
    s->state     = state;
    s->stateTime = now;
}

/** Park the walk on a target and start the settle timer. */
static void beginTarget(scanner_t *s, uint8_t target, uint16_t channel,
                        uint32_t frequency, uint32_t now)
{
    if (target == SCAN_TARGET_CHANNEL)
        s->cursor = channel;
    else if (target == SCAN_TARGET_FREQUENCY)
        s->vfoFreq = frequency;

    enterState(s, SCAN_STATE_TUNE, now);
    squelchReset(&s->squelch, now);
}

/**
 * Next scannable channel after the cursor.  Channels the operator flagged as
 * SKIP, and indices the bank refuses, are passed over; `lap` is set when the
 * walk crossed the last channel, which is what the interface reports as the
 * end of the range on a memory scan.
 */
static bool memoryNext(scanner_t *s, scanAction_t *out, bool *lap)
{
    int n = listCount(s);
    if (n <= 0)
        return false;

    uint16_t last = (uint16_t)(n - 1);
    bool started  = (s->cursor <= last);
    uint16_t idx  = started ? s->cursor : last;
    bool crossed  = false;

    for (int k = 0; k < n; k++)
    {
        uint16_t prev = idx;
        idx = (uint16_t)((prev == last) ? 0 : (prev + 1));

        /* A lap is passing from the last channel to the first one, which is
         * not what happens when the walk starts before the first channel. */
        if (started && (idx == 0))
            crossed = true;

        scanEntry_t entry;
        if (!listInfo(s, idx, &entry))
            continue;
        if (entry.skip)
            continue;

        out->target    = SCAN_TARGET_CHANNEL;
        out->channel   = idx;
        out->frequency = entry.frequency;
        *lap           = crossed;
        s->cursor      = idx;
        s->visited++;
        return true;
    }

    return false;
}

/**
 * Next frequency of a VFO scan.  A VFO parked outside the range enters it from
 * the edge it is closest to; at a limit it either wraps (loop) or ends.
 */
static bool vfoNext(scanner_t *s, scanAction_t *out, bool *lap)
{
    uint32_t lo   = s->cfg.vfoLow;
    uint32_t hi   = s->cfg.vfoHigh;
    uint32_t step = (s->cfg.vfoStep != 0) ? s->cfg.vfoStep : 1;
    bool     down = (s->cfg.direction == SCAN_DOWN);
    uint32_t f    = s->vfoFreq;
    uint32_t next;
    bool     wrapped = false;

    if (hi < lo)
    {
        uint32_t swap = lo;
        lo = hi;
        hi = swap;
    }

    if ((f < lo) || (f > hi))
    {
        /* Outside the range: the first step is onto the edge itself */
        next = down ? hi : lo;
    }
    else if (down)
    {
        if ((f - lo) < step)
        {
            if (!s->cfg.loop)
                return false;
            next    = hi;
            wrapped = true;
        }
        else
            next = f - step;
    }
    else
    {
        if ((hi - f) < step)
        {
            if (!s->cfg.loop)
                return false;
            next    = lo;
            wrapped = true;
        }
        else
            next = f + step;
    }

    out->target    = SCAN_TARGET_FREQUENCY;
    out->channel   = SCAN_CHANNEL_NONE;
    out->frequency = next;
    *lap           = wrapped;
    s->visited++;

    return true;
}

/* ---------------------------------------------------------------- priority */

static bool priorityDue(const scanner_t *s, uint32_t now)
{
    if (!s->cfg.priorityEnabled)
        return false;
    if (s->cfg.priority == SCAN_CHANNEL_NONE)
        return false;
    if (s->onPriority)
        return false;

    /*
     * The step that starts the scan is not a step *between* two channels: the
     * operator asked for a scan and the scan goes to the first channel.  The
     * first priority check comes after a channel has been listened to, which is
     * what `visited` counts (it is incremented by the walk, so the start has
     * visited exactly one).
     */
    if (s->visited < 2)
        return false;

    if (s->cfg.priorityIntervalMs == 0)
        return true;

    return ELAPSED(now, s->lastPriority) >= s->cfg.priorityIntervalMs;
}

/* ---------------------------------------------------------------- stepping */

static void stepNext(scanner_t *s, uint32_t now, scanAction_t *a);

/** Leave a priority check that found nothing: back to the interrupted step. */
static void resumeFromPriority(scanner_t *s, uint32_t now, scanAction_t *a)
{
    scanAction_t next = s->pending;

    s->onPriority = 0;
    memset(&s->pending, 0, sizeof(s->pending));
    s->pending.channel = SCAN_CHANNEL_NONE;

    a->event     = next.event;
    a->target    = next.target;
    a->channel   = next.channel;
    a->frequency = next.frequency;

    enterState(s, SCAN_STATE_TUNE, now);
    squelchReset(&s->squelch, now);
}

/**
 * Leave a stop on a signal.  The event is always RESUME — that is what the
 * operator sees — and the target is wherever the walk goes next.
 */
static void resumeAfterSignal(scanner_t *s, uint32_t now, scanAction_t *a)
{
    if (s->onPriority)
        resumeFromPriority(s, now, a);
    else
        stepNext(s, now, a);

    if (a->event != SCAN_EVENT_RANGE_END)
        a->event = SCAN_EVENT_RESUME;
}

/**
 * Choose the next target and go there, or check the priority channel first.
 *
 * The priority check only ever happens here, at a step boundary: by
 * construction the receiver is not carrying anything at that moment, so a
 * call in progress is never interrupted by it (a call keeps the machine in
 * SIGNAL, where no step is taken at all).
 */
static void stepNext(scanner_t *s, uint32_t now, scanAction_t *a)
{
    scanAction_t next;
    bool lap = false;
    bool found;

    memset(&next, 0, sizeof(next));
    next.channel = SCAN_CHANNEL_NONE;

    if (s->cfg.mode == SCAN_MODE_VFO)
        found = vfoNext(s, &next, &lap);
    else
        found = memoryNext(s, &next, &lap);

    if (!found)
    {
        /* Nothing to walk: empty bank, every channel skipped, range finished */
        enterState(s, SCAN_STATE_END, now);
        a->event     = SCAN_EVENT_RANGE_END;
        a->target    = SCAN_TARGET_NONE;
        a->channel   = SCAN_CHANNEL_NONE;
        a->frequency = 0;
        return;
    }

    next.event = lap ? SCAN_EVENT_RANGE_END : SCAN_EVENT_TUNED;

    bool sameAsPriority = (next.target == SCAN_TARGET_CHANNEL) &&
                          (next.channel == s->cfg.priority);

    if (priorityDue(s, now) && !sameAsPriority)
    {
        /*
         * Remember where the walk was going and look at the priority channel.
         * The cursor is deliberately left where it is: after the check the walk
         * carries on to the channel it had chosen, not from the priority one.
         */
        s->pending      = next;
        s->onPriority   = 1;
        s->lastPriority = now;
        enterState(s, SCAN_STATE_PRIORITY, now);
        squelchReset(&s->squelch, now);

        a->event     = SCAN_EVENT_TUNED;
        a->target    = SCAN_TARGET_CHANNEL;
        a->channel   = s->cfg.priority;
        a->frequency = 0;
        return;
    }

    a->event     = next.event;
    a->target    = next.target;
    a->channel   = next.channel;
    a->frequency = next.frequency;

    beginTarget(s, next.target, next.channel, next.frequency, now);
}

/* --------------------------------------------------------------------- API */

void scan_init(scanner_t *s)
{
    memset(s, 0, sizeof(*s));
    scan_defaultConfig(&s->cfg);
    s->state           = SCAN_STATE_IDLE;
    s->cursor          = SCAN_CHANNEL_NONE;
    s->pending.channel = SCAN_CHANNEL_NONE;
}

void scan_setList(scanner_t *s, const scanList_t *list)
{
    s->list = list;
}

bool scan_configure(scanner_t *s, const scanConfig_t *cfg)
{
    if ((cfg == NULL) || s->running)
        return false;

    s->cfg = *cfg;

    /* A configuration that would stall the walk is repaired, not refused:
     * these values come from a flash record and a menu. */
    if (s->cfg.resume > SCAN_RESUME_HOLD)
        s->cfg.resume = SCAN_RESUME_CARRIER;
    if (s->cfg.mode > SCAN_MODE_VFO)
        s->cfg.mode = SCAN_MODE_MEMORY;
    if (s->cfg.direction > SCAN_DOWN)
        s->cfg.direction = SCAN_UP;

    return true;
}

bool scan_start(scanner_t *s, uint16_t channel, uint32_t frequency,
                uint32_t now, scanAction_t *action)
{
    if (s->running)
        return false;

    s->running         = 1;
    s->cursor          = channel;
    s->vfoFreq         = frequency;
    s->onPriority      = 0;
    s->visited         = 0;
    s->stops           = 0;
    s->lastPriority    = now;
    memset(&s->pending, 0, sizeof(s->pending));
    s->pending.channel = SCAN_CHANNEL_NONE;
    squelchReset(&s->squelch, now);

    scanAction_t first;
    memset(&first, 0, sizeof(first));
    first.channel = SCAN_CHANNEL_NONE;
    stepNext(s, now, &first);

    if (action != NULL)
    {
        /*
         * A start with nothing to walk is reported as the end of the range: the
         * caller knows a start happened (the machine is running) and what it
         * needs to know is why no channel was tuned.
         */
        action->event     = (first.event == SCAN_EVENT_RANGE_END)
                          ? (uint8_t)SCAN_EVENT_RANGE_END
                          : (uint8_t)SCAN_EVENT_STARTED;
        action->target    = first.target;
        action->channel   = first.channel;
        action->frequency = first.frequency;
    }

    return true;
}

void scan_stop(scanner_t *s, uint32_t now, scanAction_t *action)
{
    bool wasRunning = s->running != 0;

    if (action != NULL)
    {
        memset(action, 0, sizeof(*action));
        action->channel = SCAN_CHANNEL_NONE;
        if (wasRunning)
            action->event = SCAN_EVENT_STOPPED;
        action->target = wasRunning ? SCAN_TARGET_HOME : SCAN_TARGET_NONE;
    }

    s->running    = 0;
    s->onPriority = 0;
    memset(&s->pending, 0, sizeof(s->pending));
    s->pending.channel = SCAN_CHANNEL_NONE;
    enterState(s, SCAN_STATE_IDLE, now);
}

bool scan_resume(scanner_t *s, uint32_t now, scanAction_t *action)
{
    if (!s->running)
        return false;

    scanAction_t a;
    memset(&a, 0, sizeof(a));
    a.channel = SCAN_CHANNEL_NONE;

    switch (s->state)
    {
        case SCAN_STATE_SIGNAL:
        case SCAN_STATE_HANG:
            /* A stop the resume rule would also have left: same way out */
            resumeAfterSignal(s, now, &a);
            break;

        case SCAN_STATE_END:
            /* Carry on from where the range ended (a lap, or nothing at all
             * for a VFO scan without loop: the machine ends again and says so) */
            stepNext(s, now, &a);
            break;

        default:
            return false;
    }

    if (action != NULL)
        *action = a;

    return true;
}

void scan_step(scanner_t *s, bool signal, bool transmitting, uint32_t now,
               scanAction_t *action)
{
    scanAction_t a;

    memset(&a, 0, sizeof(a));
    a.channel = SCAN_CHANNEL_NONE;

    if (!s->running)
    {
        if (action != NULL)
            *action = a;
        return;
    }

    /*
     * A transmission outranks the scan: whatever the machine was doing, the
     * radio is keyed, so the scan is over and the receiver goes back to the
     * channel it came from.
     */
    if (transmitting)
    {
        scan_stop(s, now, &a);
        if (action != NULL)
            *action = a;
        return;
    }

    bool active = squelchUpdate(&s->squelch, signal, now, s->cfg.debounceMs);

    switch (s->state)
    {
        case SCAN_STATE_TUNE:
            if (ELAPSED(now, s->stateTime) >= s->cfg.settleMs)
                enterState(s, SCAN_STATE_LISTEN, now);
            break;

        case SCAN_STATE_LISTEN:
            if (active)
            {
                s->stops++;
                s->signalTime = now;
                enterState(s, SCAN_STATE_SIGNAL, now);
                a.event = SCAN_EVENT_SIGNAL;
            }
            else if (ELAPSED(now, s->stateTime) >= s->cfg.dwellMs)
                stepNext(s, now, &a);
            break;

        case SCAN_STATE_SIGNAL:
            if (s->cfg.resume == SCAN_RESUME_HOLD)
                break;

            if (s->cfg.resume == SCAN_RESUME_TIMEOUT)
            {
                if (ELAPSED(now, s->signalTime) >= s->cfg.holdMs)
                    resumeAfterSignal(s, now, &a);
                break;
            }

            /* SCAN_RESUME_CARRIER: a closed squelch starts the hang timer */
            if (!active)
                enterState(s, SCAN_STATE_HANG, now);
            break;

        case SCAN_STATE_HANG:
            if (active)
            {
                /* It came back: still the same stop */
                s->signalTime = now;
                enterState(s, SCAN_STATE_SIGNAL, now);
            }
            else if (ELAPSED(now, s->stateTime) >= s->cfg.hangMs)
                resumeAfterSignal(s, now, &a);
            break;

        case SCAN_STATE_PRIORITY:
            if (active && (ELAPSED(now, s->stateTime) >= s->cfg.settleMs))
            {
                s->stops++;
                s->signalTime = now;
                enterState(s, SCAN_STATE_SIGNAL, now);
                a.event = SCAN_EVENT_PRIORITY;
            }
            else if (ELAPSED(now, s->stateTime) >=
                     (uint32_t)s->cfg.settleMs + (uint32_t)s->cfg.dwellMs)
                resumeFromPriority(s, now, &a);
            break;

        case SCAN_STATE_END:
        case SCAN_STATE_IDLE:
        default:
            break;
    }

    if (action != NULL)
        *action = a;
}

bool scan_isRunning(const scanner_t *s)
{
    return s->running != 0;
}

uint8_t scan_state(const scanner_t *s)
{
    return s->state;
}

uint16_t scan_channel(const scanner_t *s)
{
    if (s->cfg.mode == SCAN_MODE_VFO)
        return SCAN_CHANNEL_NONE;

    return s->cursor;
}

uint32_t scan_frequency(const scanner_t *s)
{
    if (s->cfg.mode == SCAN_MODE_MEMORY)
        return 0;

    return s->vfoFreq;
}

uint32_t scan_visited(const scanner_t *s)
{
    return s->visited;
}

uint32_t scan_stops(const scanner_t *s)
{
    return s->stops;
}

const char *scan_stateName(uint8_t state)
{
    return (state < SCAN_STATE_COUNT) ? stateNames[state] : "?";
}

const char *scan_eventName(uint8_t event)
{
    return (event < SCAN_EVENT_COUNT) ? eventNames[event] : "?";
}
