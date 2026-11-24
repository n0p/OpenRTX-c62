/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZONES_H
#define ZONES_H

/**
 * @file zones.h
 * @brief Channel plans as data: the Spanish VHF and UHF repeater channels and
 * the PMR446 channels.
 *
 * These are tables, not screens.  A zone is a plan (a base frequency, a step, a
 * number of channels and a repeater shift); a channel of a zone is computed
 * from it when it is asked for, so 125 channels cost a few dozen bytes of
 * flash and none of RAM.  The channel editor turns one into a stored channel
 * with zone_toStore(), and nothing else in the firmware needs to know that
 * zones exist.
 *
 * The frequencies are the Spanish plan, cited per zone in docs/ZONES.md:
 *
 *  - R0..R7, VHF: repeater outputs 145.6000-145.7750 MHz every 25 kHz, inputs
 *    600 kHz below (144.000-146.000 MHz, 16 channels of 12.5 kHz per the
 *    Reglamento de uso del dominio público radioeléctrico por aficionados,
 *    BOE nº 137 of 9 June 2006, §5.1; R0-R7 are the 25 kHz subset that the
 *    URE numbers R0..R7 / RV48..RV62).
 *  - U0..U100, UHF: 438.6500 MHz and up every 12.5 kHz, inputs 7.6 MHz below
 *    (same regulation §5.2: 32 channels of 25 kHz from 438.650 to 439.425,
 *    inputs 431.050-431.825).  The 25 kHz channels are the even indices; the
 *    odd ones are the half-channel offsets and are flagged as such.
 *  - PMR1..PMR16: 446.00625-446.19375 MHz every 12.5 kHz, 500 mW fixed
 *    (ERC/DEC/(98)25 and ECC/DEC/(15)05 for the 16-channel analogue/digital
 *    PMR446 band).
 *
 * @note The direction of the repeater shift is data: `shift` is signed, and
 * zones_setShiftInverted() flips it for the plans where the operator's radio
 * transmits on the higher frequency.  Nothing here blocks a frequency: an
 * out-of-plan channel is flagged, not refused (the radio tunes where the
 * operator tells it to, see the notes in docs/ZONES.md).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/datatypes.h"
#include "core/channel_store.h"
#include "core/cps.h"
#include "rtx/rtx.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of a zone channel name, NUL included ("PMR16" + NUL). */
#define ZONE_NAME_LEN 8

/** No zone, where an index or an identifier is expected. */
#define ZONE_NONE 0xFF

/** Identifier of each zone in this build. */
enum zoneId
{
    ZONE_VHF_REPEATERS = 0, /**< R0..R7, 2 m FM repeaters          */
    ZONE_UHF_REPEATERS = 1, /**< U0..U100, 70 cm repeaters         */
    ZONE_PMR446        = 2, /**< PMR1..PMR16, 446 MHz, 500 mW      */
    ZONE_COUNT         = 3
};

/** Flags of a zone or of one of its channels. */
enum zoneFlags
{
    ZONE_FLAG_REPEATER    = 0x01, /**< Repeater pair: TX is shifted       */
    ZONE_FLAG_PMR         = 0x02, /**< PMR446 channel                     */
    ZONE_FLAG_FIXED_POWER = 0x04, /**< Power is set by the plan, not by us */
    ZONE_FLAG_SIMPLEX     = 0x08, /**< TX frequency equals RX             */
    ZONE_FLAG_INTERLEAVED = 0x10, /**< Half channel, outside the 25 kHz plan */
    ZONE_FLAG_RX_ONLY     = 0x20  /**< Receive only                       */
};

