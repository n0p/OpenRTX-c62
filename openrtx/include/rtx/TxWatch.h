/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TX_WATCH_H
#define OPENRTX_TX_WATCH_H

/**
 * @file TxWatch.h
 * @brief Absolute transmit clock: how long the PA has been on the air since it
 *        was last released, independent of modes, threads and the PTT.
 *
 * The KeyGate (rtx/KeyGate.h) stops a *change of mode* from keying the PA. It
 * cannot stop what happened next: a mode that keyed and then never unkeyed --
 * a stream write that never returned, a DSP call that parked the RTX thread --
 * left the radio transmitting until the battery died, hot enough that it would
 * not switch on again.
 *
 * The answer is a clock that knows one thing: the PA is on the air or it is
 * not. It counts the *air*, not the transmission: a message that ends and
 * another that starts 30 ms later are the same keying as far as the PA's
 * temperature is concerned, and a mode change is not a release either. Only one
 * event puts it back to zero: the PA being off the air for TXW_RELEASE_MS,
 * which is what a real release of the PTT -- or the end of one unattended grant
 * and a new one, with the radio idle in between -- looks like from outside.
 *
 * Two instances of this run, at two limits:
 *
 *  - the RTX one (rtx.cpp), TXW_LIMIT_MS, sampled by the main thread every
 *    5 ms from the driver's own state: it cuts the transmission through the
 *    mode's abort path, so the mode unwinds and the radio returns to RX;
 *  - the driver one (radio_C62.cpp), TXW_DRIVER_LIMIT_MS, fed by
 *    radio_enableTx()/radio_disableRtx() themselves: it refuses to key and
 *    forces the PA off on its own timer, so a supervisor that never runs is
 *    still covered by a second, independent barrier.
 *
 * No clock, no radio, no threads: the caller passes the time and the level in,
 * so the whole rule is walked on the host (tests/unit/tx_watch.cpp).
 */

#include <stdbool.h>
#include <stdint.h>

/** Air time, ms, after which the RTX supervisor ends the transmission. */
#define TXW_LIMIT_MS 90000
/** Margin, ms, the driver's own barrier allows over the supervisor's limit. */
#define TXW_MARGIN_MS 5000
/** The driver barrier's limit: the supervisor's plus the margin. */
#define TXW_DRIVER_LIMIT_MS (TXW_LIMIT_MS + TXW_MARGIN_MS)
/**
 * Time off the air, ms, that counts as a release and puts the clock to zero.
 *
 * One second: longer than any inter-message gap of an unattended chain (one
 * 30 ms slot, a few hundred ms at worst) and longer than a mode change, so
 * neither resets the count by accident; shorter than the pause of an operator
 * pressing the PTT again, so a deliberate re-press does.
 */
#define TXW_RELEASE_MS 1000
/** Air time, ms, of a transmission that must be refused: the bar is spent. */
#define TXW_SPENT_MS 1000

/** What the caller has to do after txw_tick(). */
enum txWatchAction
{
    TXW_ACTION_NONE = 0, ///< Within the limit
    TXW_ACTION_CUT       ///< The limit was reached: unkey now
};

/** State of the clock. One per barrier; owned by the layer that feeds it. */
struct txWatch
{
    uint32_t limitMs;   ///< Air time allowed before the cut
    uint32_t airMs;     ///< Air accumulated by complete keyings
    uint32_t keyedAt;   ///< When the current keying started
    uint32_t offAt;     ///< When the PA was last seen off
    uint32_t keys;      ///< Keyings seen (trace)
    uint32_t rekeys;    ///< Releases that put the clock back to zero (trace)
    uint32_t cuts;      ///< Times the limit was reached (trace)
    uint32_t lastAirMs; ///< Air time at the last cut
    bool     keyed;     ///< The PA is keyed now
    bool     blocked;   ///< The bar is spent: no keying until a release
    bool     seeded;    ///< offAt holds a valid time
};

/**
 * Reset the clock at boot.
 *
 * @param w: clock.
 * @param now: current time, ms.
 * @param limitMs: air time allowed, 0 for TXW_LIMIT_MS.
 */
static inline void txw_init(struct txWatch *w, uint32_t now, uint32_t limitMs)
{
    w->limitMs   = (limitMs != 0) ? limitMs : TXW_LIMIT_MS;
    w->airMs     = 0;
    w->keyedAt   = now;
    w->offAt     = now;
    w->keys      = 0;
    w->rekeys    = 0;
    w->cuts      = 0;
    w->lastAirMs = 0;
    w->keyed     = false;
    w->blocked   = false;
    w->seeded    = true;
}

