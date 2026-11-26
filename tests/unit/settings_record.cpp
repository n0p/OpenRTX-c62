/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The settings record: version, CRC and the promise that a record written by
 * another firmware is discarded as a whole instead of being reinterpreted.
 *
 * The record is what the NVM driver puts on flash (nvmem_C62.c calls
 * settings_recordPack/Read), so this is the migration path of the real radio:
 * an operator upgrading from a build with a 156-byte settings_t must come back
 * to defaults, not to a callsign read out of a squelch level.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "core/settings.h"
#include "core/settings_record.h"
#include "core/crc.h"
}

/*
 * The DMR package appended the transmit air delay, the fourth pass the airband
 * transmit depth, the seventh (settings version 7) the default transmit power
 * and the eighth the accessory port mode.  All of them are the end of the
 * payload, so a record of an earlier pass is the same payload shorter and the
 * CRC catches it: the record is dropped as a whole instead of read with the
 * fields missing.  Version 7 is the exception the migration covers: the payload
 * it wrote *is* a prefix of this one.
 */
#define PRE_DELAY_PAYLOAD_LEN                                                  \
    (sizeof(settings_t) - sizeof(int16_t) - sizeof(uint8_t) -                  \
     sizeof(uint16_t) - sizeof(uint8_t))

/* What version 7 wrote: today's payload without the appended port mode. */
#define V7_PAYLOAD_LEN (sizeof(settings_t) - sizeof(uint8_t))

static const settings_t &dflt()
{
    return default_settings;
}

TEST_CASE("The settings record fits the NVM record budget", "[settings]")
{
    /* nvmem_C62.c refuses anything above 256 bytes: keep 64 of headroom */
    REQUIRE(SETTINGS_RECORD_SIZE <= 192);
    REQUIRE(sizeof(settings_t) < 192);

    /* The record is header plus payload and nothing else: no padding */
    REQUIRE(SETTINGS_RECORD_SIZE == SETTINGS_RECORD_HDR + sizeof(settings_t));
}

TEST_CASE("Settings round-trip through the record", "[settings]")
{
    settings_t s = dflt();

    s.brightness = 42;
    s.sqlLevel = 5;
    snprintf(s.callsign, sizeof(s.callsign), "%s", "EA5ABC");
    s.dmr_id = 2142123;
    s.dmr_dest_type = DMR_DEST_USER;
    s.dmr_dest_id = 1234567;
    s.sms_format = DMR_SMS_FMT_DD_UTF8;
    s.dmr_timeslot = 2;
    s.dmr_tx_air_delay_us = 12500;
    s.m17_can = 3;
    /* Version 4: the receive-only modes' controls, version 5: the scan block */
    s.nxdn_ran = 27;
    s.am_agc = false;
    s.am_step = true;
    s.am_snap = true;

    uint8_t rec[SETTINGS_RECORD_SIZE];
    REQUIRE(settings_recordPack(rec, sizeof(rec), &s)
            == (int)SETTINGS_RECORD_SIZE);

    REQUIRE(settings_recordVersion(rec, sizeof(rec)) == SETTINGS_VERSION);

    settings_t out;
    memset(&out, 0xA5, sizeof(out));
    REQUIRE(settings_recordRead(rec, sizeof(rec), &out) == 0);

    REQUIRE(out.brightness == 42);
    REQUIRE(out.sqlLevel == 5);
    REQUIRE(std::string(out.callsign) == "EA5ABC");
    REQUIRE(out.dmr_id == 2142123);
    REQUIRE(out.dmr_dest_type == DMR_DEST_USER);
    REQUIRE(out.dmr_dest_id == 1234567);
    REQUIRE(out.sms_format == DMR_SMS_FMT_DD_UTF8);
    REQUIRE(out.dmr_timeslot == 2);
    REQUIRE(out.dmr_tx_air_delay_us == 12500);
    REQUIRE(out.m17_can == 3);
    REQUIRE(out.nxdn_ran == 27);
    REQUIRE(out.am_agc == false);
    REQUIRE(out.am_step == true);
    REQUIRE(out.am_snap == true);
    REQUIRE(memcmp(&out, &s, sizeof(s)) == 0);
}

TEST_CASE("The receive-only modes default to the behaviour they had before",
          "[settings]")
{
    /*
     * A settings reset must not change what the modes do: these four defaults
     * are "RAN any, AGC on, 25 kHz, no snap", which is exactly what NXDN and
     * airband AM did while there was no menu to reach them.
     */
    REQUIRE(default_settings.nxdn_ran == 0xFF);
    REQUIRE(default_settings.am_agc == true);
    REQUIRE(default_settings.am_step == false);
    REQUIRE(default_settings.am_snap == false);

    /* And the new transmit depth has a default the modulator can use as is */
    REQUIRE(default_settings.am_depth == 60);
}

