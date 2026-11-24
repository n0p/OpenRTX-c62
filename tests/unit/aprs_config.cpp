/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * APRS configuration validation: the callsign and the digipeater path are
 * checked before a frame is built, because a frame with an invalid source or
 * path is silently dropped by every digipeater and the operator would only see
 * a beacon that never appears in the map.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cstdio>
#include <string>

extern "C" {
#include "protocols/APRS/ax25.h"
#include "protocols/APRS/aprs.h"
}

TEST_CASE("Callsigns are checked as AX.25 addresses", "[aprs]")
{
    REQUIRE(ax25_addrValid("EA5ABC"));
    REQUIRE(ax25_addrValid("N0CALL"));
    REQUIRE(ax25_addrValid("ea5abc"));     /* case free, upper-cased on air */
    REQUIRE(ax25_addrValid("K1ABC"));
    REQUIRE(ax25_addrValid("9A1A"));

    REQUIRE_FALSE(ax25_addrValid(""));
    REQUIRE_FALSE(ax25_addrValid(nullptr));
    REQUIRE_FALSE(ax25_addrValid("EA5ABCD"));      /* seven characters */
    REQUIRE_FALSE(ax25_addrValid("ABCDEF"));       /* no digit */
    REQUIRE_FALSE(ax25_addrValid("12345"));        /* no letter */
    REQUIRE_FALSE(ax25_addrValid("EA5-ABC"));      /* SSID is a separate field */
    REQUIRE_FALSE(ax25_addrValid("EA 5AB"));       /* no spaces */
    REQUIRE_FALSE(ax25_addrValid("EA5@#"));
}

TEST_CASE("SSIDs and digipeater aliases", "[aprs]")
{
    REQUIRE(ax25_ssidValid(0));
    REQUIRE(ax25_ssidValid(9));
    REQUIRE(ax25_ssidValid(15));
    REQUIRE_FALSE(ax25_ssidValid(16));
    REQUIRE_FALSE(ax25_ssidValid(-1));

    /* The standard new-style path */
    REQUIRE(ax25_digiValid("WIDE1-1"));
    REQUIRE(ax25_digiValid("WIDE2-1"));
    REQUIRE(ax25_digiValid("WIDE2-2"));
    REQUIRE(ax25_digiValid("WIDE7-7"));
    REQUIRE(ax25_digiValid("ED5YAM"));
    REQUIRE(ax25_digiValid("ED5YAM-3"));

    /* WIDE needs a use count, WIDEn a valid n and a use count in 1..7 */
    REQUIRE_FALSE(ax25_digiValid("WIDE"));
    REQUIRE_FALSE(ax25_digiValid("WIDE1"));
    REQUIRE_FALSE(ax25_digiValid("WIDE0-1"));
    REQUIRE_FALSE(ax25_digiValid("WIDE8-1"));
    REQUIRE_FALSE(ax25_digiValid("WIDE2-8"));
    REQUIRE_FALSE(ax25_digiValid("WIDE2-0"));
    REQUIRE_FALSE(ax25_digiValid("WIDE2-1-2"));

    /* Retired aliases: refused, APRS 1.0.1 replaced them with WIDE */
    REQUIRE_FALSE(ax25_digiValid("TRACE"));
    REQUIRE_FALSE(ax25_digiValid("RELAY"));
    REQUIRE_FALSE(ax25_digiValid("GATE"));

    REQUIRE_FALSE(ax25_digiValid(""));
    REQUIRE_FALSE(ax25_digiValid(nullptr));
}

TEST_CASE("Paths are validated item by item", "[aprs]")
{
    uint8_t n = 0xFF;

    REQUIRE(ax25_pathValid("WIDE1-1,WIDE2-1", &n));
    REQUIRE(n == 2);

    REQUIRE(ax25_pathValid("WIDE1-1,WIDE2-2", &n));
    REQUIRE(n == 2);

    REQUIRE(ax25_pathValid("ED5YAM-3,WIDE2-1", &n));
    REQUIRE(n == 2);

    /* No digipeaters is a valid, direct path */
    REQUIRE(ax25_pathValid("", &n));
    REQUIRE(n == 0);
    REQUIRE(ax25_pathValid(nullptr, &n));
    REQUIRE(n == 0);

    /* Whatever a typo can produce */
    REQUIRE_FALSE(ax25_pathValid("WIDE1-1,", &n));
    REQUIRE_FALSE(ax25_pathValid(",WIDE1-1", &n));
    REQUIRE_FALSE(ax25_pathValid("WIDE1-1,,WIDE2-1", &n));
    REQUIRE_FALSE(ax25_pathValid("WIDE1-1, WIDE2-1", &n));   /* space */
    REQUIRE_FALSE(ax25_pathValid("WIDE1", &n));
    REQUIRE_FALSE(ax25_pathValid("TRACE", &n));
    REQUIRE_FALSE(ax25_pathValid("WIDE1-1;WIDE2-1", &n));    /* ; not , */
    REQUIRE_FALSE(ax25_pathValid("WIDE1-1,WIDE2-1,WIDE2-1,WIDE2-1,"
                                 "WIDE2-1,WIDE2-1,WIDE2-1,WIDE2-1,"
                                 "WIDE2-1", &n));           /* nine digis */

    /* Eight is the AX.25 address-field limit */
    REQUIRE(ax25_pathValid("WIDE1-1,WIDE2-1,WIDE2-1,WIDE2-1,"
                           "WIDE2-1,WIDE2-1,WIDE2-1,WIDE2-1", &n));
    REQUIRE(n == 8);
}

