/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DSTAR_SLOWDATA_H
#define OPENRTX_DSTAR_SLOWDATA_H

/**
 * @file DStarSlowData.h
 * @brief The D-STAR slow-data channel: 1200 bit/s inside the DV frames,
 * carrying the 20-character TX message, copies of the radio header, NMEA and
 * D-PRS position sentences, code squelch.
 *
 * The channel is 24 bits per frame, unscrambled by XOR with the first three
 * bytes of the PN sequence.  Two consecutive frames make one 6-byte block: the
 * first byte is a type in the top nibble and the payload length in the bottom
 * nibble, then up to five payload bytes, padded with 0x66 ('f').
 *
 * JARL STD-7 does not define this layer at all ("the specification of this data
 * is not defined other than every twenty-first frame contains a three byte
 * synchronisation vector"); the block layout, the types and the 20-character
 * message come from G4KLX's analysis of on-air dumps, "The Format of D-Star
 * Slow Data" (v0.2), and match what Icom radios emit.
 *
 * There is no addressed messaging in DV: the text is a 20-character label on
 * your own transmission.  GPS arrives as fragments of NMEA 0183 sentences
 * ($GPGGA / $GPRMC / free text / APRS "$$CRC" D-PRS); there is no packed
 * 20-bit coordinate in the standard slow data.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/DSTAR/DStarDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A position fix decoded from the slow data. */
struct dstarFix {
    bool valid;        ///< A sentence was parsed and had a fix
    bool fromAprs;     ///< Came from a D-PRS ($$CRC) line
    int32_t lat;       ///< Latitude, degrees * 1e6, north positive
    int32_t lon;       ///< Longitude, degrees * 1e6, east positive
    int32_t alt;       ///< Altitude, metres (0 when absent)
    int32_t speed;     ///< Speed, km/h (0 when absent)
    uint16_t course;   ///< Course, degrees (0 when absent)
};

/** Accumulator of the slow-data channel. */
struct dstarSlow {
    uint8_t half;                     ///< 0: next 3 bytes start a block
    uint8_t blk[DSTAR_SLOW_BLOCK];    ///< Block being assembled
    uint8_t hdr[45];                  ///< Radio-header copy being assembled
    uint8_t hdrLen;
    char text[DSTAR_TEXT_LEN + 1];    ///< TX message being assembled
    uint8_t textMask;                 ///< Fragments seen, bit per 5 characters
    char gps[DSTAR_GPS_LEN + 1];      ///< NMEA / D-PRS sentence being assembled
    uint8_t gpsLen;
    uint8_t lastType;                 ///< Type of the last complete block
    uint32_t blocks;                  ///< Complete blocks since reset
};

/** Clear the accumulator (new transmission). */
void dstarSlow_reset(struct dstarSlow *sd);

/**
 * @brief Push the 24 slow bits of one frame.
 *
 * @param sd: accumulator.
 * @param slow: three bytes, already de-scrambled, air order.
 * @return true when a complete 6-byte block was assembled by this call.
 */
bool dstarSlow_push(struct dstarSlow *sd, const uint8_t slow[DSTAR_SLOW_BYTES]);

/**
 * @brief The 20-character message, when all four fragments of the current
 * transmission have arrived (the mask is cleared after a copy).
 *
 * @return the text, or NULL.
 */
const char *dstarSlow_text(struct dstarSlow *sd);

/**
 * @brief A complete NMEA / D-PRS sentence, when one has just ended.
 *
 * @return the sentence (NUL terminated, trailing CR/LF removed), or NULL.
 */
const char *dstarSlow_sentence(struct dstarSlow *sd);

/**
 * @brief Parse a position sentence (NMEA 0183 or D-PRS), no libm.
 *
 * @param sentence: NUL terminated text.
 * @param fix: destination, zeroed first.
 * @return true when the sentence was understood and carried a fix.
 */
bool dstarFix_parse(const char *sentence, struct dstarFix *fix);

/**
 * @brief NMEA checksum of a sentence body ("GPGGA,..." without '$' and '*'),
 * hex digits.
 */
uint8_t dstarNmeaChecksum(const char *body, int len);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DSTAR_SLOWDATA_H */
