/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SCANNER_H
#define SCANNER_H

/**
 * @file scanner.h
 * @brief Scan and vigilance: a state machine, no OS, no radio, no UI.
 *
 * The scanner walks a list of channels (a memory scan) or a frequency range (a
 * VFO scan), stops on activity and starts again according to the configured
 * resume rule.  It never touches the receiver: it is handed a list to walk and
 * a sample of the squelch, and it answers with an action saying where to tune
 * and what happened.  That is what makes the whole thing testable on a host
 * with a synthetic squelch, and it is also why the same machine can drive a
 * radio, a simulated receiver or a recording replayer.
 *
 * The two halves are:
 *
 *   - this file: the pure machine (scanner.c), driven by scan_step();
 *   - core/scan_service.h: the glue that reads the channel store, the
 *     squelch and the settings, and moves the radio.
 *
 * Times are milliseconds from any monotonic clock, passed in by the caller:
 * the machine never reads a clock, so a test can run a minute of scanning in a
 * few microseconds.  All the timers are compared as `now - start`, so the
 * 32-bit wrap is harmless.
 *
 * States and what they mean:
 *
 * | state    | dwell                         | leaves on                       |
 * |----------|-------------------------------|---------------------------------|
 * | IDLE     | not scanning                  | scan_start()                    |
 * | TUNE     | settleMs after a retune       | timer                           |
 * | LISTEN   | dwellMs on the target         | activity, timer                 |
 * | SIGNAL   | stopped on a signal           | resume rule, PTT, scan_stop()   |
 * | HANG     | hangMs after the carrier fell | activity again, timer           |
 * | PRIORITY | settleMs + dwellMs on prio    | activity, timer                 |
 * | END      | range finished (no loop)      | scan_stop(), scan_resume()      |
 *
 * Events, in the terms the operator sees them:
 *
 * | event                | meaning                                          |
 * |----------------------|--------------------------------------------------|
 * | SCAN_EVENT_STARTED   | the scan began, `target` says where it tuned     |
 * | SCAN_EVENT_TUNED     | moved to another channel / frequency             |
 * | SCAN_EVENT_SIGNAL    | squelch open (or digital sync): the scan stopped |
 * | SCAN_EVENT_RESUME    | the signal ended, back to stepping               |
 * | SCAN_EVENT_RANGE_END | full lap of the bank, or the end of the VFO range |
 * | SCAN_EVENT_PRIORITY  | the priority channel has activity               |
 * | SCAN_EVENT_STOPPED   | the scan is over (operator, PTT, end of range)   |
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Value of a channel index meaning "no channel". */
#define SCAN_CHANNEL_NONE 0xFFFFU

/** Where the scan walks. */
enum scanMode
{
    SCAN_MODE_MEMORY = 0, /**< The channels of the bank                        */
    SCAN_MODE_VFO    = 1  /**< A frequency range, in steps of the configured one */
};

/** What the scan does once the squelch has opened. */
enum scanResume
{
    SCAN_RESUME_CARRIER = 0, /**< Stay while the carrier is there, hangMs after
                              *   it drops, then carry on                   */
    SCAN_RESUME_TIMEOUT = 1, /**< Stay holdMs from the moment the signal
                              *   appeared, then carry on even if it is still
                              *   there (a long call is abandoned on purpose) */
    SCAN_RESUME_HOLD    = 2  /**< Stay until the operator resumes           */
};

/** Direction of a VFO scan. */
enum scanDirection
{
    SCAN_UP   = 0,
    SCAN_DOWN = 1
};

/** States of the machine, as reported to the interface. */
enum scanState
{
    SCAN_STATE_IDLE = 0,  /**< Not scanning                                  */
    SCAN_STATE_TUNE,      /**< Retuned, waiting for the receiver to settle   */
    SCAN_STATE_LISTEN,    /**< Dwelling on a channel / frequency             */
    SCAN_STATE_SIGNAL,    /**< Stopped on activity                           */
    SCAN_STATE_HANG,      /**< Carrier dropped, waiting before resuming      */
    SCAN_STATE_PRIORITY,  /**< Checking the priority channel                 */
    SCAN_STATE_END,       /**< End of the range, scanning is over            */
    SCAN_STATE_COUNT
};

