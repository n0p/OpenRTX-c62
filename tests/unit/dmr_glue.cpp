/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The records the DMR package reads from the radio (dmr_glue.h) and the two
 * pieces of arithmetic our glue adds: the transmit air delay and the SMS
 * format mapping.
 *
 * The air delay is the one number that has to be right for a call through a
 * repeater to land in the operator's slot.  The default is the measured
 * constant of this audio path (30 ms of output buffer + 10 ms of DSP
 * transport, dmr_glue.c); the trim is the operator's bench calibration.  The
 * pre-roll is that delay modulo one 60 ms TDMA frame, in 48 kHz samples.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

extern "C" {
#include "protocols/DMR/dmr_glue.h"
#include "protocols/DMR/dmr_sms.h"
}

TEST_CASE("The records start usable", "[dmr-glue]")
{
    dmrGlue_reset();

    REQUIRE(c62_dmr.id == 0);        /* no DMR ID until the operator sets one */
    REQUIRE(c62_dmr.cc == 0);
    REQUIRE(c62_dmr.slot == 1);
    REQUIRE(c62_dmr.tg == 0);
    REQUIRE_FALSE(c62_dmr.privateCall);
    REQUIRE_FALSE(c62_dmr.txFlip);
    REQUIRE_FALSE(c62_dmr.txAirDelayAuto);

    REQUIRE(c62_t3.mode == C62_T3_OFF);
    REQUIRE(c62_t3.first == 1);
    REQUIRE_FALSE(c62_t3.followAll);
}

TEST_CASE("The air delay defaults to the measured 40 ms", "[dmr-glue]")
{
    dmrGlue_reset();

    /* 30 ms of output stream + 10 ms of DSP transport: dmr_glue.c */
    REQUIRE(dmrGlue_airDelayUs() == 40000);

    /* 40000 us = 1920 samples at 48 kHz, below the 2880-sample frame */
    REQUIRE(dmrGlue_preRollSamples() == 1920);
}

TEST_CASE("The pre-roll is the delay modulo one TDMA frame", "[dmr-glue]")
{
    dmrGlue_reset();

    dmrGlue_setAirDelayUs(0);
    REQUIRE(dmrGlue_preRollSamples() == 0);

    /* One slot */
    dmrGlue_setAirDelayUs(30000);
    REQUIRE(dmrGlue_preRollSamples() == 1440);

    /* Exactly one frame: nothing to wait for */
    dmrGlue_setAirDelayUs(60000);
    REQUIRE(dmrGlue_preRollSamples() == 0);

    /* A quarter of a frame (the trim clamps at 60 ms, dmr_glue.c) */
    dmrGlue_setAirDelayUs(15000);
    REQUIRE(dmrGlue_preRollSamples() == 720);
}

TEST_CASE("The trim stays inside what the radio can delay", "[dmr-glue]")
{
    dmrGlue_reset();

    dmrGlue_setAirDelayUs(-500);
    REQUIRE(dmrGlue_airDelayUs() == 0);

    dmrGlue_setAirDelayUs(0x7FFFFFFF);
    REQUIRE(dmrGlue_airDelayUs() == 60000);
    REQUIRE(dmrGlue_preRollSamples() < 2880u);

    dmrGlue_setAirDelayUs(12500);
    REQUIRE(dmrGlue_airDelayUs() == 12500);
    REQUIRE(dmrGlue_preRollSamples() == 600);
}
