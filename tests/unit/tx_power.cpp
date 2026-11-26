/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * What the radio transmits with, on the two shapes it can boot as.
 *
 * The failure these tests lock down was reported from a radio, not a desk: with
 * no memory channel selected -- the VFO, which is what a fresh bank boots into
 * -- the PTT keyed nothing.  No RF, no LED, no error.  Two fields were to
 * blame, both of them taken from state.channel, which a VFO record leaves at
 * zero:
 *
 *   - tx_frequency 0: the C62 driver refuses an out-of-band transmit frequency
 *     (136-600 MHz) with a bare return, before the PA enable lines and the TX
 *     LED are driven.
 *   - power 0: the PA comes up with nothing behind it, and the same press
 *     looks identical from the outside.
 *
 * state_txFrequency() and state_txPowerMw() are the single place that decides
 * both now (docs/TX_SAFETY.md), and these tests are the contract: a
 * transmission always has a frequency in band and never has a power of zero
 * unless the operator asked for exactly that, in which case the gate refuses
 * instead of transmitting into nothing.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>

extern "C" {
#include "core/settings.h"
#include "core/state.h"
#include "rtx/rtx.h"
}

#include <pthread.h>

extern state_t state;

/* The window radio_C62.cpp will key in: outside it the driver refuses. */
static constexpr uint32_t TX_FREQ_MIN_HZ = 136000000u;
static constexpr uint32_t TX_FREQ_MAX_HZ = 600000000u;

/** The VFO as a record written by the previous firmware leaves it. */
static void loadBlankVfo()
{
    std::memset(&state.channel, 0, sizeof(state.channel));
    state.channel.mode = OPMODE_FM;
    state.channel.bandwidth = BW_25;
    state.channel.rx_frequency = 145500000;
    state.channel.tx_frequency = 0;
    state.channel.power = 0;
    state.settings.tx_power = 500;
}

TEST_CASE("VFO: transmit frequency follows the receiver", "[tx]")
{
    loadBlankVfo();

    REQUIRE(state_txFrequency() == 145500000);
    REQUIRE(state_txFrequency() >= TX_FREQ_MIN_HZ);
    REQUIRE(state_txFrequency() <= TX_FREQ_MAX_HZ);
}

TEST_CASE("VFO: transmit power comes from the settings", "[tx]")
{
    loadBlankVfo();

    REQUIRE(state_txPowerMw() == 500);
    REQUIRE(state_keyingPowerOk());
}

TEST_CASE("Channel: its own frequency and power win", "[tx]")
{
    std::memset(&state.channel, 0, sizeof(state.channel));
    state.channel.mode = OPMODE_FM;
    state.channel.rx_frequency = 145500000;
    state.channel.tx_frequency = 145100000; /* a repeater's input */
    state.channel.power = 2000;
    state.settings.tx_power = 500;

    REQUIRE(state_txFrequency() == 145100000);
    REQUIRE(state_txPowerMw() == 2000);
    REQUIRE(state_keyingPowerOk());
}

TEST_CASE("A power of zero is kept, and the gate refuses it", "[tx]")
{
    /*
     * The operator's own "do not transmit": the setting is stored as asked,
     * nothing clamps it to a minimum, and the answer is a refusal with a
     * reason -- never a keying at 0 mW and never a silent minimum.
     */
    loadBlankVfo();
    state.settings.tx_power = 0;

    REQUIRE(state_txPowerMw() == 0);
    REQUIRE_FALSE(state_keyingPowerOk());

    static pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;
    rtx_init(&cfgMutex);

    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.txDisable = 0;
    cfg.txPower = 0;
    REQUIRE_FALSE(rtx_keyingAllowed(&cfg));
    REQUIRE(std::strcmp(rtx_keyingVetoReason(), "no tx power") == 0);

    /* With power the veto is not this one: whatever the gate says about the
     * press, "there is nothing to transmit with" is no longer the answer. */
    cfg.txPower = 500;
    (void)rtx_keyingAllowed(&cfg);
    REQUIRE(std::strcmp(rtx_keyingVetoReason(), "no tx power") != 0);
}

TEST_CASE("The whole VFO keying route has something to drive", "[tx]")
{
    /*
     * The three conditions the driver needs, checked together and in the order
     * it checks them: a transmit frequency it will accept, a power that is not
     * zero, and a receiver on a real frequency.  This is the case that would
     * have failed before the fix, on a radio with an empty bank.
     */
    loadBlankVfo();

    const uint32_t tx = state_txFrequency();
    REQUIRE(tx >= TX_FREQ_MIN_HZ);
    REQUIRE(tx <= TX_FREQ_MAX_HZ);
    REQUIRE(state_txPowerMw() != 0);
    REQUIRE(state_keyingPowerOk());
    REQUIRE(state.channel.rx_frequency != 0);
}