/** A clock that was never initialised still works, with the default limit. */
static inline void txw_ready(struct txWatch *w, uint32_t now)
{
    if (!w->seeded)
        txw_init(w, now, 0);
}

/**
 * Air time of the transmission as it stands, ms, including the keying that is
 * open right now. This is the number the operator and the log want.
 *
 * @param w: clock.
 * @param now: current time, ms.
 * @return air time since the last release, ms.
 */
static inline uint32_t txw_airMs(const struct txWatch *w, uint32_t now)
{
    if (w->keyed)
        return w->airMs + (uint32_t)(now - w->keyedAt);

    return w->airMs;
}

/**
 * Put the clock back to zero: the radio has been idle long enough that what it
 * does next is a new transmission, not the continuation of this one.
 *
 * @param w: clock.
 */
static inline void txw_release(struct txWatch *w)
{
    if ((w->airMs > 0) || w->blocked)
        w->rekeys++;

    w->airMs   = 0;
    w->blocked = false;
}

/**
 * Report what the PA is doing. Call it every tick, from any thread.
 *
 * TXW_RELEASE_MS off the air is the only thing that puts the clock back to
 * zero, and it is checked on every sample while the PA is down, so it is
 * counted as soon as it completes -- not at the next keying. A rising edge
 * inside that window continues the same count (an unattended chain, a mode that
 * re-keys straight after its over), and a falling edge only accumulates.
 *
 * @param w: clock.
 * @param now: current time, ms.
 * @param keyed: true while the PA is on the air.
 */
static inline void txw_sample(struct txWatch *w, uint32_t now, bool keyed)
{
    txw_ready(w, now);

    if (keyed && (!w->keyed))
    {
        /* A keying that follows a completed release starts from zero */
        if ((uint32_t)(now - w->offAt) >= TXW_RELEASE_MS)
            txw_release(w);

        w->keyedAt = now;
        w->keyed   = true;
        w->keys++;
    }
    else if ((!keyed) && w->keyed)
    {
        w->airMs += (uint32_t)(now - w->keyedAt);
        w->keyed  = false;
        w->offAt  = now;
    }
    else if (!keyed)
    {
        /* Off the air long enough: the release completes here */
        if ((uint32_t)(now - w->offAt) >= TXW_RELEASE_MS)
            txw_release(w);
    }
}

/**
 * Is the bar spent? Nothing may key until the PA has been released.
 *
 * @param w: clock.
 * @return true while blocked.
 */
static inline bool txw_blocked(const struct txWatch *w)
{
    return w->blocked;
}

/**
 * May the PA be keyed now?
 *
 * False once the limit has been reached (the bar is spent) and -- through
 * txw_tick() -- until the radio has been released.
 *
 * @param w: clock.
 * @param now: current time, ms.
 * @return true if keying is still allowed.
 */
static inline bool txw_keyAllowed(const struct txWatch *w, uint32_t now)
{
    if (w->blocked)
        return false;

    return txw_airMs(w, now) < w->limitMs;
}

/**
 * Check the limit. Call it every tick from the thread that owns the barrier.
 *
 * It latches the cut: the clock stays blocked, and txw_keyAllowed() refuses
 * every keying, until the PA has been released for TXW_RELEASE_MS.
 *
 * @param w: clock.
 * @param now: current time, ms.
 * @return TXW_ACTION_CUT exactly once per overrun.
 */
static inline enum txWatchAction txw_tick(struct txWatch *w, uint32_t now)
{
    txw_ready(w, now);

    if (w->blocked)
        return TXW_ACTION_NONE;

    if (txw_airMs(w, now) < w->limitMs)
        return TXW_ACTION_NONE;

    w->blocked   = true;
    w->cuts++;
    w->lastAirMs = txw_airMs(w, now);
    return TXW_ACTION_CUT;
}

/**
 * Air time in whole seconds, for the diagnostics.
 *
 * @param w: clock.
 * @param now: current time, ms.
 * @return seconds of air since the last release, saturating at 65535.
 */
static inline uint16_t txw_airSeconds(const struct txWatch *w, uint32_t now)
{
    const uint32_t seconds = txw_airMs(w, now) / 1000u;

    return (seconds > 65535u) ? 65535u : (uint16_t)seconds;
}

#endif /* OPENRTX_TX_WATCH_H */