TEST_CASE("The DMR air delay, the AM depth and the port mode end the record",
          "[settings]")
{
    /* Version 6 adds the airband transmit depth, appended after the air delay */
    REQUIRE(SETTINGS_VERSION == 8);
    REQUIRE(V7_PAYLOAD_LEN + sizeof(uint8_t) == sizeof(settings_t));
    REQUIRE(sizeof(settings_t) ==
            PRE_DELAY_PAYLOAD_LEN + sizeof(int16_t) + sizeof(uint8_t) +
                sizeof(uint16_t) + sizeof(uint8_t));
    REQUIRE(dflt().am_depth == 60);

    /*
     * A default radio sends the measured delay: the field is 0 in the record,
     * which the mode reads as "use the measured default" (dmr_glue.c).
     */
    REQUIRE(dflt().dmr_tx_air_delay_us == 0);

    /*
     * Version 7 appends the default transmit power after the depth: 500 mW is
     * what the radio keys with when the channel it is on has no power of its
     * own -- the VFO (docs/TX_SAFETY.md).
     */
    REQUIRE(dflt().tx_power == 500);
    REQUIRE(settings_txPowerValid(dflt().tx_power));

    /*
     * Version 8 appends the accessory port mode: Never, the speaker-mic PTT,
     * so a radio that has just been flashed transmits (docs/ACCESSORY.md).
     * Auto is reachable from the CPS and applies at once, but it is not the
     * factory value: it is the mode whose switch detection is still being
     * confirmed on hardware, and a radio handed to an operator with the pad on
     * the UART is a radio whose PTT does nothing.
     */
    REQUIRE(dflt().pc_port == PORT_MODE_NEVER);
    REQUIRE(settings_portModeValid(dflt().pc_port));
    REQUIRE(std::string(settings_portModeName(dflt().pc_port)) == "Never");
}

