/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * VFO frequency entry, driven through the UI FSM.
 *
 * The C62 entry takes eight digits, 10 Hz resolution ("446.00625"), and in a
 * receive-only mode (TETRA, DMR) there is a single frequency: the FSM must
 * never ask for a TX one.
 */

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "core/state.h"
#include "core/input.h"
#include "core/event.h"
#include "core/ui.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
}

extern state_t state;

static void press(uint32_t key)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = key;
    ui_pushEvent(EVENT_KBD, msg.value);
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

/**
 * The same key, 700 ms later, as the keypad reports a key that is still down.
 *
 * The press edge has already been delivered by press(); this is the repeat.
 */
static void pressLong(uint32_t key)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = key;
    msg.long_press = 1;
    ui_pushEvent(EVENT_KBD, msg.value);
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

static void resetUi(uint8_t mode, freq_t freq)
{
    state.channel.mode = mode;
    state.channel.rx_frequency = freq;
    state.channel.tx_frequency = freq;
    state.channel.bandwidth = BW_25;
    state.channel.power = 1000;
    state.ui_screen = MAIN_VFO;
    state.tuner_mode = VFO;
    state.txDisable = false;
    state.unattendedTxDisable = false;
    state.charge = 100;
    state.emergency = false;
    state.settings = default_settings;
}

TEST_CASE("VFO entry takes eight digits of 10 Hz", "[ui]")
{
    resetUi(OPMODE_FM, 430000000);

    /* 446.00625 MHz: the first digit opens the input screen */
    press(KEY_4);
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);

    press(KEY_4);
    press(KEY_6);
    press(KEY_0);
    press(KEY_0);
    press(KEY_6);
    press(KEY_2);
    press(KEY_5);

    /* A transmitting mode now asks for the TX frequency, same screen */
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);
    REQUIRE(state.channel.rx_frequency == 430000000);

    /* ENTER with nothing typed for TX copies RX and applies both */
    press(KEY_ENTER);
    INFO("rx " << state.channel.rx_frequency << " tx "
         << state.channel.tx_frequency << " screen " << state.ui_screen);
    REQUIRE(state.channel.rx_frequency == 446006250);
    REQUIRE(state.channel.tx_frequency == 446006250);
    REQUIRE(state.ui_screen == MAIN_VFO);
}

TEST_CASE("VFO entry in a receive only mode has one frequency", "[ui]")
{
    REQUIRE(opmode_canTransmit(OPMODE_TETRA) == false);

    resetUi(OPMODE_TETRA, 430000000);

    /* 440.01295 MHz, eight digits */
    press(KEY_4);
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);
    press(KEY_4);
    press(KEY_0);
    press(KEY_0);
    press(KEY_1);
    press(KEY_2);
    press(KEY_9);
    press(KEY_5);

    /* The eighth digit applies it: no TX field, back to the main screen */
    INFO("rx " << state.channel.rx_frequency << " tx "
         << state.channel.tx_frequency << " screen " << state.ui_screen);
    REQUIRE(state.channel.rx_frequency == 440012950);
    REQUIRE(state.channel.tx_frequency == state.channel.rx_frequency);
    REQUIRE(state.ui_screen == MAIN_VFO);
}

TEST_CASE("VFO entry rejects a frequency outside the tuning range", "[ui]")
{
    resetUi(OPMODE_FM, 430000000);

    /* 1.00000 MHz: outside every band of the radio */
    press(KEY_0);
    press(KEY_0);
    press(KEY_1);
    press(KEY_0);
    press(KEY_0);
    press(KEY_0);
    press(KEY_0);
    press(KEY_0);

    press(KEY_ENTER);

    /* Still in the input screen so the digits can be corrected */
    INFO("screen " << state.ui_screen << " rx " << state.channel.rx_frequency);
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);
    REQUIRE(state.channel.rx_frequency == 430000000);

    /* The field is full: more digits are ignored, not shifted into the freq */
    press(KEY_9);
    press(KEY_9);
    REQUIRE(state.channel.rx_frequency == 430000000);
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);

    /* ESC leaves without touching the tuned frequency */
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.channel.rx_frequency == 430000000);
}

TEST_CASE("a digit held down enters one digit, not two", "[ui][input]")
{
    resetUi(OPMODE_FM, 430000000);

    /*
     * input_scanKeyboard() reports the press and, 700 ms later, the same key
     * with long_press set.  Holding a key is not rare on a keypad this size,
     * and the frequency entry counted both events: the digit went in twice and
     * every digit typed after it landed one place further along, so the
     * frequency that was applied was not the one on screen.
     */
    press(KEY_4);                     /* 4: opens the input screen */
    press(KEY_4);
    press(KEY_6);
    pressLong(KEY_6);                 /* the same 6, still held */
    press(KEY_0);
    press(KEY_0);
    press(KEY_6);
    press(KEY_2);
    press(KEY_5);

    /* ENTER closes the TX field with nothing typed, copying RX */
    press(KEY_ENTER);

    INFO("screen " << state.ui_screen << " rx " << state.channel.rx_frequency);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.channel.rx_frequency == 446006250);
}