/** What happened, as an event the interface can show or count. */
enum scanEvent
{
    SCAN_EVENT_NONE = 0,
    SCAN_EVENT_STARTED,
    SCAN_EVENT_TUNED,
    SCAN_EVENT_SIGNAL,
    SCAN_EVENT_RESUME,
    SCAN_EVENT_RANGE_END,
    SCAN_EVENT_PRIORITY,
    SCAN_EVENT_STOPPED,
    SCAN_EVENT_COUNT
};

/** Where an action wants the receiver. */
enum scanTarget
{
    SCAN_TARGET_NONE = 0,   /**< Stay where it is                          */
    SCAN_TARGET_CHANNEL,    /**< Tune to `channel` of the bank             */
    SCAN_TARGET_FREQUENCY,  /**< Tune the VFO to `frequency`               */
    SCAN_TARGET_HOME        /**< Back to what was in use when the scan began */
};

/**
 * Configuration of a scan run.
 *
 * The defaults in scan_defaultConfig() are the ones the settings fall back to;
 * a menu edits them, scan_service.c maps settings_t onto this structure.
 */
typedef struct
{
    uint8_t  mode;              /**< enum scanMode                           */
    uint8_t  resume;            /**< enum scanResume                         */
    uint8_t  direction;         /**< enum scanDirection, VFO only            */
    uint8_t  loop;              /**< VFO: wrap at the limits. Memory always
                                 *   wraps; loop off only decides whether
                                 *   the machine reports the lap and stops. */
    uint8_t  priorityEnabled;   /**< Check a priority channel between steps   */
    uint16_t priority;          /**< Channel index, or SCAN_CHANNEL_NONE      */

    uint16_t settleMs;          /**< After a retune, before the squelch is
                                 *   believed (the mode reinitialises)       */
    uint16_t dwellMs;           /**< Time on each channel before stepping     */
    uint16_t hangMs;            /**< SCAN_RESUME_CARRIER: wait after the
                                 *   carrier drops                            */
    uint16_t holdMs;            /**< SCAN_RESUME_TIMEOUT: time on a signal    */
    uint16_t debounceMs;        /**< Squelch stability required to believe a
                                 *   change (chatter filter)                  */
    uint16_t priorityIntervalMs;/**< Between priority checks; 0 = every step  */

    uint32_t vfoLow;            /**< VFO scan limits, Hz, inclusive           */
    uint32_t vfoHigh;
    uint32_t vfoStep;           /**< VFO step, Hz                             */
} scanConfig_t;

/** One entry of the list the machine walks. */
typedef struct
{
    uint32_t frequency; /**< RX frequency, for information and for reports */
    uint8_t  mode;      /**< enum opmode, for information                  */
    uint8_t  skip;      /**< 1: the operator asked to skip this channel    */
} scanEntry_t;

/**
 * The list a memory scan walks.  Both calls are pure: they read, they do not
 * tune, and the machine calls them only while stepping, so a bank backed by
 * flash costs two reads per step and nothing while listening.
 *
 * `info` returns 0 for a usable index and a negative value otherwise; an index
 * that fails is treated like a skipped one, so a bank that changes under the
 * scan cannot stall it.
 */
typedef struct
{
    int (*count)(void *ctx);
    int (*info)(void *ctx, uint16_t idx, scanEntry_t *out);
    void *ctx;
} scanList_t;

/**
 * What the machine asks for after a step.
 *
 * `event` says what happened and `target` says where the receiver has to go;
 * both can be present (a lap of the bank is a RANGE_END that also tunes to the
 * first channel again).  A caller that ignores `target` keeps receiving where
 * it was, which is what a test that only checks events does.
 */
typedef struct
{
    uint8_t  event;     /**< enum scanEvent                                */
    uint8_t  target;    /**< enum scanTarget, TARGET_NONE for no retune     */
    uint16_t channel;   /**< Channel index for SCAN_TARGET_CHANNEL          */
    uint32_t frequency; /**< Hz for SCAN_TARGET_FREQUENCY                   */
} scanAction_t;

