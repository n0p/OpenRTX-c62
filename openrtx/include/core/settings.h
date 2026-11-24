/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "hwconfig.h"
#include <stdbool.h>

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
            _reserved       : 2;
    bool    m17_can_rx;           // Check M17 CAN on RX
    char    m17_dest[10];         // M17 destination
    bool    showBatteryIcon;      // Battery display true: icon, false: percentage
    bool    gpsSetTime;           // Use GPS to ajust RTC time
    char    M17_meta_text[53];    // M17 Meta Text to send
    int16_t tetraAfcOffset;       // TETRA frequency correction learned, Hz
#ifdef CONFIG_APRS
    /*
     * APRS, on the analog FM path. Everything here is off by default: the
     * decoder is opt-in because it costs CPU while the squelch is open, and
     * the beacon is opt-in because transmitting without being asked is how a
     * station gets a reputation.
     *
     * The block is compiled in only when APRS is: other targets keep their
     * settings_t layout, and with it their NVM record size, unchanged.
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
    4,                            // Squelch level, 4 = S3
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
    0,                            // not used
    false,                        // Check M17 CAN on RX
    "",                           // Empty M17 destination
    false,                        // Display battery icon
    false,                        // Update RTC with GPS
    "OpenRTX",                    // Default M17 meta text
    0,                            // No TETRA frequency correction learned
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
};

#endif /* SETTINGS_H */
