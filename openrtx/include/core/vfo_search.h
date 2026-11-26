/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VFO_SEARCH_H
#define VFO_SEARCH_H

/**
 * @file vfo_search.h
 * @brief VFO carrier search: a state machine, no OS, no radio, no UI.
 *
 * A carrier search walks the VFO up or down, one step of the mode at a time,
 * starting where the operator already is, and stops on the first carrier it
 * believes in.  It is a different thing from the memory scan of
 * core/scanner.h: that one walks a list of channels (or a range) and applies a
 * resume rule, this one is a single point sweep of the VFO that ends for good
 * where it finds something.  They share the squelch and the retune path, not
 * the machine -- see docs/VFO_SEARCH.md, section 1, for why a second small
 * machine was cheaper than a third resume rule inside the scanner.
 *
 * The criterion that ends the search depends on the mode:
 *
 *  - analog (FM, AM airband): the squelch opens and stays open for
 *    `debounceMs`, which is the analog squelch detector of the mode itself;
 *  - digital (DMR, M17, TETRA, NXDN, P25, YSF): the mode reports synchronism
 *    *and* the number of frames that decoded with a valid check.  A sync word
 *    without frames behind it is a false positive; the machine asks for
 *    `syncFrames` valid frames (or, in a mode that publishes no counter, for
 *    `syncMs` of uninterrupted sync) before it stops.
 *
 * Once it stops on a carrier it stays there: no timer, no resume, no second
 * sweep.  The machine is over (`vfoSearch_isRunning()` turns false) and the
 * receiver is left on the frequency the action names.
 *
 * The machine is pure: the time and a sample of the receiver arrive as
 * arguments, and the answer is an action saying what happened and where the
 * receiver has to go.  That is what makes a minute of sweeping testable on a
 * host in a few microseconds (tests/unit/vfo_search.cpp).
 *
 * | state  | dwell                                        | leaves on                     |
 * |--------|----------------------------------------------|-------------------------------|
 * | IDLE   | not searching                                | vfoSearch_start()             |
 * | LISTEN | `dwellMs` (after `settleMs` in TUNE)         | carrier, sync, timer          |
 * | TUNE   | `settleMs` after a retune                    | timer                         |
 * | FOUND  | stopped on a carrier, stays for good         | nothing (the operator retunes)|
 * | END    | a whole lap of the band with nothing on it   | nothing                       |
 *
 * Events, in the terms the operator sees them:
 *
 * | event                  | meaning                                          |
 * |------------------------|--------------------------------------------------|
 * | VFOS_EV_STARTED        | the search began on `frequency`                  |
 * | VFOS_EV_TUNED          | stepped to `frequency`                           |
 * | VFOS_EV_FOUND          | carrier or sync: the search stopped, `result`    |
 * |                        | says which, `corrected` if it was retuned to the|
 * |                        | frequency the network published                  |
 * | VFOS_EV_CANCELLED      | the operator stopped it (key, PTT, red key)      |
 * | VFOS_EV_BAND_END       | a full lap with nothing on it                    |
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Direction of the sweep. */
enum vfoSearchDirection
{
    VFOS_UP   = 0,
    VFOS_DOWN = 1
};

/** Where the machine is, as reported to the interface. */
enum vfoSearchState
{
    VFOS_STATE_IDLE = 0, /**< Not searching                                  */
    VFOS_STATE_LISTEN,   /**< Dwelling on a frequency                        */
    VFOS_STATE_TUNE,     /**< Retuned, waiting for the receiver to settle    */
    VFOS_STATE_FOUND,    /**< Stopped on a carrier, stays there              */
    VFOS_STATE_END,      /**< Band swept, nothing found                      */
    VFOS_STATE_COUNT
};

/** What happened, as an event the interface can show or count. */
enum vfoSearchEvent
{
    VFOS_EV_NONE = 0,
    VFOS_EV_STARTED,
    VFOS_EV_TUNED,
    VFOS_EV_FOUND,
    VFOS_EV_CANCELLED,
    VFOS_EV_BAND_END,
    VFOS_EV_COUNT
};

/** Why the search ended, as the screen words it. */
enum vfoSearchResult
{
    VFOS_RESULT_NONE = 0,  /**< No search has ended yet                       */
    VFOS_RESULT_CARRIER,   /**< The analog squelch opened                     */
    VFOS_RESULT_SYNC,      /**< Digital synchronism with valid frames         */
    VFOS_RESULT_CANCELLED, /**< The operator stopped it                       */
    VFOS_RESULT_BAND_END,  /**< A full lap of the band, nothing on it         */
    VFOS_RESULT_COUNT
};

