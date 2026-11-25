/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EMULATOR_H
#define EMULATOR_H

#include "interfaces/keyboard.h"
#include <stdbool.h>
#include <stdint.h>
#include "SDL2/SDL.h"

#ifndef CONFIG_SCREEN_WIDTH
#define CONFIG_SCREEN_WIDTH 160
#endif

#ifndef CONFIG_SCREEN_HEIGHT
#define CONFIG_SCREEN_HEIGHT 128
#endif

enum choices {
    VAL_RSSI = 1,
    VAL_BAT,
    VAL_MIC,
    VAL_VOL,
    VAL_CH,
    VAL_PTT,
    PRINT_STATE,
    EXIT
};

typedef struct {
    float RSSI;
    float vbat;
    float micLevel;
    float volumeLevel;
    float chSelector;
    bool PTTstatus;
    bool powerOff;
} emulator_state_t;

extern emulator_state_t emulator_state;

void emulator_start();

keyboard_t emulator_getKeys();

/**
 * Translate a key name ("UP", "GREEN", "P1", "2", "KNOB_LEFT") to its bit.
 *
 * Accepts the name with or without its KEY_ prefix, case insensitively, and
 * the physical names of this radio: GREEN, RED, P1, P2 and SIDE, which are
 * KEY_ENTER, KEY_ESC, KEY_F1, KEY_F2 and KEY_MONI.
 *
 * @return the keyboard_t bit, or 0 when the name is unknown.
 */
keyboard_t emulator_keyFromName(const char *name);

/**
 * Queue one keyboard_t state for the firmware's key scan.
 *
 * The queue is consumed one entry per kbd_getKeys() call, so a press and a
 * release are two entries: `key`, then 0.
 *
 * @return false when the queue is full.
 */
bool emulator_keyEnqueue(keyboard_t keys);

/** Entries still waiting in the key queue. */
int emulator_keyPending(void);

#endif /* EMULATOR_H */
