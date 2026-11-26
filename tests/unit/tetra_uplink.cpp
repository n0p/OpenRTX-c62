/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The "Cell uplink" session of the TETRA mode, without a radio.
 *
 * Two things are checked here:
 *
 *  - the duplex conversion of the downlink (TS 100 392-15, table 2), which is
 *    what turns "the cell is on 430.500 MHz" into the uplink frequency when the
 *    cell event does not carry one;
 *  - the Off / On / "no uplink" rules the quick menu row shows: On only
 *    retunes when a frequency is known, Off goes back to the frequency it came
 *    from, and a mode change or the operator tuning elsewhere ends the session.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>

extern "C" {
#include "protocols/TETRA/TetraStatus.h"
#include "protocols/TETRA/TetraUplink.h"
#include "rtx/rtx.h"
}

namespace
{

/* A TETRA cell on the downlink the reports below use */
struct tetraStatus cellStatus(uint32_t dl, uint32_t ul, uint8_t band,
                              uint8_t duplex, bool reverse)
{
    struct tetraStatus ts;

    std::memset(&ts, 0, sizeof(ts));
    ts.sync = true;
    ts.cell = true;
    ts.mcc = 228;
    ts.mnc = 8889;
    ts.colourCode = 1;
    ts.sysinfo = true;
    ts.la = 1;
    ts.dlFreq = dl;
    ts.ulFreq = ul;
    ts.band = band;
    ts.duplexSpacing = duplex;
    ts.reverseOp = reverse;
    return ts;
}

} // namespace

TEST_CASE("The uplink frequency follows the duplex spacing table", "[tetra][ul]")
{
    /* 400 MHz band, duplex code 0: 10 MHz below, or above if reversed */
    REQUIRE(tetraUl_hzFromDownlink(392512500u, 3, 0, false) == 382512500u);
    REQUIRE(tetraUl_hzFromDownlink(392512500u, 3, 0, true) == 402512500u);
    /* 800 MHz band, code 1: 45 MHz */
    REQUIRE(tetraUl_hzFromDownlink(851000000u, 8, 1, false) == 806000000u);
    /* Reserved spacing and unknown frequency */
    REQUIRE(tetraUl_hzFromDownlink(392512500u, 3, 6, false) == 0);
    REQUIRE(tetraUl_hzFromDownlink(0, 3, 0, false) == 0);
    /* A spacing wider than the downlink cannot give a positive frequency */
    REQUIRE(tetraUl_hzFromDownlink(1000000u, 1, 0, false) == 0);
}

TEST_CASE("Cell uplink needs a known uplink frequency", "[tetra][ul]")
{
    tetraUplink_reset();

    SECTION("No cell at all")
    {
        struct tetraStatus ts;

        std::memset(&ts, 0, sizeof(ts));
        REQUIRE(tetraUplink_start(&ts, 430500000u) == -1);
        REQUIRE_FALSE(tetraUplink_active());
        REQUIRE(tetraUplink_display(430500000u) == TETRA_UL_NO_FREQ);
    }

    SECTION("Cell without SYSINFO and without a published uplink")
    {
        struct tetraStatus ts = cellStatus(0, 0, 0, 0, false);

        ts.sysinfo = false;
        REQUIRE(tetraUplink_start(&ts, 430500000u) == -1);
        REQUIRE_FALSE(tetraUplink_active());
        REQUIRE(tetraUplink_display(430500000u) == TETRA_UL_NO_FREQ);
    }

    SECTION("Published uplink")
    {
        struct tetraStatus ts = cellStatus(430500000u, 420500000u, 0, 0,
                                           false);

        REQUIRE(tetraUplink_start(&ts, 430500000u) == 0);
        REQUIRE(tetraUplink_active());
        REQUIRE(tetraUplink_freq() == 420500000u);
        REQUIRE(tetraUplink_backHz() == 430500000u);
        REQUIRE(tetraUplink_display(420500000u) == TETRA_UL_ON);
    }

    SECTION("Uplink deduced from the downlink by duplex conversion")
    {
        struct tetraStatus ts = cellStatus(430500000u, 0, 3, 0, false);

        REQUIRE(tetraUplink_start(&ts, 430500000u) == 0);
        REQUIRE(tetraUplink_freq() == 420500000u);
    }

    SECTION("Reserved duplex spacing still says no uplink")
    {
        struct tetraStatus ts = cellStatus(430500000u, 0, 3, 6, false);

        REQUIRE(tetraUplink_start(&ts, 430500000u) == -1);
        REQUIRE(tetraUplink_display(430500000u) == TETRA_UL_NO_FREQ);
    }
}

