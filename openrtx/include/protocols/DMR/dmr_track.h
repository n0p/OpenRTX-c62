/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TRACK_H
#define DMR_TRACK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stations heard with an LRRP position: the last fix of each radio, for the
 * UI (side key + * in DMR). The receiver this decoder comes from drew them
 * on a map; here they are listed. Fed by the decoder in the rtx thread,
 * read by the UI. After gmtii's c62/dmr (37893f91).
 */

/* Radios kept; the one heard longest ago makes room for a new one */
#define DMR_TRACK_MAX 16

#define DMR_TRACK_ALIAS 32

typedef struct {
    uint32_t src;                /**< Radio ID */
    char alias[DMR_TRACK_ALIAS]; /**< Talker alias, "" if none */
    float lat, lon;              /**< Degrees, + = N / E */
    int16_t speed_kmh;           /**< -1 unknown */
    int16_t dir;                 /**< Heading in degrees, -1 unknown */
    uint32_t ms;                 /**< getTick() of the last fix */
    uint16_t reports;            /**< Fixes received */
} dmr_station_t;

/**
 * Record a position report.
 *
 * @param src: reporting radio.
 * @param alias: its talker alias, NULL or "" if unknown (the one known is
 * kept).
 */
void dmr_track_add(uint32_t src, float lat, float lon, int16_t speed_kmh,
                   int16_t dir, const char *alias);

/**
 * A talker alias was decoded: name the station, if it is in the table.
 */
void dmr_track_set_alias(uint32_t src, const char *alias);

/**
 * Copy the stations, the most recent fix first.
 *
 * @param out: destination.
 * @param max: entries in out.
 * @return number of stations copied.
 */
int dmr_track_get(dmr_station_t *out, int max);

/**
 * @return a number that changes with every report, for the UI's redraw.
 */
uint32_t dmr_track_serial(void);

/**
 * Forget every station (new channel, mode left).
 */
void dmr_track_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_TRACK_H */
