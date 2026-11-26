/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The accessory pad's PTT rule and the table of the three port modes.
 * Portable C with no radio in it: the transport classifies what the receiver
 * framed, discards what arrives while the switch is held, and both ask here.
 */

#include "core/port_ptt.h"
#include "core/settings.h"

#include <stddef.h>

void port_ptt_reset(portPttState_t *s)
{
    if (s == NULL)
        return;

    s->lowSince = 0;
    s->pressSince = 0;
    s->lastPressMs = 0;
    s->lost = 0;
    s->presses = 0;
    s->glitches = 0;
    s->low = false;
    s->pressed = false;
    /*
     * Not armed: the first thing the line does is not evidence of anything.
     * A pad that is already grounded when the detector starts -- a cable with
     * the switch held, a short, a pad whose pull-up has not been applied yet --
     * has to be released once before it can press anything.
     */
    s->armed = false;
    s->unknown = false;
}

bool port_ptt_update(portPttState_t *s, bool heldLow, uint32_t nowMs)
{
    if (s == NULL)
        return false;

    if (!heldLow) {
        /*
         * The line came back up long enough for a character to have a stop bit,
         * or nothing was sent at all.  That is the release, and it is the only
         * thing that arms the detector: from here on a held line is a press
         * somebody made, not a level that was already there at start-up.
         */
        s->armed = true;
        s->unknown = false;

        if (!s->low)
            return false;

        s->low = false;

        if (s->pressed) {
            /*
             * Release: immediate, and the duration is measured from the edge
             * that started the press, not from the instant it was declared.
             */
            s->pressed = false;
            s->lastPressMs = (uint32_t)(nowMs - s->pressSince);
            return true;
        }

        /* A low that ended before the threshold: a break, a bounce, garbage. */
        s->glitches++;
        return false;
    }

    if (!s->armed) {
        /*
         * Held from the first sample and never released: the level is unknown,
         * not a press.  Nothing is timed, so the moment the line is released
         * and held again the 30 ms are counted from scratch.
         */
        s->unknown = true;
        return false;
    }

    if (!s->low) {
        s->low = true;
        s->lowSince = nowMs;
    }

    /*
     * Strictly longer than the hold: a break and a bouncing contact are
     * shorter, and a press declared exactly at the threshold would make the
     * rule impossible to state.
     */
    if (!s->pressed && ((uint32_t)(nowMs - s->lowSince) > PORT_PTT_HOLD_MS)) {
        s->pressed = true;
        s->pressSince = s->lowSince;
        s->presses++;
        return true;
    }

    return false;
}

void port_ptt_seen_high(portPttState_t *s)
{
    if (s == NULL)
        return;

    /*
     * A stop bit that was sampled high is a release witness -- the line cannot
     * be clamped and frame a clean character -- so it arms the detector.  It
     * does not end a press: one pass is little enough that a window over a
     * clamped line can fake it (see core/port_ptt.h).
     */
    s->armed = true;
    s->unknown = false;

    if (s->pressed)
        return;

    s->low = false;
    s->lowSince = 0;
}

void port_ptt_unobserved(portPttState_t *s)
{
    if (s == NULL)
        return;

    /*
     * Only the timing of a run that has not been declared yet: a press already
     * on the air ends when the line is seen up, not when a window is missed.
     */
    if (s->pressed)
        return;

    s->low = false;
    s->lowSince = 0;
}

void port_ptt_sampler_reset(portPttSampler_t *s, uint32_t nowMs)
{
    if (s == NULL)
        return;

    s->nextMs = nowMs;
    s->samples = 0;
    s->skipped = 0;
    s->unstable = 0;
    s->stream = 0;
    s->started = true;
}

portPttStep_t port_ptt_sample_step(portPttSampler_t *s, bool cleanChar,
                                   uint32_t nowMs)
{
    if (s == NULL)
        return PORT_PTT_STEP_IDLE;

    if (!s->started)
        port_ptt_sampler_reset(s, nowMs);

    /*
     * The stream counter, and the reason the gate is not simply "no bytes":
     * a clean character carries a stop bit, so the line was high there.  A
     * clamped line cannot produce one -- everything it yields has FE or BI --
     * which is what makes "the receiver is framing clean characters" the
     * right test for "the pad is carrying a live stream".
     */
    if (cleanChar) {
        if (s->stream < 255)
            s->stream++;
    } else {
        s->stream = 0;
    }

    /* Signed difference: the clock wraps, the schedule does not care. */
    if ((int32_t)(nowMs - s->nextMs) < 0)
        return PORT_PTT_STEP_IDLE;

    /*
     * One window per period, scheduled from this pass rather than from the
     * deadline it missed: a stalled loop must not come back with a burst.
     */
    s->nextMs = nowMs + PORT_PTT_SAMPLE_PERIOD_MS;

    if (s->stream >= PORT_PTT_STREAM_PASSES) {
        s->skipped++;
        return PORT_PTT_STEP_RELEASE;
    }

    if (s->stream > 0) {
        s->skipped++;
        return PORT_PTT_STEP_WAIT;
    }

    s->samples++;
    return PORT_PTT_STEP_TAKE;
}

portPttPlan_t port_ptt_plan(uint8_t mode)
{
    portPttPlan_t plan = { .padIsUart = false, .useProtocol = false,
                           .useDetector = false, .useButton = false };

    switch (mode) {
        case PORT_MODE_ALWAYS:
            /* The pad is the serial port and nothing else: today's behaviour,
             * the speaker-mic PTT does not work. */
            plan.padIsUart = true;
            plan.useProtocol = true;
            break;

        case PORT_MODE_NEVER:
            /* The pad is a plain GPIO: the switch is 100 % reliable and the
             * serial protocol is not available at all. */
            plan.useButton = true;
            break;

        case PORT_MODE_AUTO:
        default:
            /*
             * Both, with the sustained-low rule deciding.  padIsUart stays true
             * because that is where the pad lives: the windows that read it as
             * a GPIO are a few microseconds long and belong to the platform
             * (c62_portPadSample()), not to the description of the mode.
             */
            plan.padIsUart = true;
            plan.useProtocol = true;
            plan.useDetector = true;
            break;
    }

    return plan;
}

bool port_ptt_keyed(uint8_t mode, bool detected, bool raw)
{
    switch (mode) {
        case PORT_MODE_ALWAYS:
            /* A UART RX line is not a button: whatever the level does, the
             * switch is not read in this mode. */
            return false;
        case PORT_MODE_NEVER:
            return raw;
        case PORT_MODE_AUTO:
        default:
            return detected;
    }
}
