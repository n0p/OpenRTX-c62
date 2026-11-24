/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The SMS payload format is a setting: the operator picks the flavour that the
 * other radios of his network read, and the radio has to build exactly that.
 *
 * Our record has eight flavours (enum dmrSmsFmtSetting, kept for the settings
 * written by the older firmware); the package sends three payload layouts
 * (dmr_sms.h): TMS, ETSI IP text and ETSI defined short data.  The setting is
 * mapped in dmr_glue.c and the bursts built for each layout are inspected
 * here: data header, SAP and UDP port, and the text in the encoding the
 * format names.  That is what leaves the radio.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "core/datatypes.h"
#include "protocols/DMR/dmr_glue.h"
#include "protocols/DMR/dmr_sms.h"
}

/* Slot type data types of the bursts (ETSI TS 102 361-1 table 6.6) */
#define DT_CSBK 3
#define DT_DATA_HEADER 6
#define DT_RATE_12 7

#define PREAMBLES 16
#define MAX_BURSTS 48

struct Bursts {
    uint8_t dt[MAX_BURSTS];
    uint8_t b[MAX_BURSTS][12];
    int n;

    /* The user octets of the packet: the rate 1/2 blocks in order */
    std::string payload() const
    {
        std::string s;

        for (int i = 0; i < n; i++)
            if (dt[i] == DT_RATE_12)
                s.append((const char *)b[i], 12);
        return s;
    }
};

static Bursts build(uint8_t format, const char *text, bool group = true,
                    uint32_t dst = 2142, uint32_t src = 2142123)
{
    Bursts out;

    memset(&out, 0, sizeof(out));
    out.n = dmrSms_build(dst, group, format, text, src, out.dt, out.b,
                         MAX_BURSTS);
    return out;
}

/* Big-endian 16-bit field at an offset */
static uint16_t u16(const std::string &s, size_t off)
{
    return (uint16_t)(((uint8_t)s[off] << 8) | (uint8_t)s[off + 1]);
}

TEST_CASE("Every setting maps to a payload layout the package can send",
          "[dmr-sms]")
{
    /* AUTO is what this firmware has always sent: ETSI IP text (5016) */
    REQUIRE(dmrGlue_smsFormat(DMR_SMS_FMT_AUTO) == DMR_SMS_ETSI_IP);
    REQUIRE(dmrGlue_smsFormat(DMR_SMS_FMT_IP_5016) == DMR_SMS_ETSI_IP);
    REQUIRE(dmrGlue_smsFormat(DMR_SMS_FMT_IP_HC) == DMR_SMS_ETSI_IP);
    REQUIRE(dmrGlue_smsFormat(DMR_SMS_FMT_ANYTONE) == DMR_SMS_ETSI_IP);
    REQUIRE(dmrGlue_smsFormat(DMR_SMS_FMT_TMS) == DMR_SMS_TMS);
    REQUIRE(dmrGlue_smsFormat(DMR_SMS_FMT_DD_UTF16BE) == DMR_SMS_ETSI_DD);
    REQUIRE(dmrGlue_smsFormat(DMR_SMS_FMT_DD_UTF8) == DMR_SMS_ETSI_DD);
    REQUIRE(dmrGlue_smsFormat(DMR_SMS_FMT_HYTERA) == DMR_SMS_ETSI_DD);

    /* No setting, and no value from an older or newer record, is unusable */
    for (uint8_t f = 0; f < DMR_SMS_FMT_COUNT; f++)
        REQUIRE(dmrGlue_smsFormat(f) < DMR_SMS_FORMATS);
    REQUIRE(dmrGlue_smsFormat(200) < DMR_SMS_FORMATS);
}

