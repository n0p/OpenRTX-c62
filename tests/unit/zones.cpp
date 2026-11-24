/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Zones are data, so they are checked as data: every frequency against the
 * Spanish plan (BOE nº 137 of 9 June 2006 §5.1 and §5.2, as published by the
 * URE) and every shift against the plan's 600 kHz / 7.6 MHz.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "core/zones.h"
#include "core/channel_store.h"
}

/* The Spanish plan, written out: channel n of the VHF repeater zone. */
static uint32_t vhfOut(uint32_t n) { return 145600000u + n * 25000u; }
static uint32_t uhfOut(uint32_t n) { return 438650000u + n * 12500u; }

TEST_CASE("The three zones of the build are there", "[zones]")
{
    REQUIRE(zones_count() == ZONE_COUNT);
    REQUIRE(zones_get(ZONE_VHF_REPEATERS) != nullptr);
    REQUIRE(zones_get(ZONE_UHF_REPEATERS) != nullptr);
    REQUIRE(zones_get(ZONE_PMR446) != nullptr);
    REQUIRE(zones_get(ZONE_COUNT) == nullptr);
    REQUIRE(zones_at(-1) == nullptr);
    REQUIRE(zones_at(ZONE_COUNT) == nullptr);

    REQUIRE(zones_channels(zones_get(ZONE_VHF_REPEATERS)) == 8);
    REQUIRE(zones_channels(zones_get(ZONE_UHF_REPEATERS)) == 101);
    REQUIRE(zones_channels(zones_get(ZONE_PMR446)) == 16);
}

TEST_CASE("R0..R7 are the Spanish 2 m repeater channels", "[zones]")
{
    char name[ZONE_NAME_LEN];

    for (uint16_t i = 0; i < 8; i++) {
        zone_channel_t zc;
        REQUIRE(zones_channel(ZONE_VHF_REPEATERS, i, &zc) == 0);

        /* Output 145.600 + 25 kHz * n, input 600 kHz below (BOE §5.1) */
        REQUIRE(zc.rx == vhfOut(i));
        REQUIRE(zc.shift == -600000);
        REQUIRE(zc.tx == zc.rx - 600000u);
        REQUIRE(zc.tx == 145000000u + i * 25000u);

        const char *ret = zones_channelName(ZONE_VHF_REPEATERS, i, name,
                                            sizeof(name));
        REQUIRE(ret == std::string(name));
        REQUIRE(std::string(name) == "R" + std::to_string(i));

        REQUIRE(zc.mode == OPMODE_FM);
        REQUIRE(zc.bandwidth == BW_12_5);
        REQUIRE(zc.flags & ZONE_FLAG_REPEATER);
        REQUIRE_FALSE(zc.flags & ZONE_FLAG_INTERLEAVED);
        REQUIRE(zones_channelValid(&zc));
    }

    /* R0 and R7, the two ends of the plan, in Hz */
    zone_channel_t zc;
    REQUIRE(zones_channel(ZONE_VHF_REPEATERS, 0, &zc) == 0);
    REQUIRE(zc.rx == 145600000);
    REQUIRE(zc.tx == 145000000);

    REQUIRE(zones_channel(ZONE_VHF_REPEATERS, 7, &zc) == 0);
    REQUIRE(zc.rx == 145775000);
    REQUIRE(zc.tx == 145175000);

    REQUIRE(zones_channel(ZONE_VHF_REPEATERS, 8, &zc) != 0);
}

TEST_CASE("U0..U100 are the 70 cm repeater channels, 7.6 MHz below", "[zones]")
{
    zone_channel_t zc;

    /* U0 = 438.650 MHz, the first output of the Spanish plan (R70 = RU692) */
    REQUIRE(zones_channel(ZONE_UHF_REPEATERS, 0, &zc) == 0);
    REQUIRE(zc.rx == 438650000);
    REQUIRE(zc.tx == 431050000);
    REQUIRE(zc.shift == -7600000);
    REQUIRE(zc.flags & ZONE_FLAG_REPEATER);
    REQUIRE_FALSE(zc.flags & ZONE_FLAG_INTERLEAVED);

    /* The 32 channels of BOE §5.2 are the even indices: U0, U2 ... U62 */
    for (uint16_t i = 0; i <= 62; i += 2) {
        REQUIRE(zones_channel(ZONE_UHF_REPEATERS, i, &zc) == 0);
        REQUIRE(zc.rx == uhfOut(i));
        REQUIRE(zc.tx == zc.rx - 7600000u);
        REQUIRE_FALSE(zc.flags & ZONE_FLAG_INTERLEAVED);
    }

    REQUIRE(zones_channel(ZONE_UHF_REPEATERS, 62, &zc) == 0);
    REQUIRE(zc.rx == 439425000);    /* the last one of the Spanish plan */
    REQUIRE(zc.tx == 431825000);

    /* Odd indices are the 12.5 kHz half channels, flagged as such */
    REQUIRE(zones_channel(ZONE_UHF_REPEATERS, 1, &zc) == 0);
    REQUIRE(zc.rx == 438662500);
    REQUIRE(zc.flags & ZONE_FLAG_INTERLEAVED);

    /* U100 is the top of the table and still inside the 438-440 segment */
    REQUIRE(zones_channel(ZONE_UHF_REPEATERS, 100, &zc) == 0);
    REQUIRE(zc.rx == 439900000);
    REQUIRE(zc.tx == 432300000);
    REQUIRE(zones_channelValid(&zc));

    REQUIRE(zones_channel(ZONE_UHF_REPEATERS, 101, &zc) != 0);
}

