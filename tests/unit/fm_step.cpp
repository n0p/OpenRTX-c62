/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tuning step and band limits, for the analogue modes.
 *
 * The step is one table for the whole radio (`freq_steps`, core/state.c) and one
 * index (`state.step_index`); what changes per band is which entry of it makes
 * sense, and the firmware decides that in three places:
 *
 *  - the airband mode has a raster (25 kHz, or 8.33 kHz above FL195) and it puts
 *    the global step on it whenever an airband channel or the mode is selected,
 *    so the VFO never steps by whatever the previous mode left behind
 *    (qm_am_set_step, qm_am_applyRaster);
 *  - with the 8.33 kHz step, stepping by a whole number of hertz drifts off the
 *    carrier, which is a third of a kilohertz away from the designator printed
 *    on the chart; the snap moves each step back onto the nearest carrier, and
 *    it is only on for the airband mode (ui.c, `_ui_airbandSnap`);
 *  - every step is bounded by the radio's own band table (hwInfo), while the
 *    band a *service* uses is not a limit: the operator may tune just outside
 *    118-137 MHz, and the airband mode is then what refuses to transmit.
 *
 * Everything here goes through the UI FSM with key presses, which is the same
 * path the operator's finger takes.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

extern "C" {
#include "core/state.h"
#include "core/input.h"
#include "core/event.h"
#include "core/ui.h"
#include "core/airband.h"
#include "core/voicePrompts.h"
#include "interfaces/platform.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
#include "ui/ui_modes.h"
#include "ui/ui_quickmenu.h"
}

extern state_t state;

namespace
{

constexpr freq_t AIR_FREQ = 118000000;

void press(uint32_t keys, bool isLong = false)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = isLong ? 1 : 0;

    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

void resetUi(uint8_t mode, freq_t freq)
{
    state.channel.mode          = mode;
    state.channel.rx_frequency  = freq;
    state.channel.tx_frequency  = freq;
    state.channel.bandwidth     = BW_25;
    state.channel.power         = 1000;
    state.channel.rx_only       = false;
    state.ui_screen             = MAIN_VFO;
    state.tuner_mode            = VFO;
    state.txDisable             = false;
    state.charge                = 100;
    state.emergency             = false;
    state.step_index            = 4;            /* 12.5 kHz */
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;       /* silent in the harness */
    state.settings.am_step      = false;
    state.settings.am_snap      = true;
}

/// Index of a step in the table, -1 when the radio does not offer it.
int stepIndex(uint32_t hz)
{
    for (size_t i = 0; i < n_freq_steps; i++) {
        if (freq_steps[i] == hz)
            return (int)i;
    }
    return -1;
}

} /* namespace */

TEST_CASE("the step table covers the rasters and repeats nothing",
          "[step][ui]")
{
    REQUIRE(n_freq_steps > 0);

    /*
     * A step of zero would be a tuning key that does nothing, and two equal
     * entries would be a menu row that changes nothing. The table is not
     * sorted: 8.33 kHz is appended so that the airband raster is the last row
     * of the step list, which is why this checks uniqueness and not order.
     */
    for (size_t i = 0; i < n_freq_steps; i++) {
        INFO("step " << i);
        REQUIRE(freq_steps[i] > 0);

        for (size_t j = 0; j < i; j++)
            REQUIRE(freq_steps[i] != freq_steps[j]);
    }

    /* The three the analogue modes are specified against: the airband 25 kHz
     * raster, its 8.33 kHz channel, and the 12.5 kHz of the amateur bands */
    REQUIRE(stepIndex(25000) >= 0);
    REQUIRE(stepIndex(8333) >= 0);
    REQUIRE(stepIndex(12500) >= 0);

    /*
     * The boot default is index 4, and that has to be 12.5 kHz (core/state.c,
     * state_init): reordering this table would otherwise change the step a
     * fresh radio tunes by without anybody noticing.
     */
    REQUIRE(4 < n_freq_steps);
    REQUIRE(freq_steps[4] == 12500);
}

