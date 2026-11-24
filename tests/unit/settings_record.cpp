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
    s.m17_can = 3;
    /* Version 4: the receive-only modes' controls */
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
