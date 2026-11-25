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

#ifdef __cplusplus
extern "C" {
#endif

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
 *  - 4: the receive-only modes' own controls appended (NXDN RAN filter, airband
 *       AGC, raster and snap).
 *  - 5: scan configuration appended (resume rule, dwell, hold, priority
 *       channel, VFO range and step), see core/scanner.h and docs/SCAN.md.
 *  - 5: the DMR transmit air delay appended after the scan block when the DMR
 *       package landed (docs/DMR_PKG.md).  Same version on purpose: the whole
 *       tree is one development line and no field is ever reinterpreted, but
 *       the record grew, so a record of the previous pass fails its CRC and is
 *       dropped as a whole instead of loaded with the delay missing.
 *       The blocks are appends at different offsets, so merging branches is a
 *       renumber and never a re-interpretation: nothing here reorders or reuses
 *       a field.
 *  - 6: the packet modem block appended (enable, modem, KISS parameters), see
 *       docs/KISS.md and core/packet_service.h.  c62/mega stopped at 5, so 6
 *       is the next free number and no branch has to be renumbered.
 *  - 7: the default transmit power appended (settings_t.tx_power, mW), see
 *       docs/TX_SAFETY.md.  It is what the radio transmits with when the
 *       channel it is on does not carry a power of its own -- the VFO, and a
 *       bank record written before the channel editor knew the field.  Before
 *       this the VFO keyed at 0 mW: the PA enable lines went up and no RF
 *       came out, with nothing on the screen or in the log to say why.
 */
#define SETTINGS_VERSION 7

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
    /*
     * The receive-only modes' own controls, one block so that the whole of it
     * is 4 bytes. They exist because the modes are not just "a frequency": the
     * NXDN receiver has a RAN filter and the AM one has a level control and a
     * channel raster, and none of those fit an FM page.
     *
     * Everything here has a default that reproduces the behaviour the modes had
     * before the menu could reach them (RAN any, AGC on, 25 kHz, no snap), so a
     * settings reset costs the operator nothing but the values he chose.
     */
    uint8_t  nxdn_ran;            // RAN the NXDN receiver accepts, 0xFF = any
    bool     am_agc;              // Audio AGC of the AM receive path
    bool     am_step;             // Airband raster: false 25 kHz, true 8.33 kHz
    bool     am_snap;             // Snap tuning to the 8.33 kHz raster
    /*
     * Scan and vigilance (core/scanner.h, docs/SCAN.md).
     *
     * The units are chosen to keep the whole block in 15 bytes: the record has
     * a 256 B ceiling and every future field competes for it.  The times a
     * menu shows as seconds are stored coarse on purpose (a scan does not care
     * about 10 ms) and scan_service.c expands them.
     */
    uint8_t  scan_resume;         // enum scanResume: carrier, timeout, hold
    uint8_t  scan_priority;       // Priority channel index + 1, 0 = none
    uint8_t  scan_step;           // Index into scan_steps[], 0..5
    uint8_t  scan_dwell;          // Dwell per channel, units of 10 ms
    uint8_t  scan_hold;           // Hang / hold time, units of 100 ms
    uint8_t  scan_prioInterval;   // Between priority checks, 100 ms units
    uint8_t  scan_flags;          // enum settingsScanFlags
    uint32_t scan_vfoLow;         // VFO scan start / limit, Hz
    uint32_t scan_vfoHigh;        // VFO scan limit, Hz
    /**
     * Transmit air delay of the audio path, microseconds: from writing a slot
     * into the baseband stream to it leaving the PA.  0 keeps the measured
     * default of the mode (protocols/DMR/dmr_glue.c); anything else is a
     * bench calibration against a repeater.  Positive moves the burst later.
     */
    int16_t  dmr_tx_air_delay_us;
    /*
     * Modulation depth of the airband transmitter, percent. 30 to 90: below
     * that an amplitude modulated carrier is hard to hear over the noise of a
     * receiver with its AGC on, and above it the troughs of the envelope come
     * too close to zero for any PA to follow without distortion. The modulator
     * caps it further if the calibrated window of the PA cannot take it, and
     * am_tx_getDepth() is what the menu shows.
     *
     * Fourth pass, appended behind everything else: the AM block above could
     * not take it without moving the scan fields of an older record.
     */
    uint8_t  am_depth;            // Airband TX modulation depth, percent
    /**
     * Transmit power used when the channel does not name one, mW.
     *
     * A channel carries its own power, but the VFO is not a channel: it has a
     * frequency and a mode and nothing else.  Before this field the VFO
     * inherited state.channel.power, which a blank record leaves at 0, and the
     * radio keyed the PA with a power of zero -- the enable lines came up, the
     * LED did not, no RF left the antenna and no message said why
     * (docs/TX_SAFETY.md).  This is the value the operator gets instead.
     *
     * Range 0..5000, the PA's own ceiling.  0 is accepted and stored (an
     * operator may want a listen-only radio) but it never keys: the keying
     * gate refuses and the reason is visible in the status, because "the
     * button does nothing" is the one failure mode that must never be silent.
     */
    uint16_t tx_power;            // Default TX power, mW, 0 = never key
