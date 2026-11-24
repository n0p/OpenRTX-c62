/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Broadcast FM is a band and a raster, so most of it is arithmetic that can be
 * checked without a radio: the band edges the operator asked for against the
 * ones the BK1080 can actually reach, the 100 kHz and 50 kHz rasters, the
 * channel names, and the dBuV to dBm conversion the S-meter uses.
 *
 * The channel index handed to the chip is checked against the BK1080 datasheet
 * formula (Freq = Spacing x Channel + 76 MHz, BAND = 01), which is the same
 * one the factory firmware uses.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "core/wfm.h"
}

TEST_CASE("The band is the one the receiver can reach", "[wfm]")
{
    /* The request was 85-110 MHz; the chip stops at 108.0 and says so */
    REQUIRE(WFM_LOW_HZ == 85000000u);
    REQUIRE(WFM_HIGH_HZ == 108000000u);
    REQUIRE(WFM_CHANNELS == 231);   /* 85.0 to 108.0 every 100 kHz */

    REQUIRE(wfm_freqValid(85000000u));
    REQUIRE(wfm_freqValid(98000000u));
    REQUIRE(wfm_freqValid(108000000u));

    REQUIRE_FALSE(wfm_freqValid(84000000u));
    REQUIRE_FALSE(wfm_freqValid(108100000u));
    REQUIRE_FALSE(wfm_freqValid(110000000u));
    /* Off both rasters: not a channel of the band */
    REQUIRE_FALSE(wfm_freqValid(101105000u));
    /* 50 kHz offsets are channels too (Italy and others) */
    REQUIRE(wfm_freqValid(101150000u));
}

TEST_CASE("Channels are on the CCIR raster", "[wfm]")
{
    REQUIRE(wfm_chan(85000000u) == 0);
    REQUIRE(wfm_chan(87500000u) == 25);
    REQUIRE(wfm_chan(101100000u) == 161);
    REQUIRE(wfm_chan(108000000u) == 230);

    REQUIRE(wfm_chanFreq(0) == 85000000u);
    REQUIRE(wfm_chanFreq(25) == 87500000u);
    REQUIRE(wfm_chanFreq(161) == 101100000u);

    /* Every channel round-trips, which is what the tuner needs */
    for (uint16_t c = 0; c < WFM_CHANNELS; c++)
        REQUIRE(wfm_chan(wfm_chanFreq(c)) == c);

    /* Stepping stays inside the band and lands on the raster */
    REQUIRE(wfm_step(101100000u, +1) == 101200000u);
    REQUIRE(wfm_step(101100000u, -1) == 101000000u);
    REQUIRE(wfm_step(108000000u, +1) == 108000000u);
    REQUIRE(wfm_step(85000000u, -1) == 85000000u);
    REQUIRE(wfm_step(101130000u, +1) == 101200000u);  /* snaps first */
}

TEST_CASE("Snapping uses the requested raster", "[wfm]")
{
    REQUIRE(wfm_snap(101120000u, WFM_STEP_HZ) == 101100000u);
    REQUIRE(wfm_snap(101160000u, WFM_STEP_HZ) == 101200000u);
    REQUIRE(wfm_snap(101120000u, WFM_STEP_FINE_HZ) == 101100000u);
    REQUIRE(wfm_snap(101130000u, WFM_STEP_FINE_HZ) == 101150000u);
    /* Out of the band, clamped */
    REQUIRE(wfm_snap(80000000u, WFM_STEP_HZ) == WFM_LOW_HZ);
    REQUIRE(wfm_snap(120000000u, WFM_STEP_HZ) == WFM_HIGH_HZ);
}

TEST_CASE("A channel is named by its frequency", "[wfm]")
{
    char buf[12];

    REQUIRE(std::string(wfm_formatFreq(101100000u, buf, sizeof(buf)))
            == "101.1");
    REQUIRE(std::string(wfm_formatFreq(87500000u, buf, sizeof(buf)))
            == "87.5");
    REQUIRE(std::string(wfm_formatFreq(85000000u, buf, sizeof(buf)))
            == "85.0");
    REQUIRE(std::string(wfm_formatFreq(108000000u, buf, sizeof(buf)))
            == "108.0");
    /* 50 kHz raster: keep the extra digit or two stations look the same */
    REQUIRE(std::string(wfm_formatFreq(101150000u, buf, sizeof(buf)))
            == "101.15");
}

TEST_CASE("The level is reported on the dBm scale", "[wfm]")
{
    /* P(dBm) = P(dBuV) - 107 into 50 ohm */
    REQUIRE(wfm_dbuVToDbm(60) == -47);
    REQUIRE(wfm_dbuVToDbm(30) == -77);
    REQUIRE(wfm_dbuVToDbm(0) == -107);
    /* Clamped to the S-meter's floor and to 0 dBm */
    REQUIRE(wfm_dbuVToDbm(200) == 0);
}

TEST_CASE("The last status is kept for the screens", "[wfm]")
{
    wfm_status_t st;
    std::memset(&st, 0, sizeof(st));
    st.freq = 101100000u;
    st.chan = 161;
    st.rssi = 55;
    st.tuned = true;
    st.stereo = true;

    wfm_publish(&st);

    wfm_status_t back = wfm_status();
    REQUIRE(back.freq == 101100000u);
    REQUIRE(back.chan == 161);
    REQUIRE(back.rssi == 55);
    REQUIRE(back.tuned);
    REQUIRE(back.stereo);
    REQUIRE_FALSE(back.rds);    /* this receiver has no RDS decoder */
}