TEST_CASE("Cell uplink goes back to the downlink it came from", "[tetra][ul]")
{
    tetraUplink_reset();

    struct tetraStatus ts = cellStatus(430500000u, 420500000u, 3, 0, false);
    REQUIRE(tetraUplink_start(&ts, 430500000u) == 0);
    REQUIRE(tetraUplink_active());

    /* The mode that decodes the uplink needs the cell's colour code */
    uint16_t mcc = 0, mnc = 0;
    uint8_t cc = 0;
    REQUIRE(tetraUplink_cell(&mcc, &mnc, &cc));
    REQUIRE(mcc == ts.mcc);
    REQUIRE(mnc == ts.mnc);
    REQUIRE(cc == ts.colourCode);

    /* Off */
    REQUIRE(tetraUplink_stop());
    REQUIRE_FALSE(tetraUplink_active());
    REQUIRE(tetraUplink_display(420500000u) == TETRA_UL_OFF);
    REQUIRE(tetraUplink_freq() == 0);

    /* Off on a session that was not following changes nothing */
    REQUIRE_FALSE(tetraUplink_stop());
}

TEST_CASE("Leaving the TETRA mode returns to the downlink", "[tetra][ul]")
{
    tetraUplink_reset();

    struct tetraStatus ts = cellStatus(430500000u, 420500000u, 3, 0, false);
    REQUIRE(tetraUplink_start(&ts, 430500000u) == 0);

    uint32_t hz = 0;
    /* Still in TETRA, still on the uplink: nothing to do */
    REQUIRE_FALSE(tetraUplink_tick(OPMODE_TETRA, 420500000u, &hz));
    REQUIRE(tetraUplink_active());

    /* The operator switches to FM: the radio has to go back to the downlink */
    REQUIRE(tetraUplink_tick(OPMODE_FM, 420500000u, &hz));
    REQUIRE(hz == 430500000u);
    REQUIRE_FALSE(tetraUplink_active());
    REQUIRE(tetraUplink_display(430500000u) == TETRA_UL_OFF);
}

TEST_CASE("Tuning elsewhere ends the session where the operator tuned",
          "[tetra][ul]")
{
    tetraUplink_reset();

    struct tetraStatus ts = cellStatus(430500000u, 420500000u, 3, 0, false);
    REQUIRE(tetraUplink_start(&ts, 430500000u) == 0);

    uint32_t hz = 0;
    REQUIRE_FALSE(tetraUplink_tick(OPMODE_TETRA, 431000000u, &hz));
    REQUIRE_FALSE(tetraUplink_active());
    REQUIRE(tetraUplink_display(431000000u) == TETRA_UL_OFF);

    /* "No uplink" also ends with the mode, and retunes nothing */
    struct tetraStatus none;
    std::memset(&none, 0, sizeof(none));
    REQUIRE(tetraUplink_start(&none, 430500000u) == -1);
    REQUIRE(tetraUplink_display(430500000u) == TETRA_UL_NO_FREQ);
    REQUIRE_FALSE(tetraUplink_tick(OPMODE_FM, 430500000u, &hz));
    REQUIRE(tetraUplink_display(430500000u) == TETRA_UL_OFF);
}

TEST_CASE("A cell that goes away sends the radio back to the downlink",
          "[tetra][ul]")
{
    tetraUplink_reset();

    struct tetraStatus ts = cellStatus(430500000u, 420500000u, 3, 0, false);
    REQUIRE(tetraUplink_start(&ts, 430500000u) == 0);
    REQUIRE(tetraUplink_backHz() == 430500000u);

    tetraUplink_cellLost();
    REQUIRE_FALSE(tetraUplink_active());

    uint32_t hz = 0;
    REQUIRE(tetraUplink_tick(OPMODE_TETRA, 420500000u, &hz));
    REQUIRE(hz == 430500000u);

    /* Applied once: the next tick has nothing left to do */
    hz = 0;
    REQUIRE_FALSE(tetraUplink_tick(OPMODE_TETRA, 430500000u, &hz));
}