#ifdef CONFIG_KISS
    /*
     * Packet modem (KISS TNC), docs/KISS.md.
     *
     * The four KISS parameters the host can also set at run time
     * (TXDELAY, PERSIST, SLOTTIME, TXTAIL) live here so that the radio keeps
     * them across a power cycle; a value the host writes overrides the stored
     * one for the session and is not saved, which is what a TNC2 does.
     */
    bool     packet_enable;       // KISS service running
    uint8_t  packet_modem;        // pkt_modem_t: AFSK1200, AFSK9600, NXDN, DMRIP
    uint8_t  packet_port;         // KISS port of the radio (0)
    uint8_t  packet_persist;      // 0..255, channel access probability
    uint8_t  packet_fulldup;      // 1: transmit without carrier sense
    uint16_t packet_txdelay_ms;   // TXDELAY: key-up flags before the frame
    uint16_t packet_txtail_ms;    // TXTAIL: silence after the last flag
    uint16_t packet_slottime_ms;  // SLOTTIME: channel access slot
    uint16_t packet_maxframe;     // Longest frame accepted from the host
#endif
}
__attribute__((packed)) settings_t;

/**
 * Flags of settings_t.scan_flags.
 *
 * The mirror of scanConfig_t in core/scanner.h, kept as bits because the
 * settings record is the scarcest space in the firmware: scan_service.c is the
 * one place that translates them, so nothing else has to know both spellings.
 */
enum settingsScanFlags
{
    SETTINGS_SCAN_VFO_LOOP = 0x01, /**< VFO scan wraps at its limits        */
    SETTINGS_SCAN_PRIORITY = 0x02, /**< Check a priority channel           */
    SETTINGS_SCAN_DOWN     = 0x04  /**< VFO scan goes down, not up         */
};

/** Number of entries of the scan step table (scan_service.c). */
#define SETTINGS_SCAN_STEPS 6

/**
 * Scan step table, Hz: 5, 6.25, 8.33, 10, 12.5 and 25 kHz.  The index lives
 * in settings_t.scan_step, so the numeric values are stored in flash: only
 * append.
 *
 * 8.33 kHz is 25000/3 Hz and cannot be written as an integer frequency, so the
 * table carries 8330 Hz: the error is 0.04 % of the step and 4 Hz on a channel
 * that the radio's own reference is not that accurate on either.
 */
extern const uint32_t scan_steps[SETTINGS_SCAN_STEPS];

/** "5", "6.25", "8.33", "10", "12.5", "25": short name of a step, kHz. */
const char *settings_scanStepName(uint8_t idx);

/** True if idx names a step of the table. */
static inline bool settings_scanStepValid(uint8_t idx)
{
    return idx < SETTINGS_SCAN_STEPS;
}


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
    0xFF,                         // NXDN: any RAN, the pre-menu behaviour
    true,                         // AM: audio AGC on
    false,                        // AM: 25 kHz raster
    false,                        // AM: do not snap (25 kHz needs no snapping)
    0,                            // Scan resumes when the carrier drops
    0,                            // No priority channel
    4,                            // Scan step index 4: 12.5 kHz
    15,                           // Scan dwell 150 ms per channel
    20,                           // Scan hang / hold 2 s
    20,                           // Priority channel every 2 s
    SETTINGS_SCAN_VFO_LOOP,       // VFO scan wraps, up, no priority
    144000000U,                   // VFO scan: 2 m band
    146000000U,
    0,                            // Air delay 0: the measured default
    60,                           // AM: 60 % modulation depth on transmit
    500,                          // Default TX power 500 mW (docs/TX_SAFETY.md)
#ifdef CONFIG_KISS
    false,                        // Packet modem off until the operator asks
    0,                            // PKT_MODEM_AFSK1200: what an FM channel carries
    0,                            // KISS port 0
    128,                          // PERSIST 128: listen before transmit
    false,                        // Half duplex: carrier sense on
    300,                          // 300 ms TXDELAY, as APRS recommends
    20,                           // 20 ms TXTAIL
    100,                          // 100 ms SLOTTIME
    330,                          // The AX.25 ceiling, FCS included
#endif
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
 * Ceiling of the transmit power setting, milliwatts: the PA's own ceiling, the
 * same one chstore_valid() applies to a channel.  A value above it is clamped
 * by the settings loader and refused by the protocol, never transmitted.
 */
#define SETTINGS_TX_POWER_MAX 5000u

static inline bool settings_txPowerValid(uint16_t mw)
{
    return mw <= SETTINGS_TX_POWER_MAX;
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

/** True if r names one of the resume rules of core/scanner.h. */
static inline bool settings_scanResumeValid(uint8_t r)
{
    return r <= 2; /* carrier, timeout, hold */
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SETTINGS_H */