/** Where an action wants the receiver. */
enum vfoSearchTarget
{
    VFOS_TARGET_NONE = 0, /**< Stay where it is                              */
    VFOS_TARGET_FREQUENCY /**< Retune to `frequency`                         */
};

/**
 * Configuration of a search run.
 *
 * The service (core/vfo_search_service.h) fills this from the mode and the
 * step in use; a test fills it by hand with millisecond timers.
 */
typedef struct
{
    uint8_t  direction;     /**< enum vfoSearchDirection                     */
    uint8_t  digital;       /**< 1: stop on synchronism, 0: on the squelch    */

    uint32_t step;          /**< Tuning step, Hz                             */
    uint32_t low;           /**< Band limits, Hz, inclusive                  */
    uint32_t high;

    uint16_t settleMs;      /**< After a retune, before the receiver is
                             *   believed (the mode reinitialises)          */
    uint16_t dwellMs;       /**< Time on each frequency before stepping      */
    uint16_t debounceMs;    /**< Analog: squelch stability to believe it     */

    uint16_t syncFrames;    /**< Digital: valid frames required (2 or 3)     */
    uint16_t syncHoldMs;    /**< Digital: how long the sync has to hold for
                             *   the frames to count                        */
    uint16_t syncMs;        /**< Digital, mode with no frame counter: time of
                             *   uninterrupted sync required                */

    int32_t  offsetMin;     /**< Hz: smallest carrier offset worth a retune  */
    int32_t  offsetMax;     /**< Hz: largest offset accepted as a correction */
    uint32_t reportedMax;   /**< Hz: a published frequency further than this
                             *   from the tuning is ignored                 */
} vfoSearchConfig_t;

/**
 * One sample of the receiver, taken by the caller.
 *
 * `frames` is a counter that only grows while the mode decodes frames whose
 * check passed (a DMR burst, a NXDN LICH, a P25 frame, a TETRA CRC block); a
 * mode with no such counter sets `haveFrames` to false and the machine falls
 * back to the time criterion.
 */
typedef struct
{
    bool     squelch;    /**< Analog squelch open                           */
    bool     synced;     /**< The mode reports synchronism                  */
    bool     haveFrames; /**< `frames` is meaningful                        */
    uint32_t frames;     /**< Valid frames since the mode was enabled       */
    bool     offsetValid;/**< The mode measured a carrier offset            */
    int32_t  offsetHz;   /**< Hz to add to the tuning to correct it         */
    bool     reported;   /**< The network published its absolute frequency  */
    uint32_t reportedHz; /**< That frequency, Hz                            */
} vfoSearchSample_t;

/** What the machine asks for after a step. */
typedef struct
{
    uint8_t  event;     /**< enum vfoSearchEvent                           */
    uint8_t  target;    /**< enum vfoSearchTarget                          */
    uint32_t frequency; /**< Hz for VFOS_TARGET_FREQUENCY                  */
    bool     corrected; /**< The stop frequency came from the network      */
    int32_t  offsetHz;  /**< Correction applied to the tuned frequency, Hz */
    uint8_t  result;    /**< enum vfoSearchResult, when the search ended   */
} vfoSearchAction_t;

/** The machine.  Treat the fields as private. */
typedef struct
{
    vfoSearchConfig_t cfg;

    uint8_t  state;
    uint8_t  running;
    uint8_t  result;         /**< Last result, kept after the stop          */
    uint8_t  corrected;      /**< The last stop was a frequency correction  */
    int32_t  offset;         /**< Correction of the last stop, Hz           */

    uint32_t start;          /**< Frequency the search began on             */
    uint32_t frequency;      /**< Frequency the sweep is at                 */
    uint32_t stateTime;      /**< When the current state was entered        */
    uint32_t syncSince;      /**< When sync appeared here, 0 if it did not  */
    uint32_t signalSince;    /**< When the squelch opened here, 0 if not    */
    uint32_t framesBase;     /**< Valid-frame counter at the last retune    */
    uint8_t  haveFramesBase;
    uint8_t  wrapped;        /**< The sweep has wrapped around the band     */
    uint32_t visited;        /**< Frequencies listened to                   */
} vfoSearch_t;