TEST_CASE("A version 7 record is migrated, not dropped", "[settings]")
{
    /*
     * The upgrade path a radio takes when this firmware lands on one that was
     * running the previous release: version 7 wrote the same payload one byte
     * shorter (settings_t.pc_port did not exist), with a CRC over that length
     * and "7" in the header.  The operator keeps his callsign, his channels'
     * power and his scan settings, and the appended field comes back at its
     * default -- the PTT switch, not the byte that followed it on the flash.
     *
     * Every block the record grew through is given a value of its own here,
     * including the ones added after the version number stopped keeping up
     * (the DMR air delay, the airband depth, the transmit power, the packet
     * modem): the migration is a byte-for-byte copy of the prefix, so the
     * check that it lost nothing is a memcmp and not a list of fields somebody
     * remembered to add.
     */
    settings_t s = dflt();

    s.brightness = 33;
    s.contrast = 7;
    s.sqlLevel = 7;
    s.voxLevel = 3;
    s.utc_timezone = -4;
    s.gps_enabled = true;
    snprintf(s.callsign, sizeof(s.callsign), "%s", "EA5MIG");
    s.display_timer = TIMER_5M;
    s.m17_can = 9;
    s.vpLevel = 2;
    s.vpPhoneticSpell = 1;
    s.macroMenuLatch = 0;
    s.tetraAfcOff = 1;
    s.keyBacklight = 0;
    s.m17_can_rx = true;
    snprintf(s.m17_dest, sizeof(s.m17_dest), "%s", "EA5XYZ");
    s.showBatteryIcon = true;
    s.gpsSetTime = true;
    snprintf(s.M17_meta_text, sizeof(s.M17_meta_text), "%s", "MIGRATED");
    s.tetraAfcOffset = -120;
    s.rxFineHz = 40;
    /* Version 2 and 3: the DMR identity and the slot */
    s.dmr_id = 2142001;
    s.dmr_dest_id = 2142123;
    s.dmr_dest_type = DMR_DEST_USER;
    s.sms_format = DMR_SMS_FMT_DD_UTF8;
    s.dmr_timeslot = 2;
    /* Version 4: the receive-only modes' controls */
    s.nxdn_ran = 27;
    s.am_agc = false;
    s.am_step = true;
    s.am_snap = true;
    /* Version 5: the scan block */
    s.scan_resume = 2;
    s.scan_priority = 5;
    s.scan_step = 3;
    s.scan_dwell = 22;
    s.scan_hold = 33;
    s.scan_prioInterval = 44;
    s.scan_flags = SETTINGS_SCAN_PRIORITY | SETTINGS_SCAN_DOWN;
    s.scan_vfoLow = 430000000U;
    s.scan_vfoHigh = 440000000U;
    /* Appended without renumbering: air delay, airband depth, TX power */
    s.dmr_tx_air_delay_us = 12500;
    s.am_depth = 75;
    s.tx_power = 2500;
#ifdef CONFIG_APRS
    s.aprs_decode = true;
    s.aprs_beaconEn = true;
    s.aprs_msgAck = false;
    s.aprs_ssid = 3;
    s.aprs_lat_e4 = 388700;
    s.aprs_lon_e4 = -12300;
    s.aprs_table = '\\';
    s.aprs_symbol = '>';
    snprintf(s.aprs_comment, sizeof(s.aprs_comment), "%s", "migrated");
    snprintf(s.aprs_path, sizeof(s.aprs_path), "%s", "WIDE2-1");
    s.aprs_interval_s = 900;
    s.aprs_txdelay_ms = 400;
#endif
#ifdef CONFIG_KISS
    s.packet_enable = true;
    s.packet_modem = 1;
    s.packet_port = 0;
    s.packet_persist = 63;
    s.packet_fulldup = 1;
    s.packet_txdelay_ms = 250;
    s.packet_txtail_ms = 30;
    s.packet_slottime_ms = 80;
    s.packet_maxframe = 256;
#endif
    s.pc_port = PORT_MODE_ALWAYS; /* not part of a v7 record: must not survive */

    uint8_t rec[SETTINGS_RECORD_SIZE];
    std::memset(rec, 0xFF, sizeof(rec));
    rec[0] = 0x58; rec[1] = 0x54; rec[2] = 0x52; rec[3] = 0x4F; /* "ORTX" */
    rec[4] = (uint8_t)(crc_ccitt(&s, V7_PAYLOAD_LEN) & 0xFF);
    rec[5] = (uint8_t)(crc_ccitt(&s, V7_PAYLOAD_LEN) >> 8);
    rec[6] = 7; rec[7] = 0;
    std::memcpy(&rec[SETTINGS_RECORD_HDR], &s, V7_PAYLOAD_LEN);

    settings_t out;
    std::memset(&out, 0x5A, sizeof(out));
    uint8_t cause = SETTINGS_RESET_VERSION;

    REQUIRE(settings_recordVersion(rec, sizeof(rec)) == 7);
    REQUIRE(settings_recordLoad(rec, sizeof(rec), &out, &cause) == 0);
    /* Migrated, not reset: the screen must not tell the operator he lost
     * everything. */
    REQUIRE(cause == SETTINGS_NO_RESET);

    REQUIRE(out.brightness == 33);
    REQUIRE(out.sqlLevel == 7);
    REQUIRE(std::string(out.callsign) == "EA5MIG");
    REQUIRE(out.tx_power == 2500);
    REQUIRE(out.scan_dwell == 22);
    REQUIRE(out.dmr_id == 2142001);

    /* Nothing the v7 record carried was lost, byte for byte, including the
     * blocks appended after the version number last moved. */
    REQUIRE(std::memcmp(&out, &s, V7_PAYLOAD_LEN) == 0);

    /* The appended field is the default, never the byte that followed it on
     * flash.  A version 7 record says nothing about the pad, so it takes the
     * factory value: the PTT switch (docs/ACCESSORY.md). */
    REQUIRE(out.pc_port == PORT_MODE_NEVER);
    REQUIRE(out.pc_port == default_settings.pc_port);
    REQUIRE(settings_portModeValid(out.pc_port));

    /*
     * And the record the next write produces is a version 8 one carrying the
     * same data, so the upgrade is finished on the first save and not redone
     * on every boot.
     */
    uint8_t fresh[SETTINGS_RECORD_SIZE];
    REQUIRE(settings_recordPack(fresh, sizeof(fresh), &out)
            == (int)SETTINGS_RECORD_SIZE);
    REQUIRE(settings_recordVersion(fresh, sizeof(fresh)) == SETTINGS_VERSION);

    settings_t again;
    std::memset(&again, 0x00, sizeof(again));
    REQUIRE(settings_recordRead(fresh, sizeof(fresh), &again) == 0);
    REQUIRE(std::memcmp(&again, &out, sizeof(out)) == 0);

    /* A v7 record whose CRC does not cover its own length is still corrupt: the
     * migration must not read a truncated or damaged payload. */
    uint8_t bad[SETTINGS_RECORD_SIZE];
    std::memcpy(bad, rec, sizeof(bad));
    bad[SETTINGS_RECORD_HDR + 2] ^= 0x01;
    std::memset(&out, 0x5A, sizeof(out));
    REQUIRE(settings_recordRead(bad, sizeof(bad), &out) ==
            SETTINGS_RECORD_ERR_CORRUPT);
}

