/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VFO_SEARCH_SERVICE_H
#define VFO_SEARCH_SERVICE_H

/**
 * @file vfo_search_service.h
 * @brief VFO carrier search wired to the radio: the squelch, the digital
 *        synchronism, the retune and the band.  docs/VFO_SEARCH.md is the map
 *        of the feature.
 *
 * This is the only part of the search that knows the firmware exists.  It
 * samples the receiver, feeds core/vfo_search.h, turns its actions into a
 * VFO frequency (state.channel plus an RTX synchronisation, exactly the door
 * the quick menu and the scan use) and keeps the result the screen shows.
 *
 * It runs in the main thread, from state_task(), every 5 ms.
 *
 * What the interface has to do, and nothing else:
 *
 *   - on the VFO main screen, arm a long press of UP or DOWN and call
 *     vfoSearchService_start(VFOS_UP) / vfoSearchService_start(VFOS_DOWN) once
 *     the key has been held for three seconds;
 *   - while vfoSearchService_active(), take any key, stop the search with
 *     vfoSearchService_stop() and consume the key (it does nothing else);
 *   - draw vfoSearchService_status() while searching and for a couple of
 *     seconds after it ends, to show the reason.
 *
 * PTT needs no hook: the service sees the transmission, stops the search and
 * leaves the receiver where the sweep was, without touching the call.
 */

#include <stdbool.h>
#include <stdint.h>

#include "core/vfo_search.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How long the result of a finished search stays worth showing, ms. */
#define VFO_SEARCH_MESSAGE_MS 2500

/** Snapshot of the search, for a screen or a status line. */
struct vfoSearchStatus
{
    bool     active;     /**< A search is running                         */
    uint8_t  state;      /**< enum vfoSearchState                         */
    uint8_t  direction;  /**< enum vfoSearchDirection                     */
    uint8_t  result;     /**< enum vfoSearchResult of the last stop        */
    bool     corrected;  /**< The last stop came from the network         */
    uint32_t frequency;  /**< Hz: where the sweep is, or stopped          */
    int32_t  offsetHz;   /**< Correction applied on the last stop, Hz     */
    uint32_t visited;    /**< Frequencies listened to in this run         */
    uint16_t progress;   /**< Tenths of a percent of the band swept       */
    uint32_t age;        /**< ms since the last stop, 0 while running     */
};

/**
 * Prepare the service.  Called from state_init(): it only resets the machine.
 */
void vfoSearchService_init(void);

/**
 * Start a carrier search from the frequency in use.
 *
 * Refused (false) when the main screen is not the VFO one, when the radio is
 * transmitting, when a memory scan is running, or when the frequency is not
 * inside a band the radio can sweep.
 *
 * @param direction: VFOS_UP or VFOS_DOWN.
 * @return true if the sweep began.
 */
bool vfoSearchService_start(uint8_t direction);

/**
 * Stop a running search and leave the receiver on the frequency the sweep is
 * at.  Safe to call when not searching, and safe to call while transmitting:
 * the retune it may owe is applied when the transmission ends.
 */
void vfoSearchService_stop(void);

/**
 * Periodic step: sample the receiver, advance the machine and move the radio.
 * Called from state_task() every 5 ms; it costs a comparison while no search
 * is running.
 */
void vfoSearchService_poll(void);

/** True while a search is running. */
bool vfoSearchService_active(void);

/** Copy the current state, for the screen. */
bool vfoSearchService_status(struct vfoSearchStatus *out);

/**
 * Short text for the screen: the reason the search stopped ("carrier",
 * "sync", "net"), or the state while it runs.  Never NULL.
 */
const char *vfoSearchService_reasonName(uint8_t result, bool corrected);

#ifdef __cplusplus
}
#endif

#endif /* VFO_SEARCH_SERVICE_H */