TEST_CASE("selecting the airband raster puts the VFO step on it",
          "[step][airband][ui]")
{
    resetUi(OPMODE_AIRBAND, AIR_FREQ);

    /* 25 kHz: the classic plan */
    qm_am_set_step(0);
    REQUIRE(qm_am_get_step() == 0);
    REQUIRE(state.settings.am_step == 0);
    REQUIRE(freq_steps[state.step_index] == 25000);

    /* 8.33 kHz: three channels per 25 kHz slot */
    qm_am_set_step(1);
    REQUIRE(qm_am_get_step() == 1);
    REQUIRE(state.settings.am_step == 1);
    REQUIRE(freq_steps[state.step_index] == 8333);

    /*
     * Coming back into an airband channel restores the operator's raster: the
     * step is global, so the previous mode may have left 12.5 kHz behind.
     */
    state.step_index = (uint8_t)stepIndex(12500);
    qm_am_applyRaster();
    REQUIRE(freq_steps[state.step_index] == 8333);

    state.settings.am_step = 0;
    state.step_index = (uint8_t)stepIndex(12500);
    qm_am_applyRaster();
    REQUIRE(freq_steps[state.step_index] == 25000);

    /*
     * And it is the airband's raster, not a global one: an FM channel leaves
     * the step the operator set alone.
     */
    state.channel.mode = OPMODE_FM;
    state.step_index = (uint8_t)stepIndex(12500);
    qm_am_applyRaster();
    REQUIRE(freq_steps[state.step_index] == 12500);
}

TEST_CASE("stepping moves by the step and stops at the band edge",
          "[step][limits][ui]")
{
    const hwInfo_t *hw = platform_getHwInfo();
    REQUIRE(hw->vhf_band == 1);

    resetUi(OPMODE_FM, 145500000);
    state.step_index = (uint8_t)stepIndex(12500);

    /* Up one step, down two: the arithmetic of the key, not of the mode */
    press(KEY_UP);
    REQUIRE(state.channel.rx_frequency == 145512500);
    REQUIRE(state.channel.tx_frequency == 145512500);

    press(KEY_DOWN);
    press(KEY_DOWN);
    REQUIRE(state.channel.rx_frequency == 145487500);
    REQUIRE(state.channel.tx_frequency == 145487500);

    /* A coarser step moves by exactly that much */
    state.step_index = (uint8_t)stepIndex(25000);
    press(KEY_UP);
    REQUIRE(state.channel.rx_frequency == 145512500);

    /*
     * The band table of the platform is the hard limit: at the top of the
     * highest band a step up is refused and the frequency stays where it is,
     * so the radio can never be tuned to a frequency the front end has no
     * calibration for.
     */
    const freq_t top = (freq_t)hw->vhf_maxFreq * 1000000U;
    resetUi(OPMODE_FM, top);
    press(KEY_UP);
    REQUIRE(state.channel.rx_frequency == top);

    const freq_t bottom = (freq_t)hw->vhf_minFreq * 1000000U;
    resetUi(OPMODE_FM, bottom);
    press(KEY_DOWN);
    REQUIRE(state.channel.rx_frequency == bottom);

    /* One step inside the edge is still a step */
    resetUi(OPMODE_FM, top - 25000);
    state.step_index = (uint8_t)stepIndex(25000);
    press(KEY_UP);
    REQUIRE(state.channel.rx_frequency == top);
}

TEST_CASE("the airband 8.33 kHz step lands on a carrier, not between two",
          "[step][airband][raster][ui]")
{
    /*
     * The stepping happens where the host's band table has room for it: the
     * emulator reports a VHF band starting at 136 MHz (the C62 in the lab tunes
     * from 18 MHz, platform/targets/c62/platform.c), and 136.000 MHz is channel
     * 2160 of the raster, so a hundred steps fit inside the band.
     */
    const uint16_t first = 2160;
    const freq_t   base  = 136000000;

    REQUIRE(airband_channelFreq(first) == base);

    resetUi(OPMODE_AIRBAND, base);
    qm_am_set_step(1);
    state.settings.am_snap = true;

    /*
     * A hundred steps of 8333 Hz are 833300 Hz, and a hundred channels of the
     * raster are 833333 Hz: without the snap the receiver would be 33 Hz -- and
     * after a thousand steps a third of a kilohertz -- off the carrier it is
     * supposed to be on.
     */
    for (int i = 0; i < 100; i++)
        press(KEY_UP);

    INFO("rx " << state.channel.rx_frequency);
    REQUIRE(state.channel.rx_frequency == airband_channelFreq(first + 100));
    REQUIRE(airband_channelIndex(state.channel.rx_frequency) == first + 100);

    /* And back down again to where it started, still on a carrier */
    for (int i = 0; i < 100; i++)
        press(KEY_DOWN);

    REQUIRE(state.channel.rx_frequency == base);
    REQUIRE(airband_channelIndex(state.channel.rx_frequency) == first);

    /*
     * With the snap off, the same hundred steps end 33 Hz off the channel:
     * which is the drift the snap exists to remove, and the reason it is on by
     * default for the band.
     */
    resetUi(OPMODE_AIRBAND, base);
    qm_am_set_step(1);
    state.settings.am_snap = false;

    for (int i = 0; i < 100; i++)
        press(KEY_UP);

    INFO("rx " << state.channel.rx_frequency);
    REQUIRE(state.channel.rx_frequency == base + 100 * 8333U);
    REQUIRE(state.channel.rx_frequency != airband_channelFreq(first + 100));

    /* The 25 kHz raster needs no snap: 25 kHz is exact */
    resetUi(OPMODE_AIRBAND, base);
    qm_am_set_step(0);
    state.settings.am_snap = false;

    for (int i = 0; i < 39; i++)
        press(KEY_UP);

    REQUIRE(state.channel.rx_frequency == 136975000);
    REQUIRE(airband_channelIndex(state.channel.rx_frequency)
            == AIRBAND_CHANNELS_8K33 - 1);
}

