/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "hwconfig.h"
#include "core/datatypes.h"
#include <stdbool.h>

/**
 * Version of the settings payload, kept in the record header by the NVM
 * driver (see core/settings_record.h).
 *
 * This header changes often: every field added or moved bumps the version and
 * an old record is then discarded as a whole, with the defaults coming back,
 * instead of being reinterpreted with fields landing at the wrong offset.  The
 * radio tells the operator through state.settings_reset (docs/SETTINGS.md).
 *
*  - 1: the original record (no version in the header, 156 B with APRS).
 *  - 2: DMR destination, own DMR ID and SMS format appended.
 *  - 3: DMR timeslot appended (1 or 2), so the quick menu can move the call
 *       between the two slots of the 60 ms frame.
 *  - 4: DMR transmit air delay (microseconds) appended: the calibrated delay
 *       of the audio path, see docs/DMR_PKG.md.
 */
#define SETTINGS_VERSION 4

typedef enum
{
    TIMER_OFF =  0,
    TIMER_5S  =  1,
    TIMER_10S =  2,
    TIMER_15S =  3,
    TIMER_20S =  4,
    TIMER_25S =  5,
    TIMER_30S =  6,
    TIMER_1M  =  7,
    TIMER_2M  =  8,
    TIMER_3M  =  9,
    TIMER_4M  = 10,
    TIMER_5M  = 11,
    TIMER_15M = 12,
    TIMER_30M = 13,
    TIMER_45M = 14,
    TIMER_1H  = 15
}
display_timer_t;

typedef struct
{
    uint8_t brightness;           // Display brightness
    uint8_t contrast;             // Display contrast
    uint8_t sqlLevel;             // Squelch level
    uint8_t voxLevel;             // Vox level
    int8_t  utc_timezone;         // Timezone, in units of half hours
    bool    gps_enabled;          // GPS active
    char    callsign[10];         // Plaintext callsign
    uint8_t display_timer   : 4,  // Standby timer
            m17_can         : 4;  // M17 CAN
    uint8_t vpLevel         : 3,  // Voice prompt level
            vpPhoneticSpell : 1,  // Phonetic spell enabled
            macroMenuLatch  : 1,  // Automatic latch of macro menu
            tetraAfcOff     : 1,  // TETRA frequency correction off
            keyBacklight    : 1,  // Keypad backlight on
            _reserved       : 1;
    bool    m17_can_rx;           // Check M17 CAN on RX
    char    m17_dest[10];         // M17 destination
    bool    showBatteryIcon;      // Battery display true: icon, false: percentage
    bool    gpsSetTime;           // Use GPS to ajust RTC time
    char    M17_meta_text[53];    // M17 Meta Text to send
    int16_t tetraAfcOffset;       // TETRA frequency correction learned, Hz
    int16_t rxFineHz;             // Manual fine RX tuning, Hz (10 Hz steps)
#ifdef CONFIG_APRS
    /*
     * APRS, on the analog FM path. Everything here is off by default: the
     * decoder is opt-in because it costs CPU while the squelch is open, and
     * the beacon is opt-in because transmitting without being asked is how a
     * station gets a reputation.
     *
     * The block is compiled in only when APRS is: other targets keep their
     * settings_t layout, and with it their NVM record size, unchanged.
     *
     * The station identity is the callsign above plus aprs_ssid, so the
     * operator types it once; the path, symbol and comment are separate
     * because APRS uses them per packet.
     */
    bool    aprs_decode;          // Decode APRS on the FM receive path
    bool    aprs_beaconEn;        // Beacon transmission enabled
    bool    aprs_msgAck;          // Acknowledge received messages
    uint8_t aprs_ssid;            // SSID appended to the callsign, 0..15
    int32_t aprs_lat_e4;          // Latitude, degrees * 10000
    int32_t aprs_lon_e4;          // Longitude, degrees * 10000
    char    aprs_table;           // APRS symbol table ('/' or '\\')
    char    aprs_symbol;          // APRS symbol code
    char    aprs_comment[26];     // Status text / beacon comment
    char    aprs_path[24];        // Digipeater path, e.g. "WIDE1-1,WIDE2-1"
    uint16_t aprs_interval_s;     // Beacon interval, seconds (>= 30)
    uint16_t aprs_txdelay_ms;     // TXDELAY, ms (300 is the APRS default)
#endif
    /*
     * DMR identity and call destination.
     *
     * dmr_id is the radio's own subscriber ID: it goes in the source field of
     * every link control, data header and SMS header this radio transmits
     * (TS 102 361-1 clause 9.3.1, 24 bits), so a wrong value means nobody can
     * reply.  dmr_dest_type/dmr_dest_id are the default destination of a call
     * (a talkgroup or a single subscriber), which the per-channel destination
     * of the channel store can override.
     */
    uint32_t dmr_id;              // Own DMR ID, 1..16777215
    uint32_t dmr_dest_id;         // Default call destination, 1..16777215
    uint8_t  dmr_dest_type;       // enum dmrDestType
    uint8_t  sms_format;          // enum dmrSmsFormat
    uint8_t  dmr_timeslot;        // 1 or 2: the slot of the 60 ms TDMA frame
    /**
     * Transmit air delay of the audio path, microseconds: from writing a slot
     * into the baseband stream to it leaving the PA.  0 keeps the measured
     * default of the mode (protocols/DMR/dmr_glue.c); anything else is a
     * bench calibration against a repeater.  Positive moves the burst later.
     */
    int16_t  dmr_tx_air_delay_us;
}
__attribute__((packed)) settings_t;