TEST_CASE("A record of an older version is discarded with a reason", "[settings]")
{
    settings_t s = dflt();
    uint8_t rec[SETTINGS_RECORD_SIZE];

    /*
     * A payload four bytes shorter than today's: the first firmware's
     * settings_t, whatever its exact size was.
     */
    const size_t LEGACY_LEN = sizeof(settings_t) - 4;

    REQUIRE(settings_recordPack(rec, sizeof(rec), &s) > 0);

    /*
     * The header of the first firmware: magic, payload CRC over a smaller
     * settings_t and no version (the field was padding, zero).
     */
    uint8_t old[SETTINGS_RECORD_SIZE];
    memset(old, 0xFF, sizeof(old));
    old[0] = 0x58; old[1] = 0x54; old[2] = 0x52; old[3] = 0x4F;  /* "ORTX" */
    old[4] = (uint8_t)(crc_ccitt(&s, LEGACY_LEN) & 0xFF);
    old[5] = (uint8_t)(crc_ccitt(&s, LEGACY_LEN) >> 8);
    old[6] = 0;
    old[7] = 0;                       /* version 0: before versioning */
    memcpy(&old[SETTINGS_RECORD_HDR], &s, LEGACY_LEN);

    settings_t out;
    memset(&out, 0x5A, sizeof(out));

    uint8_t cause = SETTINGS_NO_RESET;
    REQUIRE(settings_recordLoad(old, sizeof(old), &out, &cause) ==
            SETTINGS_RECORD_ERR_VERSION);
    REQUIRE(cause == SETTINGS_RESET_VERSION);

    /* Nothing was written to the destination on a rejected record */
    settings_t untouched;
    memset(&untouched, 0x5A, sizeof(untouched));
    REQUIRE(memcmp(&out, &untouched, sizeof(out)) == 0);

    /* Same for a record from a newer firmware */
    rec[6] = (uint8_t)(SETTINGS_VERSION + 1);
    REQUIRE(settings_recordRead(rec, sizeof(rec), &out) ==
            SETTINGS_RECORD_ERR_VERSION);
}

TEST_CASE("A record from before the air delay is refused, not half-read",
          "[settings]")
{
    settings_t s = dflt();
    uint8_t old[SETTINGS_RECORD_SIZE];

    /*
     * The record the previous pass wrote: the current version, a valid magic
     * and a CRC over a payload without the DMR air delay.  The version matches,
     * so it is the CRC length that refuses it; the radio keeps its defaults and
     * the operator loses the settings once, which is the documented cost of
     * the append.
     */
    memset(old, 0xFF, sizeof(old));
    old[0] = 0x58; old[1] = 0x54; old[2] = 0x52; old[3] = 0x4F;  /* "ORTX" */
    old[4] = (uint8_t)(crc_ccitt(&s, PRE_DELAY_PAYLOAD_LEN) & 0xFF);
    old[5] = (uint8_t)(crc_ccitt(&s, PRE_DELAY_PAYLOAD_LEN) >> 8);
    old[6] = (uint8_t)SETTINGS_VERSION; old[7] = 0;  /* current version */
    memcpy(&old[SETTINGS_RECORD_HDR], &s, PRE_DELAY_PAYLOAD_LEN);

    settings_t out;
    uint8_t cause = SETTINGS_NO_RESET;
    memset(&out, 0x5A, sizeof(out));

    REQUIRE(settings_recordLoad(old, sizeof(old), &out, &cause) ==
            SETTINGS_RECORD_ERR_CORRUPT);
    REQUIRE(cause == SETTINGS_RESET_CORRUPT);

    /* Nothing was written to the destination */
    settings_t untouched;
    memset(&untouched, 0x5A, sizeof(untouched));
    REQUIRE(memcmp(&out, &untouched, sizeof(out)) == 0);

    /* The reset state is what the upgrade leaves: measured delay, no trim */
    REQUIRE(dflt().dmr_tx_air_delay_us == 0);
}