TEST_CASE("a designator typed by hand becomes the carrier it names",
          "[step][airband][raster][ui]")
{
    /*
     * 136.805 is a designator, not a frequency: its carrier is 136.80833. The
     * VFO entry snaps it when the band's snap is on, and leaves exactly what
     * was typed when it is off. (Typed where the host's band table reaches --
     * see the case above.)
     */
    /* The band takes one frequency in this build, so the eighth digit applies */
    REQUIRE(opmode_canTransmit(OPMODE_AIRBAND) == false);

    resetUi(OPMODE_AIRBAND, 136000000);
    state.settings.am_snap = true;

    press(KEY_1);
    press(KEY_3);
    press(KEY_6);
    press(KEY_8);
    press(KEY_0);
    press(KEY_5);
    press(KEY_0);
    press(KEY_0);

    /* The airband is receive only in this build: eight digits apply it */
    REQUIRE(state.channel.rx_frequency == 136808333);
    REQUIRE(state.channel.tx_frequency == state.channel.rx_frequency);
    REQUIRE(airband_channelIndex(state.channel.rx_frequency) == 2257);

    resetUi(OPMODE_AIRBAND, 136000000);
    state.settings.am_snap = false;

    press(KEY_1);
    press(KEY_3);
    press(KEY_6);
    press(KEY_8);
    press(KEY_0);
    press(KEY_5);
    press(KEY_0);
    press(KEY_0);

    REQUIRE(state.channel.rx_frequency == 136805000);

    /*
     * The same digits on an FM channel are taken as typed: the snap is the
     * airband's, and nothing else in the radio rounds a frequency off.
     */
    resetUi(OPMODE_FM, 145500000);
    state.settings.am_snap = true;

    press(KEY_1);
    press(KEY_4);
    press(KEY_5);
    press(KEY_5);
    press(KEY_0);
    press(KEY_0);
    press(KEY_1);
    press(KEY_2);
    press(KEY_ENTER);   /* transmit modes ask for the TX field: copy RX */

    REQUIRE(state.channel.rx_frequency == 145500120);
}

TEST_CASE("the service band is not a tuning limit, only a transmit one",
          "[step][airband][limits][ui]")
{
    /*
     * 118-137 MHz is the airband service's band, not the radio's: the operator
     * can tune the VFO just outside it (the front end works, the band plan is
     * their responsibility, docs/UI.md), and it is the mode that refuses to
     * transmit there. Both halves are asserted, because the pair is the design:
     * no silent clamp of the frequency, no transmit outside the band.
     */
    resetUi(OPMODE_AIRBAND, airband_channelFreq(AIRBAND_CHANNELS_8K33 - 1));
    qm_am_set_step(0);
    state.settings.am_snap = true;

    /* The top channel, then one step past the top of the band */
    REQUIRE(state.channel.rx_frequency == 136975000);
    press(KEY_UP);
    REQUIRE(state.channel.rx_frequency == 137000000);
    REQUIRE_FALSE(airband_inBand(state.channel.rx_frequency));

    /* The radio still tunes there: the band table is what limits tuning */
    const hwInfo_t *hw = platform_getHwInfo();
    REQUIRE(state.channel.rx_frequency
            <= (freq_t)hw->vhf_maxFreq * 1000000U);

    /* And the airband transmitter refuses that frequency */
    airband_txSetState(AIRBAND_TX_ARMED);
    REQUIRE_FALSE(airband_txAllowed(state.channel.rx_frequency));
    REQUIRE(airband_txAllowed(136975000));
    airband_txSetState(AIRBAND_TX_LOCKED);

    /* Back inside the band it is a normal channel again */
    press(KEY_DOWN);
    REQUIRE(state.channel.rx_frequency == 136975000);
    REQUIRE(airband_inBand(state.channel.rx_frequency));
    REQUIRE(airband_channelIndex(state.channel.rx_frequency)
            == AIRBAND_CHANNELS_8K33 - 1);
}
