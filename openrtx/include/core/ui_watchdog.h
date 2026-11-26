/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_WATCHDOG_H
#define UI_WATCHDOG_H

/**
 * @file ui_watchdog.h
 * @brief The application watchdog is fed only while the UI thread advances.
 *
 * The receiver, the audio worker and the protocol thread can all be working
 * while the UI thread is stuck: that is what the aeronautical-AM freeze looked
 * like from outside -- the receiver worked, the cable answered, and keys and
 * screen were dead.  The main thread is the one that feeds the hardware
 * watchdog, so a UI thread that never came back left a radio that looked
 * functional to everything except the operator, until the battery was pulled.
 *
 * The rule is therefore: the main loop feeds the watchdog only while the UI
 * thread's beat counter is moving.  Three seconds without a new beat means the
 * UI is not slow, it is parked, and the reset is the safe state (the PA enable
 * lines are off until a transmission keys them, and the radio comes back up in
 * receive).
 *
 * It lives here, and not inline in the main loop, because the "supervisor path
 * that was never called" is this project's other recurring bug: a rule written
 * in a place no test can reach is a rule nobody knows is still there.  This one
 * is a named function with no clock of its own -- the caller passes the time and
 * the beat in -- so the whole rule is walked on the host
 * (tests/unit/tx_watch.cpp) and the call itself is checked in the source
 * (tests/unit/platform_wiring.cpp).
 */

#include <stdbool.h>
#include <stdint.h>

/** Time without a new UI beat, ms, after which the feed stops. */
#define UIW_STALL_MS 3000

/** State of the rule. One instance, owned by the main thread. */
struct uiWatchdog
{
    uint32_t beat;    ///< Last beat value seen
    uint32_t beatAt;  ///< When that value last changed
    uint32_t stalls;  ///< Times the feed has been stopped (trace)
    bool     seeded;  ///< beatAt holds a valid time
    bool     stalled; ///< The feed is stopped: the UI is not advancing
};

/**
 * Reset the rule at boot.
 *
 * @param w: state.
 * @param now: current time, ms.
 * @param beat: the UI thread's beat counter as it stands.
 */
static inline void uiw_init(struct uiWatchdog *w, uint32_t now, uint32_t beat)
{
    w->beat    = beat;
    w->beatAt  = now;
    w->stalls  = 0;
    w->seeded  = true;
    w->stalled = false;
}

/**
 * Run the rule once: called from the main loop, once per pass, just before the
 * watchdog would be fed.
 *
 * @param w: state.
 * @param now: current time, ms.
 * @param beat: the UI thread's beat counter now.
 * @return true if the watchdog may be fed, false if the UI has stopped and the
 *         reset has to happen instead.
 */
static inline bool uiw_service(struct uiWatchdog *w, uint32_t now, uint32_t beat)
{
    if (!w->seeded)
        uiw_init(w, now, beat);

    if (beat != w->beat)
    {
        /* The UI advanced: the clock of the rule restarts, and a stall is over.
         * A UI thread that comes back after a long pass is fed again -- the
         * reset is the answer to a thread that never comes back, not to a slow
         * one. */
        w->beat    = beat;
        w->beatAt  = now;
        w->stalled = false;

        return true;
    }

    if ((!w->stalled) && ((uint32_t)(now - w->beatAt) > UIW_STALL_MS))
    {
        w->stalled = true;
        w->stalls++;

        return false;
    }

    return !w->stalled;
}

/**
 * Is the feed stopped?
 *
 * @param w: state.
 * @return true while the UI thread is considered parked.
 */
static inline bool uiw_stalled(const struct uiWatchdog *w)
{
    return w->stalled;
}

#endif /* UI_WATCHDOG_H */
