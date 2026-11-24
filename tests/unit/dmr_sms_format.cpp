/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The SMS payload format is a setting: the operator picks the flavour that the
 * other radios of his network read, and the radio has to build exactly that.
 * Each format is encoded with the real encoder and read back with the real
 * decoder, so this is the round trip of the radio and not a table lookup.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "core/settings.h"
#include "protocols/DMR/DmrSms.h"
}

static int encodeDecode(uint8_t format, std::string &textOut,
                        struct dmrSmsMessage *msgOut)
{
    struct dmrSmsEncode req;
    struct dmrSmsPacket pkt;

    memset(&req, 0, sizeof(req));
    req.app = dmrSms_appForFormat(format);
    req.src = 2142123;
    req.dst = 2142;
    req.group = true;
    req.confirmed = false;
    req.tmsSeq = 5;                 /* TMS needs a sequence number */
    req.text = "Hola 123";

    if (dmrSms_encode(&req, &pkt) != 0)
        return -1;

    struct dmrSmsMessage msg;
    memset(&msg, 0, sizeof(msg));

    if (dmrSms_decodePacket(pkt.header, pkt.blocks, pkt.blockCount, &msg) != 0)
        return -2;

    textOut = msg.text;

    if (msgOut != nullptr)
        *msgOut = msg;

    return 0;
}

TEST_CASE("Auto is the format this firmware has always sent", "[dmr-sms]")
{
    REQUIRE(dmrSms_appForFormat(DMR_SMS_FMT_AUTO) == DMR_SMS_APP_IP_HC);

    /* An unknown value (a record from another build) degrades to auto */
    REQUIRE(dmrSms_appForFormat(200) == DMR_SMS_APP_IP_HC);
}

TEST_CASE("Every format builds the application it names", "[dmr-sms]")
{
    REQUIRE(dmrSms_appForFormat(DMR_SMS_FMT_DD_UTF16BE) ==
            DMR_SMS_APP_DD_UTF16BE);
    REQUIRE(dmrSms_appForFormat(DMR_SMS_FMT_DD_UTF8) == DMR_SMS_APP_DD_UTF8);
    REQUIRE(dmrSms_appForFormat(DMR_SMS_FMT_IP_5016) == DMR_SMS_APP_IP);
    REQUIRE(dmrSms_appForFormat(DMR_SMS_FMT_IP_HC) == DMR_SMS_APP_IP_HC);
    REQUIRE(dmrSms_appForFormat(DMR_SMS_FMT_ANYTONE) == DMR_SMS_APP_ANYTONE);
    REQUIRE(dmrSms_appForFormat(DMR_SMS_FMT_TMS) == DMR_SMS_APP_TMS);
    REQUIRE(dmrSms_appForFormat(DMR_SMS_FMT_HYTERA) == DMR_SMS_APP_HYTERA);

    /* ... and none of them is silent */
    for (uint8_t f = 0; f < DMR_SMS_FMT_COUNT; f++)
        REQUIRE(dmrSms_appIsText(dmrSms_appForFormat(f)));
}

TEST_CASE("The text survives every payload format", "[dmr-sms]")
{
    for (uint8_t f = 0; f < DMR_SMS_FMT_COUNT; f++) {
        std::string text;
        struct dmrSmsMessage msg;

        INFO("format " << (int)f << " = " << settings_smsFormatName(f));
        REQUIRE(encodeDecode(f, text, &msg) == 0);
        REQUIRE(text == "Hola 123");
        REQUIRE(msg.crcOk);
        REQUIRE(msg.src == 2142123);
        REQUIRE(msg.dst == 2142);
        REQUIRE(msg.group);
    }
}

