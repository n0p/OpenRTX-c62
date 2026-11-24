/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The band plan is arithmetic, so it is checked exhaustively: every channel of
 * both rasters, its carrier, its designator and the way back. The reference
 * values are the ones a chart prints: 118.005 is the channel at 118.00833,
 * 121.500 (the emergency channel) is on the 25 kHz raster, and the band ends
 * at 136.975.
 */

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "core/airband.h"
}

/**
 * The designator a frequency must print, without calling the code under test
 * to work out what it should be: the slot base plus the 8.33 kHz index of the
 * channel inside it.
 */
static std::string expectedName(uint32_t channel)
{
    uint32_t khz = 118000 + (channel / 3) * 25 + (channel % 3) * 5;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%03u", khz / 1000, khz % 1000);
    return buf;
}

TEST_CASE("Airband band limits", "[airband]")
{
    REQUIRE(airband_inBand(118000000));
    REQUIRE(airband_inBand(121500000));
    REQUIRE(airband_inBand(136975000));
    REQUIRE_FALSE(airband_inBand(117999999));
    REQUIRE_FALSE(airband_inBand(136975001));
    REQUIRE_FALSE(airband_inBand(145500000));
}

TEST_CASE("Airband 25 kHz raster", "[airband]")
{
    REQUIRE(airband_freq25k(0) == 118000000);
    REQUIRE(airband_freq25k(1) == 118025000);
    REQUIRE(airband_freq25k(12) == 118300000);
    REQUIRE(airband_freq25k(AIRBAND_CHANNELS_25K - 1) == 136975000);

    /* Every 25 kHz channel is a channel of the 8.33 kHz raster as well */
    for (uint16_t i = 0; i < AIRBAND_CHANNELS_25K; i++) {
        uint32_t f = airband_freq25k(i);
        int32_t channel = airband_channelIndex(f);
        REQUIRE(channel == (int32_t)3 * i);
        REQUIRE(airband_channelFreq((uint16_t)channel) == f);
    }
}

TEST_CASE("Airband 8.33 kHz raster", "[airband]")
{
    /* 25/3 kHz per channel, so the third one lands on 25 kHz again */
    REQUIRE(airband_channelFreq(0) == 118000000);
    REQUIRE(airband_channelFreq(1) == 118008333);
    REQUIRE(airband_channelFreq(2) == 118016667);
    REQUIRE(airband_channelFreq(3) == 118025000);
    REQUIRE(airband_channelFreq(AIRBAND_CHANNELS_8K33 - 1) == 136975000);

    /* The last channel really is the last one inside the band */
    REQUIRE(airband_inBand(airband_channelFreq(AIRBAND_CHANNELS_8K33 - 1)));
    REQUIRE_FALSE(
        airband_inBand(airband_channelFreq(AIRBAND_CHANNELS_8K33)));

    /* No two channels share a carrier, and none is more than a hertz off */
    for (uint16_t i = 1; i < AIRBAND_CHANNELS_8K33; i++) {
        uint32_t prev = airband_channelFreq(i - 1);
        uint32_t cur = airband_channelFreq(i);
        double ideal = 25000.0 / 3.0;
        REQUIRE((cur - prev) >= 8333);
        REQUIRE((cur - prev) <= 8334);
        REQUIRE(std::abs((double)(cur - prev) - ideal) < 1.0);

        /* And each one is found again from its own frequency */
        REQUIRE(airband_channelIndex(cur) == (int32_t)i);
    }
}