TEST_CASE("PMR1..PMR16: 446.00625 to 446.19375 MHz, 500 mW fixed", "[zones]")
{
    zone_channel_t zc;
    char name[ZONE_NAME_LEN];

    for (uint16_t i = 0; i < 16; i++) {
        REQUIRE(zones_channel(ZONE_PMR446, i, &zc) == 0);

        REQUIRE(zc.rx == 446006250u + i * 12500u);
        REQUIRE(zc.tx == zc.rx);            /* simplex, no shift */
        REQUIRE(zc.shift == 0);
        REQUIRE(zc.power == 500);
        REQUIRE(zc.flags & ZONE_FLAG_PMR);
        REQUIRE(zc.flags & ZONE_FLAG_FIXED_POWER);
        REQUIRE(zc.flags & ZONE_FLAG_SIMPLEX);
        REQUIRE_FALSE(zc.flags & ZONE_FLAG_REPEATER);
        REQUIRE(zc.mode == OPMODE_FM);
        REQUIRE(zones_channelValid(&zc));

        zones_channelName(ZONE_PMR446, i, name, sizeof(name));
        REQUIRE(std::string(name) == "PMR" + std::to_string(i + 1));
    }

    REQUIRE(zones_channel(ZONE_PMR446, 0, &zc) == 0);
    REQUIRE(zc.rx == 446006250);
    REQUIRE(zones_channel(ZONE_PMR446, 15, &zc) == 0);
    REQUIRE(zc.rx == 446193750);

    /* The channel spacing is 12.5 kHz and the last one ends the band */
    REQUIRE(zones_channel(ZONE_PMR446, 16, &zc) != 0);

    /* 5 W on a PMR channel is not a PMR channel */
    zone_channel_t loud;
    REQUIRE(zones_channel(ZONE_PMR446, 3, &loud) == 0);
    loud.power = 5000;
    REQUIRE_FALSE(zones_channelValid(&loud));
    loud.power = 500;
    REQUIRE(zones_channelValid(&loud));
}

TEST_CASE("The repeater shift direction is configurable", "[zones]")
{
    zone_channel_t zc;

    REQUIRE_FALSE(zones_shiftInverted());

    zones_setShiftInverted(true);
    REQUIRE(zones_shiftInverted());

    REQUIRE(zones_channel(ZONE_VHF_REPEATERS, 0, &zc) == 0);
    REQUIRE(zc.shift == 600000);
    REQUIRE(zc.tx == 146200000);        /* 145.600 + 600 kHz */

    REQUIRE(zones_channel(ZONE_UHF_REPEATERS, 0, &zc) == 0);
    REQUIRE(zc.shift == 7600000);
    REQUIRE(zc.tx == 446250000);

    /* PMR is simplex: inverting the shift must not invent one */
    REQUIRE(zones_channel(ZONE_PMR446, 0, &zc) == 0);
    REQUIRE(zc.shift == 0);
    REQUIRE(zc.tx == zc.rx);

    zones_setShiftInverted(false);
    REQUIRE(zones_channel(ZONE_VHF_REPEATERS, 0, &zc) == 0);
    REQUIRE(zc.shift == -600000);
}

TEST_CASE("A zone channel becomes a channel_t and a stored record", "[zones]")
{
    channel_t ch;
    REQUIRE(zones_toChannel(ZONE_VHF_REPEATERS, 4, &ch) == 0);
    REQUIRE(ch.mode == OPMODE_FM);
    REQUIRE(ch.rx_frequency == 145700000);      /* R4 */
    REQUIRE(ch.tx_frequency == 145100000);
    REQUIRE(ch.power == 5000);
    REQUIRE(std::string(ch.name) == "R4");

    chstore_channel_t rec;
    REQUIRE(zones_toStore(ZONE_PMR446, 7, &rec) == 0);
    REQUIRE(std::string(rec.name) == "PMR8");
    REQUIRE(rec.rx_freq == 446093750);
    REQUIRE(rec.tx_freq == rec.rx_freq);
    REQUIRE(rec.power == 500);
    REQUIRE(rec.mode == OPMODE_FM);
    REQUIRE(rec.dmr_dest_type == DMR_DEST_NONE);
    REQUIRE(rec.flags & CHSTORE_FLAG_SIMPLEX);

    /* What the zone produces is what the channel store accepts */
    REQUIRE(chstore_valid(&rec));

    REQUIRE(zones_toStore(ZONE_UHF_REPEATERS, 0, &rec) == 0);
    REQUIRE(std::string(rec.name) == "U0");
    REQUIRE(rec.rx_freq == 438650000);
    REQUIRE(rec.tx_freq == 431050000);
    REQUIRE_FALSE(rec.flags & CHSTORE_FLAG_SIMPLEX);
    REQUIRE(chstore_valid(&rec));

    REQUIRE(zones_toStore(ZONE_PMR446, 99, &rec) != 0);
}
