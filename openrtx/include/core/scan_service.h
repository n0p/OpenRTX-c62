/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SCAN_SERVICE_H
#define SCAN_SERVICE_H

/**
 * @file scan_service.h
 * @brief Scan and vigilance, wired to the radio, the channel bank and the
 *        settings.  docs/SCAN.md is the map of the whole feature.
 *
 * This is the only part of the scan that knows the firmware exists: it samples
 * the squelch and the transmit state, feeds core/scanner.h, turns its actions
 * into a channel of state.channel plus an RTX synchronisation, and keeps the
 * events the interface shows on screen.
 *
 * It runs in the main thread, from state_task(), every 5 ms.
 *
 * What the interface has to do, and nothing else (the interface files are not
 * touched by the scan, see docs/SCAN.md for the exact hooks):
 *
 *   - call scan_service_start(SCAN_MODE_MEMORY) or
 *     scan_service_start(SCAN_MODE_VFO) from a key;
 *   - call scan_service_stop() before any manual retune or channel change;
 *   - call scan_service_popEvent() while drawing, to show what happened.
 *
 * There is no need to stop it on PTT: the service sees the transmission, stops
 * the scan and puts the receiver back where it was, without touching the
 * configuration of a call in progress.
 */

#include <stdbool.h>
#include <stdint.h>
#include "core/scanner.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Event kept for the interface: what happened and where. */
struct scanEventMsg
{
    uint8_t  event;      /**< enum scanEvent                            */
    uint16_t channel;    /**< Channel the event refers to, may be NONE  */
    uint32_t frequency;  /**< Frequency the event refers to, 0 if none  */
};

/** Snapshot of the scan, for a screen or a status line. */
struct scanStatus
{
    bool     running;    /**< A scan session is active                  */
    uint8_t  state;      /**< enum scanState                            */
    uint8_t  mode;       /**< enum scanMode of the running scan         */
    uint16_t channel;    /**< Channel being scanned, NONE on VFO        */
    uint32_t frequency;  /**< Frequency being scanned, 0 on channels    */
    uint32_t visited;    /**< Channels / frequencies since the start    */
    uint32_t stops;      /**< Times activity stopped the scan           */
};

/**
 * Prepare the service.  Called from state_init(), after the channel bank and
 * the settings are loaded: it only resets the machine.
 */
void scan_service_init(void);

/**
 * Start scanning.
 *
 * @param mode: SCAN_MODE_MEMORY walks the channel bank, SCAN_MODE_VFO walks the
 *              range in the settings.
 * @return false if a scan is already running, if there is nothing to walk
 *         (empty bank) or if the radio is transmitting.
 */
bool scan_service_start(uint8_t mode);

/**
 * Stop scanning and put the receiver back on the channel / frequency the scan
 * started from.  Safe to call when not scanning, and safe to call from the
 * middle of a call: the retune waits for the transmission to end.
 */
void scan_service_stop(void);

/**
 * Periodic step: sample the squelch and the transmit state, advance the
 * machine and move the radio.  Called from state_task() every 5 ms; does
 * nothing (and costs nothing) while no scan is running.
 */
void scan_service_poll(void);

/** Copy the current state of the scan. */
bool scan_service_status(struct scanStatus *out);

/**
 * Take one event out of the queue, oldest first.
 *
 * @return true if one was returned, false when the queue is empty.
 */
bool scan_service_popEvent(struct scanEventMsg *out);

/** Short name of the running state ("listen", "signal", ...), or "idle". */
const char *scan_service_stateName(void);

#ifdef __cplusplus
}
#endif

#endif /* SCAN_SERVICE_H */
