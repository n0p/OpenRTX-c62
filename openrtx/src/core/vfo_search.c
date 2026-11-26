/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/vfo_search.h"

#include <string.h>

static void actionReset(vfoSearchAction_t *a)
{
    if (a == NULL)
        return;

    memset(a, 0, sizeof(*a));
    a->event  = VFOS_EV_NONE;
    a->target = VFOS_TARGET_NONE;
    a->result = VFOS_RESULT_NONE;
}

bool vfoSearch_mayStart(bool vfoScreen, bool transmitting, bool scanning)
{
    return vfoScreen && !transmitting && !scanning;
}

void vfoSearch_defaultConfig(vfoSearchConfig_t *cfg)
{
    if (cfg == NULL)
        return;

    memset(cfg, 0, sizeof(*cfg));
    cfg->direction   = VFOS_UP;
    cfg->digital     = 0;

    cfg->step        = 12500U;
    cfg->low         = 144000000U;
    cfg->high        = 146000000U;

    cfg->settleMs    = 40;
    cfg->dwellMs     = 120;
    cfg->debounceMs  = 20;

    cfg->syncFrames  = 2;
    cfg->syncHoldMs  = 100;
    cfg->syncMs      = 400;

    cfg->offsetMin   = 200;
    cfg->offsetMax   = 3000;
    cfg->reportedMax = 25000U;
}

void vfoSearch_init(vfoSearch_t *s)
{
    if (s == NULL)
        return;

    memset(s, 0, sizeof(*s));
    vfoSearch_defaultConfig(&s->cfg);
    s->state  = VFOS_STATE_IDLE;
    s->result = VFOS_RESULT_NONE;
}

bool vfoSearch_configure(vfoSearch_t *s, const vfoSearchConfig_t *cfg)
{
    if ((s == NULL) || (cfg == NULL))
        return false;

    /* A running sweep has a meaning tied to its band and its step */
    if (s->running)
        return false;

    if ((cfg->step == 0) || (cfg->low > cfg->high))
        return false;

    if ((cfg->syncFrames == 0) || (cfg->dwellMs == 0))
        return false;

    s->cfg = *cfg;
    return true;
}

bool vfoSearch_start(vfoSearch_t *s, uint32_t frequency, uint32_t now,
                     const vfoSearchSample_t *sample,
                     vfoSearchAction_t *action)
{
    actionReset(action);

    if ((s == NULL) || s->running)
        return false;

    if ((s->cfg.step == 0) || (s->cfg.low > s->cfg.high))
        return false;

    if ((frequency < s->cfg.low) || (frequency > s->cfg.high))
        return false;

    s->running        = 1;
    s->state          = VFOS_STATE_LISTEN;
    s->result         = VFOS_RESULT_NONE;
    s->corrected      = 0;
    s->offset         = 0;
    s->start          = frequency;
    s->frequency      = frequency;
    s->stateTime      = now;
    s->syncSince      = 0;
    s->signalSince    = 0;
    s->wrapped        = 0;
    s->visited        = 1;

    /* Only the frames that arrive from now on are evidence about this
     * frequency: whatever the mode decoded before was another signal */
    s->framesBase     = (sample != NULL) ? sample->frames : 0;
    s->haveFramesBase = (sample != NULL) ? 1 : 0;

    if (action != NULL) {
        action->event     = VFOS_EV_STARTED;
        action->target    = VFOS_TARGET_NONE;
        action->frequency = frequency;
        action->result    = VFOS_RESULT_NONE;
    }

    return true;
}

bool vfoSearch_stop(vfoSearch_t *s, uint32_t now, vfoSearchAction_t *action)
{
    actionReset(action);

    if ((s == NULL) || !s->running)
        return false;

    /*
     * Cancelled: the receiver stays where the sweep last tuned it.  That is
     * what the operator asked for, and it is why the action carries no target.
     */
    s->running   = 0;
    s->state     = VFOS_STATE_IDLE;
    s->result    = VFOS_RESULT_CANCELLED;
    s->stateTime = now;

    if (action != NULL) {
        action->event     = VFOS_EV_CANCELLED;
        action->target    = VFOS_TARGET_NONE;
        action->frequency = s->frequency;
        action->result    = VFOS_RESULT_CANCELLED;
    }

    return true;
}

/**
 * Take the frequency correction the receiver reports, if any: the absolute
 * frequency the network publishes comes first, the measured carrier offset
 * second.  A correction outside the band is refused.
 */