static const settings_t default_settings =
{
    100,                          // Brightness
#ifdef CONFIG_SCREEN_CONTRAST
    CONFIG_DEFAULT_CONTRAST,      // Contrast
#else
    255,                          // Contrast
#endif
    3,                            // Squelch level, 3 = S3
    0,                            // Vox level
    0,                            // UTC Timezone
    false,                        // GPS enabled
    "N0CALL",                     // Default callsign
    TIMER_30S,                    // 30 seconds
    0,                            // M17 CAN
    0,                            // Voice prompts off
    0,                            // Phonetic spell off
    1,                            // Automatic latch of macro menu enabled
    0,                            // TETRA frequency correction on
    1,                            // Keypad backlight on
    0,                            // not used
    false,                        // Check M17 CAN on RX
    "",                           // Empty M17 destination
    false,                        // Display battery icon
    false,                        // Update RTC with GPS
    "OpenRTX",                    // Default M17 meta text
    0,                            // No TETRA frequency correction learned
    0,                            // No manual fine RX tuning
#ifdef CONFIG_APRS
    false,                        // APRS decoding off
    false,                        // APRS beacon off
    true,                         // Acknowledge messages when decoding
    9,                            // APRS SSID -9, the portable/temporary one
    0,                            // No position until the user enters one
    0,
    '/',                          // Primary symbol table
    '-',                          // Symbol code: house / fixed station
    "OpenRTX C62",                // Beacon comment
    "WIDE1-1,WIDE2-1",            // Default APRS digipeater path
    600,                          // Beacon every 10 minutes
    300,                          // 300 ms TXDELAY, APRS101 recommendation
#endif
    0,                            // No DMR ID yet: the operator sets it
    9,                            // Default talkgroup, as the RTX starts with
    DMR_DEST_GROUP,               // Group call by default
    DMR_SMS_FMT_AUTO,             // The format this firmware always sent
    1,                            // Timeslot 1, the one every network uses
    0,                            // Air delay 0: the measured default
};

/**
 * True if id is a usable DMR identifier: 24 bits, 1 to 16777215.  0 is
 * reserved for "no address" and is rejected on purpose, a call to ID 0 cannot
 * be answered.
 */
static inline bool settings_dmrIdValid(uint32_t id)
{
    return (id >= DMR_ID_MIN) && (id <= DMR_ID_MAX);
}

/**
 * True if the pair describes a destination a DMR call can use: a known type
 * and, unless the type is NONE, a valid identifier.
 */
static inline bool settings_dmrDestValid(uint8_t type, uint32_t id)
{
    if (type == DMR_DEST_NONE)
        return true;

    if ((type != DMR_DEST_GROUP) && (type != DMR_DEST_USER))
        return false;

    return settings_dmrIdValid(id);
}

/**
 * True if slot is a DMR timeslot: 1 or 2.  Anything else would make the
 * transmitter key a slot the frame does not have.
 */
static inline bool settings_dmrTimeslotValid(uint8_t slot)
{
    return (slot == 1) || (slot == 2);
}

/** True if f names one of the SMS payload formats. */
static inline bool settings_smsFormatValid(uint8_t f)
{
    return f < (uint8_t)DMR_SMS_FMT_COUNT;
}

/** "none", "group", "user": short name of a destination type, for menus. */
const char *settings_dmrDestName(uint8_t type);

/** "auto", "DD UTF-16BE", ...: short name of an SMS format, for menus. */
const char *settings_smsFormatName(uint8_t format);

/** "TS1" or "TS2": short name of a timeslot, for menus. */
const char *settings_dmrTimeslotName(uint8_t timeslot);

/** Value of the settings reset cause, as reported by state.settings_reset. */
enum settingsReset {
    SETTINGS_NO_RESET = 0,   /**< Loaded from flash, nothing lost          */
    SETTINGS_RESET_BLANK,    /**< Nothing valid in flash: first power on    */
    SETTINGS_RESET_VERSION,  /**< Written by an older firmware: discarded   */
    SETTINGS_RESET_CORRUPT,  /**< Bad CRC: discarded                        */
};

#endif /* SETTINGS_H */
