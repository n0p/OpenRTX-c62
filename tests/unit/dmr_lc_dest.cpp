/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The destination of a DMR call has to reach the link control, which is what
 * the receiver reads to decide whether the call is for it: a talkgroup call is
 * FLCO 0x00 and every radio on that talkgroup opens, a unit call is FLCO 0x03
 * and only the addressed subscriber opens.  The source ID travels in the same
 * PDU: it is the radio's own DMR ID from the settings.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

extern "C" {
#include "protocols/DMR/DmrTx.h"
}

/* Read n bits, MSB first, from a packed 12-byte LC */
static uint32_t bits(const uint8_t lc[12], int off, int n)
{
    uint32_t v = 0;

    for (int i = 0; i < n; i++) {
        int b = off + i;
        v = (v << 1) | ((lc[b >> 3] >> (7 - (b & 7))) & 1u);
    }

    return v;
}

TEST_CASE("A group call is addressed to its talkgroup", "[dmr-tx]")
{
    uint8_t lc[12];

    dmrTx_linkControl(lc, 2142123, 2142, true, DMR_DT_VOICE_LC_HEADER);

    REQUIRE(bits(lc, 0, 1) == 0);        /* PF */
    REQUIRE(bits(lc, 2, 6) == 0x00);     /* FLCO: group voice channel user */
    REQUIRE(bits(lc, 8, 8) == 0x00);     /* FID: standard */
    REQUIRE(bits(lc, 16, 8) == 0x00);    /* service options */
    REQUIRE(bits(lc, 24, 24) == 2142);   /* destination: the talkgroup */
    REQUIRE(bits(lc, 48, 24) == 2142123);/* source: our DMR ID */
}

TEST_CASE("A unit call is addressed to one subscriber", "[dmr-tx]")
{
    uint8_t lc[12];

    dmrTx_linkControl(lc, 2142123, 1234567, false, DMR_DT_VOICE_LC_HEADER);

    REQUIRE(bits(lc, 2, 6) == 0x03);         /* FLCO: unit-to-unit */
    REQUIRE(bits(lc, 24, 24) == 1234567);
    REQUIRE(bits(lc, 48, 24) == 2142123);
}

TEST_CASE("The LC carries the identifiers whole and no more", "[dmr-tx]")
{
    uint8_t lc[12];

    /* The largest 24-bit identifiers */
    dmrTx_linkControl(lc, 0xFFFFFF, 0xFFFFFF, true, DMR_DT_VOICE_LC_HEADER);
    REQUIRE(bits(lc, 24, 24) == 0xFFFFFF);
    REQUIRE(bits(lc, 48, 24) == 0xFFFFFF);

    /* A 32-bit value must be cut to its 24 bits, not shifted into the CRC */
    dmrTx_linkControl(lc, 0x01234567, 0x0089ABCD, true, DMR_DT_VOICE_LC_HEADER);
    REQUIRE(bits(lc, 24, 24) == 0x89ABCD);
    REQUIRE(bits(lc, 48, 24) == 0x234567);
}

TEST_CASE("The terminator of a data call keeps the destination", "[dmr-tx]")
{
    uint8_t lc[12];

    dmrTx_dataTerminator(lc, 2142123, 2142, true);

    /*
     * Terminator Data Link Control (TS 102 361-3 table 7.1): FLCO 110000, then
     * the destination at bit 16 (there is no service-options octet), the source
     * at bit 40, and the G/I bit at 64.
     */
    REQUIRE(bits(lc, 2, 6) == 0x30);     /* terminator with LC */
    REQUIRE(bits(lc, 16, 24) == 2142);
    REQUIRE(bits(lc, 40, 24) == 2142123);
    REQUIRE(bits(lc, 64, 1) == 1);       /* G/I: the call was a group one */
    REQUIRE(bits(lc, 66, 1) == 1);       /* FMF: last fragment */

    dmrTx_dataTerminator(lc, 2142123, 1234567, false);
    REQUIRE(bits(lc, 16, 24) == 1234567);
    REQUIRE(bits(lc, 64, 1) == 0);       /* unit call */
}