static void applyCorrection(vfoSearch_t *s, const vfoSearchSample_t *sm,
                            vfoSearchAction_t *a)
{
    const vfoSearchConfig_t *c = &s->cfg;
    uint32_t corrected = s->frequency;
    int32_t  offset    = 0;
    bool     fromNet   = false;

    if (sm->reported && (sm->reportedHz != 0)) {
        int64_t d = (int64_t)sm->reportedHz - (int64_t)s->frequency;
        if ((d != 0) && (d <= (int64_t)c->reportedMax) &&
            (d >= -(int64_t)c->reportedMax)) {
            corrected = sm->reportedHz;
            offset    = (int32_t)d;
            fromNet   = true;
        }
    }

    if (!fromNet && sm->offsetValid) {
        int32_t off = sm->offsetHz;
        if ((off >= c->offsetMin) || (off <= -c->offsetMin)) {
            if ((off <= c->offsetMax) && (off >= -c->offsetMax)) {
                int64_t target = (int64_t)s->frequency + (int64_t)off;
                if ((target >= (int64_t)c->low) && (target <= (int64_t)c->high)) {
                    corrected = (uint32_t)target;
                    offset    = off;
                }
            }
        }
    }

    if (a != NULL) {
        a->corrected = (corrected != s->frequency);
        a->offsetHz  = offset;
        if (corrected != s->frequency) {
            a->target    = VFOS_TARGET_FREQUENCY;
            a->frequency = corrected;
        }
    }

    s->frequency = corrected;
    s->corrected = (offset != 0) ? 1 : 0;
    s->offset    = offset;
}

/**
 * Enter FOUND: the search is over and the receiver stays where it is (on the
 * corrected frequency, when there is one).
 */
static void stopOnCarrier(vfoSearch_t *s, const vfoSearchSample_t *sm,
                          uint32_t now, uint8_t result, vfoSearchAction_t *a)
{
    s->state     = VFOS_STATE_FOUND;
    s->running   = 0;
    s->result    = result;
    s->stateTime = now;

    if (a != NULL) {
        a->event     = VFOS_EV_FOUND;
        a->target    = VFOS_TARGET_NONE;
        a->frequency = s->frequency;
        a->result    = result;
    }

    applyCorrection(s, sm, a);
}

/** End of the band: one full lap with nothing on it. */
static void endOfBand(vfoSearch_t *s, uint32_t now, vfoSearchAction_t *a)
{
    s->running   = 0;
    s->state     = VFOS_STATE_END;
    s->result    = VFOS_RESULT_BAND_END;
    s->stateTime = now;
    s->frequency = s->start;

    if (a != NULL) {
        a->event     = VFOS_EV_BAND_END;
        a->target    = VFOS_TARGET_FREQUENCY; /* back where the search began */
        a->frequency = s->start;
        a->result    = VFOS_RESULT_BAND_END;
    }
}

/**
 * Step to the next frequency, wrapping around the band.  A lap is complete
 * when the sweep passes the frequency it started on after having wrapped.
 */
static void advance(vfoSearch_t *s, uint32_t now, vfoSearchAction_t *a)
{
    const vfoSearchConfig_t *c = &s->cfg;
    bool     up   = (c->direction == VFOS_UP);
    uint32_t next = s->frequency;
    bool     lap  = false;

    if (up) {
        /* The second half of the test catches a 32-bit wrap: never happens
         * with real frequencies, always with a broken step */
        if ((s->frequency + c->step > c->high) ||
            (s->frequency + c->step < s->frequency)) {
            s->wrapped = 1;
            next = c->low;
            lap  = (next >= s->start);
        }
        else
            next = s->frequency + c->step;
    }
    else {
        if ((s->frequency < c->low) || ((s->frequency - c->low) < c->step)) {
            s->wrapped = 1;
            next = c->high;
            lap  = (next <= s->start);
        }
        else
            next = s->frequency - c->step;
    }

    if (s->wrapped && !lap) {
        if (up)
            lap = (next >= s->start);
        else
            lap = (next <= s->start);
    }

    if (lap) {
        endOfBand(s, now, a);
        return;
    }

    s->frequency = next;
    s->state     = VFOS_STATE_TUNE;
    s->stateTime = now;
    s->syncSince    = 0;
    s->signalSince  = 0;
    s->haveFramesBase = 0;
    s->visited++;

    if (a != NULL) {
        a->event     = VFOS_EV_TUNED;
        a->target    = VFOS_TARGET_FREQUENCY;
        a->frequency = next;
        a->result    = VFOS_RESULT_NONE;
    }
}

/**
 * How long a frequency is listened to before stepping.
 *
 * A digital mode that needs `syncMs` of uninterrupted sync cannot be believed
 * in less than that, so the dwell never cuts the proof short: the effective
 * dwell of a digital sweep is at least `syncMs + syncHoldMs`.  Without this a
 * configuration with a short dwell would sweep a band without ever being able
 * to stop in it, which is a silent way to make a search useless.
 */