/** Squelch tracker: the raw sample and how long it has been stable. */
typedef struct
{
    uint8_t  raw;
    uint8_t  stable;
    uint32_t since;
} scanSquelch_t;

/** The machine.  Treat the fields as private. */
typedef struct
{
    scanConfig_t cfg;
    const scanList_t *list;

    uint8_t  state;
    uint8_t  running;
    uint8_t  onPriority;      /**< The current target is the priority one    */

    uint16_t cursor;          /**< Channel the walk is at                    */
    uint32_t vfoFreq;         /**< Frequency the walk is at                  */
    uint32_t lastPriority;    /**< When the priority channel was last seen   */
    uint32_t stateTime;       /**< When the current state was entered        */
    uint32_t signalTime;      /**< When the current signal appeared          */

    scanAction_t pending;     /**< Target to resume to after a priority check */
    scanSquelch_t squelch;

    uint32_t visited;         /**< Channels / frequencies stepped over       */
    uint32_t stops;           /**< Times activity stopped the scan           */
} scanner_t;

/** Fill a configuration with the defaults described in this header. */
void scan_defaultConfig(scanConfig_t *cfg);

/** Reset a machine to idle, with the default configuration. */
void scan_init(scanner_t *s);

/** Attach the list a memory scan walks.  May be NULL (a VFO-only machine). */
void scan_setList(scanner_t *s, const scanList_t *list);

/**
 * Apply a configuration.  Refused while a scan is running: changing the dwell
 * or the range in the middle of a run would make the state mean two things.
 *
 * @return true if it was applied.
 */
bool scan_configure(scanner_t *s, const scanConfig_t *cfg);

/**
 * Begin scanning.
 *
 * @param channel: channel in use, the walk continues after it. Pass
 *                 SCAN_CHANNEL_NONE to start at the first one.
 * @param frequency: VFO frequency in use, the walk continues from it.
 * @param now: current time, ms.
 * @param action: receives STARTED plus the first target, may be NULL.
 * @return false if a scan is already running.
 */
bool scan_start(scanner_t *s, uint16_t channel, uint32_t frequency,
                uint32_t now, scanAction_t *action);

/**
 * Stop scanning.  The action asks for SCAN_TARGET_HOME, so the caller can put
 * the receiver back where it was; the machine is idle afterwards and safe to
 * restart.
 */
void scan_stop(scanner_t *s, uint32_t now, scanAction_t *action);

/**
 * Leave a stop the resume rule would not leave by itself: the operator
 * pressed the key again on a SCAN_RESUME_HOLD stop, or on an END state.
 * Ignored (returns false) when the machine is not stopped.
 */
bool scan_resume(scanner_t *s, uint32_t now, scanAction_t *action);

/**
 * Advance the machine by one sample.
 *
 * @param signal: the squelch is open, or the mode reports a digital carrier
 *                (a DMR sync, an M17 LSF): any indication of activity.
 * @param transmitting: the radio is transmitting.  The scan stops at once:
 *                a scan must never be in the way of a transmission.
 * @param now: current time, ms.
 * @param action: always written; may be NULL when the caller does not care.
 */
void scan_step(scanner_t *s, bool signal, bool transmitting, uint32_t now,
               scanAction_t *action);

/** True while a scan is running (not counting an END state). */
bool scan_isRunning(const scanner_t *s);

/** Current state, enum scanState. */
uint8_t scan_state(const scanner_t *s);

/** Channel the walk is at, or SCAN_CHANNEL_NONE for a VFO scan. */
uint16_t scan_channel(const scanner_t *s);

/** Frequency the walk is at (0 for a memory scan). */
uint32_t scan_frequency(const scanner_t *s);

/** Channels / frequencies stepped over since the scan began. */
uint32_t scan_visited(const scanner_t *s);

/** Times activity stopped the scan since it began. */
uint32_t scan_stops(const scanner_t *s);

/** "idle", "tune", ...: short name of a state, for displays and logs. */
const char *scan_stateName(uint8_t state);

/** "tuned", "signal", ...: short name of an event. */
const char *scan_eventName(uint8_t event);

#ifdef __cplusplus
}
#endif

#endif /* SCANNER_H */