/** One channel of a zone, resolved: everything is in Hz, mW and enums. */
typedef struct
{
    uint8_t  zone;                 /**< enum zoneId                        */
    uint16_t index;                /**< 0-based index inside the zone      */
    char     name[ZONE_NAME_LEN];  /**< "R0", "U12", "PMR3", NUL terminated */
    uint32_t rx;                   /**< Frequency the radio listens on, Hz */
    uint32_t tx;                   /**< Frequency it transmits on, Hz      */
    int32_t  shift;                /**< tx - rx, Hz (negative: repeater)   */
    uint8_t  mode;                 /**< enum opmode                        */
    uint8_t  bandwidth;            /**< enum bandwidth                     */
    uint16_t power;                /**< TX power, mW                       */
    uint8_t  flags;                /**< enum zoneFlags                     */
}
zone_channel_t;

/** A zone: the plan a channel is computed from. */
typedef struct
{
    uint8_t     id;           /**< enum zoneId                             */
    const char *prefix;       /**< Name prefix: "R", "U", "PMR"            */
    const char *descr;        /**< One line for the screen                 */
    uint32_t    base;         /**< Frequency of index 0, Hz                */
    int32_t     step;         /**< Frequency step between channels, Hz     */
    int32_t     shift;        /**< Nominal repeater shift, Hz (signed)     */
    uint16_t    count;        /**< Number of channels                      */
    uint8_t     first;        /**< Number printed for index 0 (1 for PMR)  */
    uint8_t     mode;         /**< enum opmode                             */
    uint8_t     bandwidth;    /**< enum bandwidth                          */
    uint16_t    power;        /**< TX power, mW                            */
    uint8_t     flags;        /**< enum zoneFlags shared by its channels   */
    uint8_t     planEvery;    /**< 1: every channel is on the plan,
                                   2: every second one (25 kHz raster)      */
}
zone_t;

/** Number of zones compiled into this build. */
int zones_count(void);

/** @return the zone with that identifier, NULL if there is none. */
const zone_t *zones_get(uint8_t id);

/** @return the i-th zone, NULL out of range. */
const zone_t *zones_at(int i);

/** Number of channels of a zone. */
int zones_channels(const zone_t *z);

/**
 * Resolve one channel of a zone.
 *
 * @param id: zone identifier.
 * @param index: 0-based index inside the zone.
 * @param out: filled with name, RX, TX, shift, mode, bandwidth, power, flags.
 * @return 0 on success, -EINVAL on an unknown zone or an index out of range.
 */
int zones_channel(uint8_t id, uint16_t index, zone_channel_t *out);

/** Name of a zone channel ("U12"), written into buf. */
const char *zones_channelName(uint8_t id, uint16_t index, char *buf,
                              size_t cap);

/**
 * True if the resolved channel can be programmed: its frequencies are inside
 * the coverage of the radio, its power is one the PA can produce.  A PMR
 * channel is valid with its fixed 500 mW and only with it.
 */
bool zones_channelValid(const zone_channel_t *zc);

/**
 * Fill a channel_t from a zone channel, for the VFO path.
 *
 * @return 0 on success, negative on a bad zone or index.
 */
int zones_toChannel(uint8_t id, uint16_t index, channel_t *out);

/**
 * Build the stored record of a zone channel, ready for chstore_add(): the
 * name, the frequencies, the power, the mode and, for the repeater zones, the
 * shift.  The DMR fields are left at their defaults (colour code 1, TS1, no
 * destination), a repeater channel is an FM channel.
 *
 * @return 0 on success, negative on a bad zone or index.
 */
int zones_toStore(uint8_t id, uint16_t index, chstore_channel_t *out);

/**
 * Invert the direction of the repeater shift of every zone.  Off by default,
 * which is the Spanish plan: the radio listens on the repeater output and
 * transmits 600 kHz (VHF) or 7.6 MHz (UHF) below it.  On means the opposite,
 * for a plan read the other way round.
 */
void zones_setShiftInverted(bool inverted);

/** True when the repeater shift is inverted. */
bool zones_shiftInverted(void);

#ifdef __cplusplus
}
#endif

#endif /* ZONES_H */