static uint32_t listenTime(const vfoSearchConfig_t *c)
{
    uint32_t t = c->dwellMs;

    if (c->digital) {
        uint32_t need = (uint32_t)c->syncMs + (uint32_t)c->syncHoldMs;
        if (t < need)
            t = need;
    }

    return t;
}

void vfoSearch_step(vfoSearch_t *s, const vfoSearchSample_t *sample,
                    uint32_t now, vfoSearchAction_t *action)
{
    actionReset(action);

    if ((s == NULL) || (sample == NULL) || !s->running)
        return;

    const vfoSearchConfig_t *c = &s->cfg;

    if (s->state == VFOS_STATE_TUNE) {
        if ((now - s->stateTime) < c->settleMs)
            return;

        /* The receiver has settled: the frame counter starts here */
        s->state          = VFOS_STATE_LISTEN;
        s->stateTime      = now;
        s->syncSince      = 0;
        s->signalSince    = 0;
        s->framesBase     = sample->frames;
        s->haveFramesBase = sample->haveFrames ? 1 : 0;
        return;
    }

    if (s->state != VFOS_STATE_LISTEN)
        return;

    if (c->digital) {
        uint32_t frames = 0;

        if (sample->haveFrames) {
            if (!s->haveFramesBase || (sample->frames < s->framesBase)) {
                /* The mode reinitialised behind our back: rebase */
                s->framesBase     = sample->frames;
                s->haveFramesBase = 1;
            }
            frames = sample->frames - s->framesBase;
        }

        if (sample->synced) {
            if (s->syncSince == 0)
                s->syncSince = now;

            uint32_t held = now - s->syncSince;

            /*
             * Two ways to believe a digital carrier, and both need something
             * behind the sync word: either enough frames whose check passed,
             * or, in a mode that publishes no counter, uninterrupted sync for
             * longer than any mode keeps its sync flag set without decoding.
             */
            if (sample->haveFrames) {
                if ((frames >= c->syncFrames) && (held >= c->syncHoldMs)) {
                    stopOnCarrier(s, sample, now, VFOS_RESULT_SYNC, action);
                    return;
                }
            }
            else if (held >= c->syncMs) {
                stopOnCarrier(s, sample, now, VFOS_RESULT_SYNC, action);
                return;
            }
        }
        else
            s->syncSince = 0;
    }
    else {
        if (sample->squelch) {
            if (s->signalSince == 0)
                s->signalSince = now;

            if ((now - s->signalSince) >= c->debounceMs) {
                stopOnCarrier(s, sample, now, VFOS_RESULT_CARRIER, action);
                return;
            }
        }
        else
            s->signalSince = 0;
    }

    if ((now - s->stateTime) >= listenTime(c))
        advance(s, now, action);
}

bool vfoSearch_isRunning(const vfoSearch_t *s)
{
    return (s != NULL) && (s->running != 0);
}

uint8_t vfoSearch_state(const vfoSearch_t *s)
{
    return (s != NULL) ? s->state : (uint8_t)VFOS_STATE_IDLE;
}

uint8_t vfoSearch_result(const vfoSearch_t *s)
{
    return (s != NULL) ? s->result : (uint8_t)VFOS_RESULT_NONE;
}

uint32_t vfoSearch_frequency(const vfoSearch_t *s)
{
    return (s != NULL) ? s->frequency : 0U;
}

int32_t vfoSearch_offset(const vfoSearch_t *s)
{
    return (s != NULL) ? (int32_t)s->offset : 0;
}

uint32_t vfoSearch_visited(const vfoSearch_t *s)
{
    return (s != NULL) ? s->visited : 0U;
}

const char *vfoSearch_stateName(uint8_t state)
{
    static const char *const names[VFOS_STATE_COUNT] = {
        "idle", "listen", "tune", "found", "end"
    };

    return (state < VFOS_STATE_COUNT) ? names[state] : "?";
}

const char *vfoSearch_eventName(uint8_t event)
{
    static const char *const names[VFOS_EV_COUNT] = {
        "none", "started", "tuned", "found", "cancelled", "band-end"
    };

    return (event < VFOS_EV_COUNT) ? names[event] : "?";
}

const char *vfoSearch_resultName(uint8_t result)
{
    static const char *const names[VFOS_RESULT_COUNT] = {
        "none", "carrier", "sync", "cancelled", "band-end"
    };

    return (result < VFOS_RESULT_COUNT) ? names[result] : "?";
}
