/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DSTAR_STATUS_H
#define OPENRTX_DSTAR_STATUS_H

/**
 * @file DStarStatus.h
 * @brief What the D-STAR mode has decoded: the call in progress, the slow-data
 * message and position, and the stations heard.
 *
 * Written by the rtx thread one frame at a time and copied out with
 * dstar_getStatus() / dstarHeard(), which are safe to call from the UI thread.
 *
 * It is also where the slow data meets the rest of the firmware:
 *
 *  - a complete 20-character message becomes an inbox entry through the shared
 *    store (core/messaging.h, transport MSG_TRANSPORT_DSTAR), deduplicated so a
 *    message repeated every superframe is stored once;
 *  - a position sentence updates the caller's fix and the heard-station entry,
 *    which is the D-STAR equivalent of APRS's received-station list: callsign,
 *    repeater, destination, last heard, position.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/DSTAR/DStarDefs.h"
#include "protocols/DSTAR/DStarFec.h"
#include "protocols/DSTAR/DStarPhy.h"
#include "protocols/DSTAR/DStarSlowData.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Stations remembered in the heard list. */
#define DSTAR_HEARD_MAX 8

/** One station heard, as the UI shows it. */
struct dstarHeardStation {
    char call[DSTAR_CALL_LEN + 1];    ///< Own callsign
    char sfx[DSTAR_SUFFIX_LEN + 1];   ///< Own callsign suffix
    char rpt1[DSTAR_CALL_LEN + 1];    ///< Source repeater ("DIRECT" when simplex)
    char rpt2[DSTAR_CALL_LEN + 1];    ///< Destination repeater
    char ur[DSTAR_CALL_LEN + 1];      ///< Destination station
    uint32_t heard;                   ///< Uptime in ms of the last frame
    uint32_t frames;                  ///< Frames received from it
    bool gpsValid;                    ///< A position was received
    int32_t lat;                      ///< Degrees * 1e6
    int32_t lon;                      ///< Degrees * 1e6
    int32_t alt;                      ///< Metres
};

/** The mode's status block. */
struct dstarStatus {
    bool sync;          ///< A transmission is being decoded
    bool voice;         ///< Voice frames arrived in the last second
    char my[DSTAR_CALL_LEN + 1];     ///< Caller
    char sfx[DSTAR_SUFFIX_LEN + 1];  ///< Caller's suffix
    char ur[DSTAR_CALL_LEN + 1];     ///< Destination ("CQCQCQ" for a general call)
    char rpt1[DSTAR_CALL_LEN + 1];   ///< Access repeater
    char rpt2[DSTAR_CALL_LEN + 1];   ///< Gateway / destination repeater
    uint8_t flags[3];                ///< Flag bytes of the last header
    int8_t viterbi;                  ///< Corrected channel bits of the last header
    char text[DSTAR_TEXT_LEN + 1];   ///< Last slow-data message
    bool gpsValid;                   ///< A position sentence was parsed
    bool gpsAprs;                    ///< ... and it came from a D-PRS line
    int32_t lat;                     ///< Degrees * 1e6
    int32_t lon;                     ///< Degrees * 1e6
    int32_t alt;                     ///< Metres
    int32_t speed;                   ///< km/h
    uint16_t course;                 ///< Degrees
    uint32_t headers;                ///< Headers with a good P_FCS
    uint32_t frames;                 ///< Frames decoded
    uint32_t resyncs;                ///< Resync words found where expected
    uint32_t texts;                  ///< Messages reassembled
    uint32_t sentences;              ///< Position sentences parsed
    uint32_t slowBlocks;             ///< Complete slow-data blocks
};

/** Clear the status, keeping the counters (new frequency, new call). */
void dstarStatus_reset(void);

/** Clear everything, counters included. */
void dstarStatus_clear(void);

/**
 * @brief Account a decoded header: fills the call fields and files the station.
 *
 * @param hdr: the header.
 * @param syncErrors: bit errors of the frame sync.
 * @param now: uptime in ms.
 */
void dstarStatus_header(const struct dstarHeader *hdr, int syncErrors, uint32_t now);

/**
 * @brief Account a decoded frame: keeps the slow data running, stores messages
 * and positions.
 *
 * @param frame: the frame.
 * @param now: uptime in ms.
 */
void dstarStatus_frame(const struct dstarPhyFrame *frame, uint32_t now);

/**
 * @brief The slow-data accumulator, so the mode can reset it on a new call.
 */
struct dstarSlow *dstarStatus_slow(void);

/** Copy the status out. */
void dstar_getStatus(struct dstarStatus *out);

/** @return the i-th heard station, most recent first, NULL out of range. */
const struct dstarHeardStation *dstarHeard(int i);

/** Own callsign suffix / destination the transmitter should use. */
void dstarStatus_setStation(const char *my, const char *sfx);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DSTAR_STATUS_H */
