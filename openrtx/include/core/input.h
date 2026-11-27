/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INPUT_H
#define INPUT_H

#include "interfaces/keyboard.h"
#include <inttypes.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Time interval in milliseconds after which a keypress is considered a long-press
 */
static const uint16_t input_longPressTimeout = 700;

/**
 * Structure that represents a keyboard event payload
 * The maximum size of an event payload is 30 bits
 * For a keyboard event we use 1 bit to signal a short or long press
 * And the remaining 29 bits to communicate currently pressed keys.
 */
typedef union {
    struct {
        uint32_t long_press : 1;
        uint32_t keys       : 29;
        uint32_t _padding   : 2;
    };

    uint32_t value;
} kbd_msg_t;

/**
 * Scan all the keyboard buttons to detect possible keypresses filling a
 * keyboard event data structure. The function returns true if a keyboard event
 * has been detected.
 *
 * @param msg: keyboard event message to be filled.
 * @return true if a keyboard event has been detected, false otherwise.
 */
bool input_scanKeyboard(kbd_msg_t *msg);

/**
 * True if the event carries a number key that has just been pressed.
 *
 * input_scanKeyboard() reports the press edge and, 700 ms later, a second
 * event for the same key with long_press set. The long press is a repeat of a
 * keystroke already delivered, not another one: a screen that types a digit
 * has to see it once, or holding a key enters it twice (the frequency, the
 * offset and the DMR ID entries all did). Screens that give the long press a
 * meaning of its own test msg.long_press before asking.
 *
 * @param msg: the keyboard queue message
 * @return true if a number key was pressed by this event
 */
bool input_isNumberPressed(kbd_msg_t msg);

/**
 * True if the event carries a character key that has just been pressed.
 *
 * Same rule as input_isNumberPressed(): the long-press repeat is not a second
 * keystroke.
 *
 * @param msg: the keyboard queue message
 * @return true if a character key was pressed by this event
 */
bool input_isCharPressed(kbd_msg_t msg);

/**
 * This function returns the smallest number that is pressed on the keyboard,
 * 0 if none is pressed.
 *
 * @param msg: the keyboard queue message
 * @return the smalled pressed number on the keyboard
 */
uint8_t input_getPressedNumber(kbd_msg_t msg);

/**
 * This function returns the smallest number pressed on the keyboard and
 * associated to character. If no button is pressed, zero is returned.
 *
 * @param msg: the keyboard queue message
 * @return the smallest number associated to a char.
 */
uint8_t input_getPressedChar(kbd_msg_t msg);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* INPUT_H */
