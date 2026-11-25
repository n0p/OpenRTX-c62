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
            /* Both, with the sustained-low rule deciding. */
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
