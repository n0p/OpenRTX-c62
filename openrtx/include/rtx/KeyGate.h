/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_KEY_GATE_H
#define OPENRTX_KEY_GATE_H

/**
 * @file KeyGate.h
 * @brief When the PTT may key the transmitter: a stable mode and a fresh press.
 *
 * A change of mode used to be able to key the PA. The modes decide to transmit
 * from the *level* of the PTT line, sampled every 30 ms, so a PTT that read
 * pressed for any reason -- held across the transition, a stale value, a pin
 * sampled while the chip was being reconfigured -- keyed the mode that had just
 * been constructed, before it had a receiver or a transmit chain of its own.
 * That is how a DMR mode went on the air on a mode change and left the radio
 * stuck: the thread that should have unkeyed was the one that had just keyed.
 *
 * The rule this module implements is the one a radio needs:
 *
 *  - a mode change vetoes keying from its start until KEYGATE_GUARD_MS after
 *    its end, whatever the PTT line says;
 *  - the PTT has to have been *released* once since the change: the level that
 *    was present before it is not an operator asking to transmit;
 *  - the press that keys has to persist KEYGATE_HOLD_MS, so one bad sample
 *    does not key anything;
 *  - an unattended transmission (a queued short message, the packet modem)
 *    waits KEYGATE_SETTLE_MS after the change and never runs with the PTT down
 *    during it.
 *
 * No clock, no radio, no threads: the caller passes the time and the level in,
 * which is what lets the whole rule be walked on the host
 * (tests/unit/key_gate.cpp) instead of reasoned about.
 */

#include <stdbool.h>
#include <stdint.h>

/** Keying is vetoed for this long after a mode change ends, ms. */
#define KEYGATE_GUARD_MS 150
/** A press has to last this long before it keys, ms. One 30 ms tick is enough. */
#define KEYGATE_HOLD_MS 25
/** An unattended transmission waits this long after a mode change, ms. */
#define KEYGATE_SETTLE_MS 250

/** State of the gate. One per radio, owned by the RTX layer. */
struct keyGate
{
    bool     transition; ///< A mode change is in progress: nothing may key
    bool     released;   ///< The PTT has been seen up since the change
    bool     ptt;        ///< Last PTT level sampled
    uint32_t readyAt;    ///< No keying before this time (end of change + guard)
    uint32_t settleAt;   ///< No unattended transmission before this time
    uint32_t pressAt;    ///< When the current press started
    uint32_t grantedUntil; ///< An unattended transmission was asked for until
    uint32_t edges;      ///< Presses counted since the last reset (trace)
    uint32_t vetoes;     ///< Keying attempts refused (trace)
};

/** Reset the gate at boot: the first mode entry is a change like any other. */
static inline void keyGate_init(struct keyGate *g, uint32_t now)
{
    g->transition = true;
    g->released   = false;
    g->ptt        = false;
    g->readyAt    = now;
    g->settleAt   = now;
    g->pressAt    = 0;
    g->grantedUntil = 0;
    g->edges      = 0;
    g->vetoes     = 0;
}

/**
 * A mode change starts: veto keying and forget that the PTT was ever pressed.
 *
 * @param g: gate.
 * @param now: current time, ms.
 */
static inline void keyGate_beginTransition(struct keyGate *g, uint32_t now)
{
    g->transition = true;
    g->released   = false;
    g->ptt        = false;
    g->pressAt    = 0;
    g->readyAt    = now;
    g->settleAt   = now;
}

/**
 * Forget that the PTT was pressed, without starting a mode change.
 *
 * For the one case where the radio has just cut a transmission that is still
 * being asked for -- the transmit watchdog --: the press is spent, so keying
 * has to wait until the operator lets go and presses again.  Without it the
 * gate keeps `released` and `ptt` from before the cut and the next pass grants
 * the same stuck press, which is a transmitter that runs 90 s out of every 91
 * for as long as the line stays low.
 *
 * @param g: gate.
 */
static inline void keyGate_forgetPress(struct keyGate *g)
{
    g->released = false;
    g->ptt      = false;
    g->pressAt  = 0;
}

/**
 * The mode change ended: keep the veto for the guard time and settle the
 * unattended path.
 *
 * @param g: gate.
 * @param now: current time, ms.
 */
static inline void keyGate_endTransition(struct keyGate *g, uint32_t now)
{
    g->transition = false;
    g->readyAt    = now + KEYGATE_GUARD_MS;
    g->settleAt   = now + KEYGATE_GUARD_MS + KEYGATE_SETTLE_MS;
}

/**
 * Sample the PTT line. Call it before asking; it is idempotent within a tick.
 *
 * A press is timed from its rising edge, and any sample with the PTT up marks
 * the operator as having released it: from then on the next press may key.
 *
 * @param g: gate.
 * @param now: current time, ms.
 * @param ptt: PTT level, true while pressed.
 */
static inline void keyGate_sample(struct keyGate *g, uint32_t now, bool ptt)
{
    if (ptt && !g->ptt)
    {
        g->pressAt = now;
        g->edges++;
    }

    if (!ptt)
        g->released = true;

    g->ptt = ptt;
}

/**
 * May the PTT key the transmitter now?
 *
 * @param g: gate.
 * @param now: current time, ms.
 * @return true only with the mode stable, a release seen since the change and a
 *         press that has lasted KEYGATE_HOLD_MS.
 */
static inline bool keyGate_pttKey(struct keyGate *g, uint32_t now)
{
    if (g->transition || (!g->released) || (!g->ptt))
        return false;

    if (now < g->readyAt)
        return false;

    return (uint32_t)(now - g->pressAt) >= KEYGATE_HOLD_MS;
}

/** How long an unattended request is good for, ms: the caller keys at once. */
#define KEYGATE_GRANT_MS 1000

/**
 * Note that a caller that is allowed to transmit unattended (a queued short
 * message, the packet modem) is about to key.
 *
 * The grant is what keeps the driver's choke point honest: the PA may be keyed
 * with the PTT up only inside one of these, so a mode that keys on a stale
 * level -- or one that never asked -- is refused instead of being mistaken for
 * an unattended transmission.
 *
 * @param g: gate.
 * @param now: current time, ms.
 */
static inline void keyGate_grantUnattended(struct keyGate *g, uint32_t now)
{
    g->grantedUntil = now + KEYGATE_GRANT_MS;
}

/**
 * May an unattended transmission start now? (a queued short message, the packet
 * modem: no operator, no PTT.)
 *
 * @param g: gate.
 * @param now: current time, ms.
 * @return true once the mode change has ended and settled, with the PTT up.
 */
static inline bool keyGate_unattended(struct keyGate *g, uint32_t now)
{
    if (g->transition || g->ptt)
        return false;

    return now >= g->settleAt;
}

/**
 * Is an unattended transmission in progress? The PA choke point asks this: a
 * keying with the PTT up is permitted only inside a live grant.
 *
 * @param g: gate.
 * @param now: current time, ms.
 * @return true if the gate allowed an unattended transmission recently.
 */
static inline bool keyGate_unattendedGranted(const struct keyGate *g,
                                             uint32_t now)
{
    return (int32_t)(g->grantedUntil - now) > 0;
}

/**
 * Note a refused keying attempt, for the log.
 *
 * @param g: gate.
 */
static inline void keyGate_noteVeto(struct keyGate *g)
{
    g->vetoes++;
}

#endif /* OPENRTX_KEY_GATE_H */