TEST_CASE("Airband channel designators", "[airband]")
{
    char buf[AIRBAND_NAME_LEN];

    /* The designator is not the frequency: it names the channel */
    REQUIRE(std::string(airband_channelName(118000000, buf, sizeof(buf)))
            == "118.000");
    REQUIRE(std::string(airband_channelName(118008333, buf, sizeof(buf)))
            == "118.005");
    REQUIRE(std::string(airband_channelName(118016667, buf, sizeof(buf)))
            == "118.010");
    REQUIRE(std::string(airband_channelName(118025000, buf, sizeof(buf)))
            == "118.025");
    REQUIRE(std::string(airband_channelName(121500000, buf, sizeof(buf)))
            == "121.500");
    REQUIRE(std::string(airband_channelName(136975000, buf, sizeof(buf)))
            == "136.975");

    /* The designators of the whole band: only 00, 05, 10, 25, 30, 35, ... */
    for (uint16_t i = 0; i < AIRBAND_CHANNELS_8K33; i++) {
        std::string name = airband_channelName(airband_channelFreq(i), buf,
                                              sizeof(buf));
        REQUIRE(name == expectedName(i));
        REQUIRE(name.size() == 7);

        uint32_t khz = (uint32_t)std::stoul(name.substr(0, 3)) * 1000
                     + (uint32_t)std::stoul(name.substr(4, 3));
        uint32_t within = (khz % 25);
        const bool onRaster = (within == 0) || (within == 5) || (within == 10);
        REQUIRE(onRaster);
    }

    /* Out of the band there is no designator, and no buffer is overrun */
    REQUIRE(std::string(airband_channelName(145500000, buf, sizeof(buf)))
            == "");
    REQUIRE(std::string(airband_channelName(118000000, buf, 4)) == "");
}

TEST_CASE("Airband designator to carrier", "[airband]")
{
    REQUIRE(airband_nameToFreq("118.000") == 118000000);
    REQUIRE(airband_nameToFreq("118.005") == 118008333);
    REQUIRE(airband_nameToFreq("118.010") == 118016667);
    REQUIRE(airband_nameToFreq("118.025") == 118025000);
    REQUIRE(airband_nameToFreq("121.500") == 121500000);
    REQUIRE(airband_nameToFreq("136.975") == 136975000);
    REQUIRE(airband_nameToFreq(" 136.975 MHz ") == 136975000);
    REQUIRE(airband_nameToFreq("118.005MHz") == 118008333);

    /* What the screen prints reads back to the same carrier */
    char buf[AIRBAND_NAME_LEN];
    for (uint16_t i = 0; i < AIRBAND_CHANNELS_8K33; i += 7) {
        uint32_t f = airband_channelFreq(i);
        airband_channelName(f, buf, sizeof(buf));
        REQUIRE(airband_nameToFreq(buf) == (int32_t)f);
    }

    /* Names that are not channels */
    REQUIRE(airband_nameToFreq("118.020") == -EINVAL); /* not on the raster */
    REQUIRE(airband_nameToFreq("118.001") == -EINVAL);
    REQUIRE(airband_nameToFreq("118.9") == -EINVAL);   /* two decimals     */
    REQUIRE(airband_nameToFreq("118") == -EINVAL);     /* no decimals      */
    REQUIRE(airband_nameToFreq("118.005 m") == -EINVAL);
    REQUIRE(airband_nameToFreq("abc") == -EINVAL);
    REQUIRE(airband_nameToFreq("") == -EINVAL);
    REQUIRE(airband_nameToFreq(nullptr) == -EINVAL);

    /* Legal designators outside the band */
    REQUIRE(airband_nameToFreq("117.000") == -ERANGE);
    REQUIRE(airband_nameToFreq("137.000") == -ERANGE);
}

TEST_CASE("Airband snapping", "[airband]")
{
    /* A frequency reached by stepping 8333 Hz has drifted off the channel */
    uint32_t stepped = 118000000;
    for (int i = 0; i < 3; i++)
        stepped += 8333;
    REQUIRE(stepped == 118024999);
    REQUIRE(airband_snap(stepped) == 118025000);

    /* Halfway between two channels, the nearer one wins */
    REQUIRE(airband_snap(118004000) == 118000000);
    REQUIRE(airband_snap(118005000) == 118008333);
    REQUIRE(airband_snap(118012000) == 118008333);

    /* A channel is left where it is, and so is anything outside the band */
    REQUIRE(airband_snap(118008333) == 118008333);
    REQUIRE(airband_snap(145500000) == 145500000);
    REQUIRE(airband_snap(100000000) == 100000000);

    /* 1000 steps of the knob: never more than a kHz off, and never lost */
    uint32_t f = 118000000;
    for (int i = 0; i < 1000; i++) {
        f += 8333;
        uint32_t snapped = airband_snap(f);
        REQUIRE(airband_channelIndex(snapped) == (int32_t)(i + 1));
        REQUIRE(snapped == airband_channelFreq((uint16_t)(i + 1)));
    }
}