TEST_CASE("A TMS message leaves as UDP 4007 with the TMS header", "[dmr-sms]")
{
    Bursts out = build(DMR_SMS_TMS, "Hola 123");
    REQUIRE(out.n > PREAMBLES + 1);

    /* Preambles, a data header, then the packet in rate 1/2 blocks */
    for (int i = 0; i < PREAMBLES; i++)
        REQUIRE(out.dt[i] == DT_CSBK);
    REQUIRE(out.dt[PREAMBLES] == DT_DATA_HEADER);

    /* Unconfirmed packet (DPF 2), SAP 4 = IP */
    REQUIRE((out.b[PREAMBLES][0] & 0x3Fu) == 0x02);
    REQUIRE((out.b[PREAMBLES][1] >> 4) == 0x04);

    /* The IPv4/UDP destination and source ports */
    std::string p = out.payload();
    REQUIRE(p.size() >= 34);
    REQUIRE(u16(p, 20) == 4007);
    REQUIRE(u16(p, 22) == 4007);

    /*
     * TMS simple text: length, header 0xA0 (0xE0 when an individual peer has
     * to acknowledge), sequence, encoding 4, the text in UTF-16LE.
     */
    REQUIRE(((uint8_t)p[30] & 0xE0u) == 0xA0u);
    REQUIRE((uint8_t)p[31] == 0x00);
    REQUIRE((uint8_t)p[33] == 0x04);

    const char utf16le[] = { 'H', 0, 'o', 0, 'l', 0, 'a', 0, ' ', 0 };
    REQUIRE(p.compare(34, sizeof(utf16le), utf16le, sizeof(utf16le)) == 0);

    /* A message to one subscriber asks for its acknowledgement */
    Bursts priv = build(DMR_SMS_TMS, "Hola 123", false, 1234567);
    std::string pp = priv.payload();
    REQUIRE(((uint8_t)pp[30] & 0xE0u) == 0xE0u);
}

TEST_CASE("ETSI IP text leaves as UDP 5016, UTF-16BE", "[dmr-sms]")
{
    Bursts out = build(DMR_SMS_ETSI_IP, "Hola 123");
    REQUIRE(out.dt[PREAMBLES] == DT_DATA_HEADER);
    REQUIRE((out.b[PREAMBLES][0] & 0x3Fu) == 0x02);
    REQUIRE((out.b[PREAMBLES][1] >> 4) == 0x04);

    std::string p = out.payload();
    REQUIRE(u16(p, 20) == 5016);
    REQUIRE(u16(p, 22) == 5016);

    /* 0x00 before every ASCII character: UTF-16BE */
    const char utf16be[] = { 0, 'H', 0, 'o', 0, 'l', 0, 'a', 0, ' ' };
    REQUIRE(p.compare(28, sizeof(utf16be), utf16be, sizeof(utf16be)) == 0);
}

TEST_CASE("Defined short data leaves as DPF 13, SAP 10, ISO 8859-1",
          "[dmr-sms]")
{
    Bursts out = build(DMR_SMS_ETSI_DD, "Hola 123");
    REQUIRE(out.dt[PREAMBLES] == DT_DATA_HEADER);

    /* DPF 13 (short defined), SAP 10 (short data) */
    REQUIRE((out.b[PREAMBLES][0] & 0x3Fu) == 0x0D);
    REQUIRE((out.b[PREAMBLES][1] >> 4) == 0x0A);

    /* The text as it is, one octet per character, no IP layer */
    std::string p = out.payload();
    REQUIRE(p.compare(0, 8, "Hola 123") == 0);
}

TEST_CASE("The packet carries both identifiers", "[dmr-sms]")
{
    Bursts out = build(DMR_SMS_ETSI_IP, "Hola", true, 91, 2142123);

    /* Destination at octet 2, source at 5 (TS 102 361-3, figure 8.3) */
    const uint8_t *h = out.b[PREAMBLES];
    REQUIRE((((uint32_t)h[2] << 16) | (h[3] << 8) | h[4]) == 91);
    REQUIRE((((uint32_t)h[5] << 16) | (h[6] << 8) | h[7]) == 2142123);
    REQUIRE((h[0] & 0x80u) != 0);   /* group destination */
}

TEST_CASE("An empty or oversized text is not sent", "[dmr-sms]")
{
    REQUIRE(build(DMR_SMS_ETSI_IP, "").n == 0);

    std::string longText(DMR_SMS_TEXT_MAX + 1, 'x');
    REQUIRE(build(DMR_SMS_ETSI_IP, longText.c_str()).n == 0);

    /* The whole length fits, in the shortest layout */
    std::string maxText(DMR_SMS_TEXT_MAX, 'x');
    Bursts out = build(DMR_SMS_ETSI_DD, maxText.c_str());
    REQUIRE(out.n > PREAMBLES + 1);
}
