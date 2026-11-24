/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTOCOLS_DPMR_DPMRDATA_H
#define PROTOCOLS_DPMR_DPMRDATA_H

/**
 * @file DpmrData.h
 * @brief dPMR short data: the 18 bit SLD field of the CCH (TS 102 490, 5.9).
 *
 * dPMR has no separate data frame in a voice call: the slow data field of the
 * CCH carries it, 18 bits per 80 ms frame, and those 18 bits mean two very
 * different things depending on the communication mode of the call.
 *
 *  - mode 001, voice + slow data (clause 5.9.1): two user bytes per frame,
 *    each preceded by a continuation flag that marks the last byte of the
 *    message.  This is what a CPS uses for short text between radios of the
 *    same system.
 *  - mode 101, voice + appended type 2 data (clause 5.9.2): the field is a
 *    header - 5 reserved bits, a 2 bit data position, a 4 bit format, a
 *    continuation flag and a 6 bit length - and the bytes themselves travel in
 *    the TCH of the following data frames, not here.
 *
 * The field is 18 bits, so it does not fit a uint16_t: the whole API takes
 * uint32_t and the top two bits are the continuation flag of the first byte.
 *
 * Nothing in this file allocates, and everything is a pure function of the 18
 * bits, so the host tests and the firmware share it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of user data the reassembler keeps: 16 frames of 2 bytes. */
#define DPMR_SLD_TEXT_BYTES 32

/** Data position of the SLD header, table 5.9a. */
enum dpmrDataPosition
{
    DPMR_DP_NONE = 0,           /**< 00: no data in this frame          */
    DPMR_DP_DATA = 3            /**< 11: this frame carries data        */
};

/** SLD header of a voice + appended data call, clause 5.9.2. */
struct dpmrSld
{
    uint8_t dp;                 /**< enum dpmrDataPosition              */
    uint8_t format;             /**< enum dpmrDataFormat, table 5.9b    */
    bool    last;               /**< continuation flag: data ends here  */
    uint8_t length;             /**< 6 bit data length, in bytes        */
};

/** Decode the 18 bit SLD field as the clause 5.9.2 header. */
void dpmrData_sldDecode(uint32_t sld, struct dpmrSld *out);

/** One frame of a voice + slow data call: two bytes and their flags. */
struct dpmrSlowData
{
    uint8_t byte[2];            /**< user bytes, in transmission order  */
    bool    last[2];            /**< continuation flag of each byte     */
};

/**
 * Decode the 18 bit SLD field of a voice + slow data call (clause 5.9.1).
 * @return number of user bytes, always 2
 */
unsigned dpmrData_slowDecode(uint32_t sld, struct dpmrSlowData *out);

/** Reassembly of a slow data message that spans several frames. */
struct dpmrSldMessage
{
    uint8_t text[DPMR_SLD_TEXT_BYTES];  /**< bytes received so far      */
    uint8_t len;                        /**< bytes in text              */
    bool    complete;                   /**< a continuation flag said so */
    bool    overflow;                   /**< the message did not fit     */
};

/** Start a new message. */
void dpmrSld_reset(struct dpmrSldMessage *m);

/**
 * Append one frame of slow data.
 *
 * The message keeps growing until a continuation flag marks its last byte;
 * then complete is set and the bytes are kept for the caller to read or copy
 * before the next dpmrSld_reset().  A message longer than DPMR_SLD_TEXT_BYTES
 * sets overflow and is truncated, so a corrupt stream cannot run away.
 */
void dpmrSld_push(struct dpmrSldMessage *m, const struct dpmrSlowData *d);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOLS_DPMR_DPMRDATA_H */