TEST_CASE("The defined-data formats differ in the way ETSI says", "[dmr-sms]")
{
    std::string text;
    struct dmrSmsMessage utf16;
    struct dmrSmsMessage utf8;

    REQUIRE(encodeDecode(DMR_SMS_FMT_DD_UTF16BE, text, &utf16) == 0);
    REQUIRE(encodeDecode(DMR_SMS_FMT_DD_UTF8, text, &utf8) == 0);

    /* DD_HEAD with DD = 0x14 (UTF-16BE) against DD = 0x12 (UTF-8) */
    REQUIRE(utf16.dd == DMR_SMS_DD_UTF16BE);
    REQUIRE(utf8.dd == DMR_SMS_DD_UTF8);
    REQUIRE(utf16.app == DMR_SMS_APP_DD_UTF16BE);
    REQUIRE(utf8.app == DMR_SMS_APP_DD_UTF8);

    /* UTF-16BE needs two octets per character, UTF-8 one for ASCII */
    struct dmrSmsEncode req;
    struct dmrSmsPacket pkt;
    memset(&req, 0, sizeof(req));
    req.src = 1;
    req.dst = 2;
    req.text = "Hola 123";

    req.app = DMR_SMS_APP_DD_UTF8;
    REQUIRE(dmrSms_encode(&req, &pkt) == 0);
    unsigned utf8Blocks = pkt.blockCount;

    req.app = DMR_SMS_APP_DD_UTF16BE;
    REQUIRE(dmrSms_encode(&req, &pkt) == 0);
    REQUIRE(pkt.blockCount > utf8Blocks);
}

TEST_CASE("The IP formats carry the ETSI UDP port", "[dmr-sms]")
{
    std::string text;
    struct dmrSmsMessage ip;
    struct dmrSmsMessage hc;
    struct dmrSmsMessage anytone;

    REQUIRE(encodeDecode(DMR_SMS_FMT_IP_5016, text, &ip) == 0);
    REQUIRE(encodeDecode(DMR_SMS_FMT_IP_HC, text, &hc) == 0);
    REQUIRE(encodeDecode(DMR_SMS_FMT_ANYTONE, text, &anytone) == 0);

    REQUIRE(ip.udpDst == 5016);
    REQUIRE(hc.udpDst == 5016);
    REQUIRE(anytone.udpDst == 5016);

    /*
     * The receiver classifies any 5016 record as ETSI IP; what tells the two
     * flavours apart is the SAP the encoder put in the header, so that is what
     * is checked here (and the text, of course).
     */
    struct dmrSmsEncode req;
    struct dmrSmsPacket pkt;

    memset(&req, 0, sizeof(req));
    req.src = 1;
    req.dst = 2;
    req.text = "Hola 123";

    req.app = DMR_SMS_APP_IP;
    REQUIRE(dmrSms_encode(&req, &pkt) == 0);
    REQUIRE(pkt.app == DMR_SMS_APP_IP);

    req.app = DMR_SMS_APP_IP_HC;
    REQUIRE(dmrSms_encode(&req, &pkt) == 0);
    REQUIRE(pkt.app == DMR_SMS_APP_IP_HC);

    req.app = DMR_SMS_APP_ANYTONE;
    REQUIRE(dmrSms_encode(&req, &pkt) == 0);
    REQUIRE(pkt.app == DMR_SMS_APP_ANYTONE);

    REQUIRE(anytone.app == DMR_SMS_APP_ANYTONE);
    REQUIRE(text == "Hola 123");
}

TEST_CASE("A message reassembled burst by burst keeps the format", "[dmr-sms]")
{
    struct dmrSmsEncode req;
    struct dmrSmsPacket pkt;

    memset(&req, 0, sizeof(req));
    req.app = dmrSms_appForFormat(DMR_SMS_FMT_DD_UTF8);
    req.src = 2142123;
    req.dst = 91;
    req.group = true;
    req.text = "Mensaje de prueba";

    REQUIRE(dmrSms_encode(&req, &pkt) == 0);

    struct dmrSmsRx rx;
    dmrSmsRx_reset(&rx);

    bool done = false;
    struct dmrSmsRxResult res;

    for (unsigned i = 0; i <= pkt.blockCount; i++) {
        uint8_t dataType = 0;
        uint8_t info[96];
        uint8_t block[12];
        memset(&res, 0, sizeof(res));

        REQUIRE(dmrSms_packetBurst(&pkt, i, &dataType, info));

        memset(block, 0, sizeof(block));
        for (int b = 0; b < 96; b++)
            block[b >> 3] |= (uint8_t)(info[b] << (7 - (b & 7)));

        if (dmrSmsRx_feed(&rx, dataType, block, &res) == 1) {
            REQUIRE(res.message);
            REQUIRE(res.msg.app == DMR_SMS_APP_DD_UTF8);
            REQUIRE(res.msg.dd == DMR_SMS_DD_UTF8);
            REQUIRE(std::string(res.msg.text) == "Mensaje de prueba");
            done = true;
        }
    }

    REQUIRE(done);
}
