/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * APRS 1.0.1 information fields: position (uncompressed and compressed),
 * status, messages and acknowledgements, plus the received-station list.
 *
 * References: APRS Protocol Reference 1.0.1 (APRS101.PDF, 2000):
 *   chapter 8  position without timestamp, uncompressed
 *   chapter 9  position without timestamp, compressed
 *   chapter 12 messages, acknowledgements and rejection
 *   chapter 16 status reports
 * and AX.25 v2.2 for the frame layer.
 */
#ifndef APRS_H
#define APRS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ax25.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APRS_MAX_STATIONS  32
#define APRS_CALL_MAX      10
#define APRS_COMMENT_MAX   48

typedef enum
{
    APRS_INFO_INVALID = 0,
    APRS_INFO_POSITION,      /**< '!' or '=' : position, no timestamp   */
    APRS_INFO_POSITION_TS,   /**< '/' or '@' : position with timestamp  */
    APRS_INFO_STATUS,        /**< '>'                                   */
    APRS_INFO_MESSAGE,       /**< ':'                                   */
    APRS_INFO_ACK,           /**< message whose text is "ack" + number  */
    APRS_INFO_REJ,           /**< message whose text is "rej" + number  */
    APRS_INFO_OTHER          /**< anything else, kept for the raw view  */
} aprs_info_type_t;

typedef struct
{
    double  lat;             /**< degrees, north positive               */
    double  lon;             /**< degrees, east positive                */
    char    table;           /**< symbol table: '/', '\\' or overlay     */
    char    symbol;          /**< symbol code                           */
    bool    compressed;      /**< came in the compressed form           */
    char    comment[APRS_COMMENT_MAX];
} aprs_pos_t;

typedef struct
{
    aprs_info_type_t type;
    aprs_pos_t       pos;        /**< valid for POSITION / POSITION_TS   */
    char             addressee[APRS_CALL_MAX];  /**< MESSAGE / ACK      */
    char             text[APRS_COMMENT_MAX + 32];  /**< message or status */
    char             msgNo[6];   /**< message number, "" if none         */
} aprs_info_t;

/* ------------------------------------------------------------ encoding --- */

/**
 * Uncompressed position without timestamp (APRS101 chapter 8):
 * "!DDMM.mmN/DDDMM.mmW<symbol>comment".
 *
 * @param messaging: true emits '=' (station can receive messages) instead of
 *                   '!' (position only).
 * @return the information field length, or 0 if it does not fit.
 */
size_t aprs_build_position(char *out, size_t cap, const aprs_pos_t *p,
                           bool messaging);

/**
 * Compressed position without timestamp (APRS101 chapter 9): 1 byte of type,
 * 1 of symbol table, 4+4 base-91 bytes, 1 symbol byte, comment. Saves 4 bytes
 * over the uncompressed form and, on the C62, one full HDLC frame when the
 * comment is long.
 */
size_t aprs_build_position_compressed(char *out, size_t cap,
                                      const aprs_pos_t *p, bool messaging);

/** Status report (APRS101 chapter 16): ">text". */
size_t aprs_build_status(char *out, size_t cap, const char *text);

/** Message (APRS101 chapter 12): ":ADDRESSEE:text{nn". */
size_t aprs_build_message(char *out, size_t cap, const char *addressee,
                          const char *text, const char *msgNo);

/** Acknowledgement: ":ADDRESSEE:acknn". */
size_t aprs_build_ack(char *out, size_t cap, const char *addressee,
                      const char *msgNo);

/** Rejection: ":ADDRESSEE:rejnn". */
size_t aprs_build_rej(char *out, size_t cap, const char *addressee,
                      const char *msgNo);

/** Decode an information field. Returns false only if it is empty. */
bool aprs_parse_info(const char *info, size_t len, aprs_info_t *out);

/** Format a position for the UI ("43 44.50N 002 09.12W") or a fixed text. */
size_t aprs_format_position(char *out, size_t cap, const aprs_pos_t *p);

/** Parse "DDMM.mmN" / "DDDMM.mmW" (or decimal degrees) into p->lat / p->lon. */
bool aprs_parse_lat(const char *s, double *lat);
bool aprs_parse_lon(const char *s, double *lon);

/* ----------------------------------------------------------- stations --- */

typedef struct
{
    char     call[APRS_CALL_MAX];
    bool     hasPos;
    double   lat;
    double   lon;
    char     symbol;
    char     table;
    int32_t  distM;          /**< from the local position, 0 if unknown */
    uint16_t bearing;        /**< degrees from the local position       */
    uint32_t lastMs;         /**< uptime of the last packet, ms         */
    uint16_t packets;
    aprs_info_type_t lastType;
    char     comment[APRS_COMMENT_MAX];
} aprs_station_t;

typedef struct
{
    aprs_station_t st[APRS_MAX_STATIONS];
    uint8_t        count;
    uint8_t        next;     /**< round robin replacement victim        */
} aprs_station_list_t;

void aprs_station_list_init(aprs_station_list_t *l);

/**
 * Record a received frame. Updates the station entry (creating or replacing
 * one), computes distance and bearing from the local position if both are
 * known, and returns the entry.
 */
aprs_station_t *aprs_station_update(aprs_station_list_t *l,
                                    const ax25_frame_t *f,
                                    const aprs_info_t *info,
                                    bool haveLocal, double localLat,
                                    double localLon, uint32_t nowMs);

/** Great circle distance in metres. */
uint32_t aprs_distance_m(double lat1, double lon1, double lat2, double lon2);

/** Initial bearing in degrees (0 = north, clockwise). */
uint16_t aprs_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/**
 * Beacon interval check, with the APRS good practice guard: never less than
 * the configured interval and never less than 30 s.
 *
 * @return true if a beacon should be sent now.
 */
bool aprs_beacon_due(uint32_t lastMs, uint32_t intervalS, uint32_t nowMs);

/** True if the path contains an address with this callsign (own-packet test) */
bool aprs_path_contains(const ax25_frame_t *f, const char *call);

#ifdef __cplusplus
}
#endif

#endif /* APRS_H */