TEST_CASE("A damaged or blank record is reported, not loaded", "[settings]")
{
    settings_t s = dflt();
    uint8_t rec[SETTINGS_RECORD_SIZE];
    settings_t out;
    uint8_t cause = SETTINGS_NO_RESET;

    REQUIRE(settings_recordPack(rec, sizeof(rec), &s) > 0);

    /* One flipped bit in the payload: CRC catches it */
    uint8_t bad[SETTINGS_RECORD_SIZE];
    memcpy(bad, rec, sizeof(bad));
    bad[SETTINGS_RECORD_HDR + 3] ^= 0x01;
    REQUIRE(settings_recordRead(bad, sizeof(bad), &out) ==
            SETTINGS_RECORD_ERR_CORRUPT);
    REQUIRE(settings_recordLoad(bad, sizeof(bad), &out, &cause) ==
            SETTINGS_RECORD_ERR_CORRUPT);
    REQUIRE(cause == SETTINGS_RESET_CORRUPT);

    /* An erased sector reads all 0xFF: that is "first power on", not damage */
    uint8_t blank[SETTINGS_RECORD_SIZE];
    memset(blank, 0xFF, sizeof(blank));
    REQUIRE(settings_recordLoad(blank, sizeof(blank), &out, &cause) != 0);
    REQUIRE(cause == SETTINGS_RESET_BLANK);

    /* A record that is too short is refused before anything is read */
    REQUIRE(settings_recordRead(rec, SETTINGS_RECORD_HDR - 1, &out) ==
            SETTINGS_RECORD_ERR_CORRUPT);
    REQUIRE(settings_recordPack(nullptr, sizeof(rec), &s) < 0);
    REQUIRE(settings_recordPack(rec, SETTINGS_RECORD_HDR, &s) < 0);
    REQUIRE(settings_recordRead(nullptr, sizeof(rec), &out) ==
            SETTINGS_RECORD_ERR_CORRUPT);
}

TEST_CASE("DMR identifiers and SMS formats are validated", "[settings]")
{
    /* 24 bits, 1..16777215 */
    REQUIRE(settings_dmrIdValid(1));
    REQUIRE(settings_dmrIdValid(2142123));
    REQUIRE(settings_dmrIdValid(0xFFFFFF));
    REQUIRE_FALSE(settings_dmrIdValid(0));
    REQUIRE_FALSE(settings_dmrIdValid(0x1000000));

    REQUIRE(settings_dmrDestValid(DMR_DEST_NONE, 0));
    REQUIRE(settings_dmrDestValid(DMR_DEST_GROUP, 9));
    REQUIRE(settings_dmrDestValid(DMR_DEST_USER, 1234567));
    REQUIRE_FALSE(settings_dmrDestValid(DMR_DEST_GROUP, 0));
    REQUIRE_FALSE(settings_dmrDestValid(3, 9));

    for (uint8_t f = 0; f < DMR_SMS_FMT_COUNT; f++)
        REQUIRE(settings_smsFormatValid(f));
    REQUIRE_FALSE(settings_smsFormatValid(DMR_SMS_FMT_COUNT));

    REQUIRE(std::string(settings_dmrDestName(DMR_DEST_GROUP)) == "group");
    REQUIRE(std::string(settings_dmrDestName(DMR_DEST_USER)) == "user");
    REQUIRE(std::string(settings_dmrDestName(DMR_DEST_NONE)) == "none");
    REQUIRE(std::string(settings_smsFormatName(DMR_SMS_FMT_AUTO)) == "auto");
    REQUIRE(std::string(settings_smsFormatName(DMR_SMS_FMT_TMS)) == "TMS");

    /* The timeslot is 1 or 2, and the menu names it the way radios do */
    REQUIRE(settings_dmrTimeslotValid(1));
    REQUIRE(settings_dmrTimeslotValid(2));
    REQUIRE_FALSE(settings_dmrTimeslotValid(0));
    REQUIRE_FALSE(settings_dmrTimeslotValid(3));
    REQUIRE(std::string(settings_dmrTimeslotName(1)) == "TS1");
    REQUIRE(std::string(settings_dmrTimeslotName(2)) == "TS2");

    /* The defaults are themselves valid: a reset leaves a usable radio */
    REQUIRE(settings_dmrDestValid(dflt().dmr_dest_type, dflt().dmr_dest_id));
    REQUIRE(settings_smsFormatValid(dflt().sms_format));
    REQUIRE(settings_dmrTimeslotValid(dflt().dmr_timeslot));
    REQUIRE(dflt().dmr_timeslot == 1);
    REQUIRE(dflt().dmr_id == 0);  /* no ID until the operator sets one */
}