TEST_CASE("A validated path is what the frame builder puts on the wire",
          "[aprs]")
{
    ax25_addr_t digis[AX25_MAX_DIGIS];

    REQUIRE(ax25_pathValid("WIDE1-1,WIDE2-1", nullptr));
    REQUIRE(ax25_parse_path("WIDE1-1,WIDE2-1", digis, AX25_MAX_DIGIS) == 2);

    /* WIDE1-1: call "WIDE1 ", SSID 1 */
    REQUIRE(std::string(digis[0].call, AX25_CALL_LEN) == "WIDE1 ");
    REQUIRE(digis[0].ssid == 1);
    REQUIRE(std::string(digis[1].call, AX25_CALL_LEN) == "WIDE2 ");
    REQUIRE(digis[1].ssid == 1);
}

/* ------------------------------------------------------------------ packets --- */

/*
 * The information field of a packet, built from the settings the operator
 * entered: a hand-entered position (there is no GPS on the C62), the symbol and
 * the comment.  The APRS screen calls these with state.settings.aprs_*.
 */
static aprs_pos_t testPosition()
{
    aprs_pos_t p;
    memset(&p, 0, sizeof(p));
    p.lat = 38.0;
    p.lon = -1.0;
    p.table = '/';
    p.symbol = '>';
    snprintf(p.comment, sizeof(p.comment), "%s", "OpenRTX C62");
    return p;
}

TEST_CASE("A position packet carries the hand-entered coordinates", "[aprs]")
{
    aprs_pos_t p = testPosition();
    char info[128];

    size_t n = aprs_build_position(info, sizeof(info), &p, false);
    REQUIRE(n > 0);

    /* !DDMM.mmN/DDDMM.mmW>comment */
    REQUIRE(info[0] == '!');
    REQUIRE(std::string(info).rfind("!3800.00N/00100.00W>", 0) == 0);
    REQUIRE(std::string(info).find("OpenRTX C62") != std::string::npos);

    /* The compressed form is there for the shorter frame */
    char comp[64];
    REQUIRE(aprs_build_position_compressed(comp, sizeof(comp), &p, false) > 0);

    /* Messaging-capable stations announce with '=' instead of '!' */
    n = aprs_build_position(info, sizeof(info), &p, true);
    REQUIRE(n > 0);
    REQUIRE(info[0] == '=');

    /* Southern/western hemisphere and a full minute value */
    p.lat = -33.95;
    p.lon = -70.6667;
    REQUIRE(aprs_build_position(info, sizeof(info), &p, false) > 0);
    REQUIRE(std::string(info).find("3357.00S/07040.00W") != std::string::npos);
}

TEST_CASE("Status and message fields are built for the configured station",
          "[aprs]")
{
    char info[128];

    REQUIRE(aprs_build_status(info, sizeof(info), "OpenRTX C62") > 0);
    REQUIRE(std::string(info) == ">OpenRTX C62");

    /* :ADDRESSEE:text{nn  (APRS101 chapter 12), addressee space padded to 9 */
    REQUIRE(aprs_build_message(info, sizeof(info), "EA5ABC-9", "Hola", "01") > 0);
    REQUIRE(std::string(info) == ":EA5ABC-9 :Hola{01");

    REQUIRE(aprs_build_ack(info, sizeof(info), "EA5ABC-9", "01") > 0);
    REQUIRE(std::string(info) == ":EA5ABC-9 :ack01");

    /* A buffer that cannot hold the packet is a failure, not a cut frame */
    REQUIRE(aprs_build_message(info, 8, "EA5ABC-9", "Hola", "01") == 0);
}