/**
 * Whether a search may be started at all.
 *
 * The carrier search belongs to the VFO: it is started with a long press of
 * UP or DOWN on the VFO main screen and nowhere else, so a long press on the
 * channel screen, in the menus or on the frequency input screen starts
 * nothing.  A transmission and a running memory scan both own the receiver, so
 * neither can be interrupted by a search either.
 *
 * @param vfoScreen: the main screen in use is the VFO one.
 * @param transmitting: the radio is transmitting.
 * @param scanning: a memory / VFO scan of core/scanner.h is running.
 * @return true when a search may start.
 */
bool vfoSearch_mayStart(bool vfoScreen, bool transmitting, bool scanning);

/**
 * Fill a configuration with the defaults the service uses: up, 12.5 kHz,
 * 144-146 MHz, 40 ms of settle, 120 ms of dwell, 20 ms of debounce, two valid
 * frames over 100 ms, 400 ms of sync where there is no counter (longer than
 * any mode keeps its sync flag set without decoding), a correction from 200 Hz
 * to 3 kHz, and a published frequency within 25 kHz.
 */
void vfoSearch_defaultConfig(vfoSearchConfig_t *cfg);

/** Reset a machine to idle, with the default configuration. */
void vfoSearch_init(vfoSearch_t *s);

/**
 * Apply a configuration.  Refused (false) while a search is running: changing
 * the band or the step in the middle of a run would make the state mean two
 * things.
 */
bool vfoSearch_configure(vfoSearch_t *s, const vfoSearchConfig_t *cfg);

/**
 * Begin a search on the frequency the receiver is already tuned to.
 *
 * The machine starts listening there straight away: the receiver is settled
 * and retuning to the same frequency would only reinitialise the mode.  The
 * action reports VFOS_EV_STARTED with no retune.
 *
 * The sample is the receiver as it is now, and it matters for a digital mode:
 * the frames already decoded are the baseline, so only the frames that arrive
 * *while listening* count as evidence.  With a NULL sample the baseline is
 * zero, which is only right for a mode that has just been enabled.
 *
 * @param frequency: current VFO frequency, Hz.
 * @param now: current time, ms.
 * @param sample: receiver sample, may be NULL.
 * @param action: receives the event, may be NULL.
 * @return false if a search is already running or the configuration is
 *         unusable (step 0, empty band, start outside the band).
 */
bool vfoSearch_start(vfoSearch_t *s, uint32_t frequency, uint32_t now,
                     const vfoSearchSample_t *sample,
                     vfoSearchAction_t *action);

/**
 * Stop the search and stay on the frequency it is at.  Safe to call when not
 * searching (returns false and does nothing).
 *
 * The caller keeps the receiver where it is: the action carries no retune,
 * which is the difference with a band end (that one goes back to the
 * frequency the search started from).
 */
bool vfoSearch_stop(vfoSearch_t *s, uint32_t now, vfoSearchAction_t *action);

/**
 * Advance the machine by one sample.
 *
 * @param sample: what the receiver reports right now, never NULL.
 * @param now: current time, ms.
 * @param action: always written, may be NULL when the caller does not care.
 */
void vfoSearch_step(vfoSearch_t *s, const vfoSearchSample_t *sample,
                    uint32_t now, vfoSearchAction_t *action);

/** True while the sweep can still move (false in FOUND and END). */
bool vfoSearch_isRunning(const vfoSearch_t *s);

/** Current state, enum vfoSearchState. */
uint8_t vfoSearch_state(const vfoSearch_t *s);

/** Result of the last stop, enum vfoSearchResult. */
uint8_t vfoSearch_result(const vfoSearch_t *s);

/** Frequency the sweep is at (or stopped on), Hz. */
uint32_t vfoSearch_frequency(const vfoSearch_t *s);

/** Correction applied on the last stop, Hz: positive means it tuned up. */
int32_t vfoSearch_offset(const vfoSearch_t *s);

/** Frequencies listened to since the search began. */
uint32_t vfoSearch_visited(const vfoSearch_t *s);

/** "idle", "listen", ...: short name of a state, for displays and logs. */
const char *vfoSearch_stateName(uint8_t state);

/** "found", "tuned", ...: short name of an event. */
const char *vfoSearch_eventName(uint8_t event);

/** "carrier", "sync", ...: short name of a result. */
const char *vfoSearch_resultName(uint8_t result);

#ifdef __cplusplus
}
#endif

#endif /* VFO_SEARCH_H */
