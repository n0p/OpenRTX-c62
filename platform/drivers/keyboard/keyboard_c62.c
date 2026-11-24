/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <interfaces/keyboard.h>
#include <zephyr/drivers/gpio.h>
#include <interfaces/delays.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(keyboard, LOG_LEVEL_DBG);
#include <zephyr/device.h>

#define ROW1_NODE DT_NODELABEL(row1)
#define ROW2_NODE DT_NODELABEL(row2)
#define ROW3_NODE DT_NODELABEL(row3)
#define ROW4_NODE DT_NODELABEL(row4)
#define COL1_NODE DT_NODELABEL(col1)
#define COL2_NODE DT_NODELABEL(col2)
#define COL3_NODE DT_NODELABEL(col3)
#define COL4_NODE DT_NODELABEL(col4)
#define COL5_NODE DT_NODELABEL(col5)

static const struct gpio_dt_spec row1 = GPIO_DT_SPEC_GET(ROW1_NODE, gpios);
static const struct gpio_dt_spec row2 = GPIO_DT_SPEC_GET(ROW2_NODE, gpios);
static const struct gpio_dt_spec row3 = GPIO_DT_SPEC_GET(ROW3_NODE, gpios);
static const struct gpio_dt_spec row4 = GPIO_DT_SPEC_GET(ROW4_NODE, gpios);
static const struct gpio_dt_spec col1 = GPIO_DT_SPEC_GET(COL1_NODE, gpios);
static const struct gpio_dt_spec col2 = GPIO_DT_SPEC_GET(COL2_NODE, gpios);
static const struct gpio_dt_spec col3 = GPIO_DT_SPEC_GET(COL3_NODE, gpios);
static const struct gpio_dt_spec col4 = GPIO_DT_SPEC_GET(COL4_NODE, gpios);
static const struct gpio_dt_spec col5 = GPIO_DT_SPEC_GET(COL5_NODE, gpios);

/*
 * The C62 keypad is a 4 x 5 matrix: the four row lines are driven low one at a
 * time and the five column lines are read with their pull-ups, as the stock
 * firmware does (keypad_scan @ 0x1801b72c, row table 0x1809e56c, column table
 * 0x1809e578; see forks/c62-analysis/reconstructed/drivers/keypad.c and the
 * schematic C62_Teardown/Schematics/C62_KEYPAD.pdf).
 *
 * The device tree names of the two groups match the stock ones, but the column
 * order is not the stock one, so it is fixed here: stock col0..col4 are
 * PA14, PB11, PA17, PA19, PB6.
 *
 * The key index is the stock one: idx = 2 + row * 5 + col, with keyMap[]
 * holding keys 2..21 in index - 2. Driving the columns and reading the rows
 * (what this driver did before) is the transpose of the matrix: it only sees
 * the eight positions where the two groups cross and reports the wrong key for
 * them, so ENTER (row 0, col 0) never arrived at the UI.
 */
static const struct gpio_dt_spec *rows[] = { &row1, &row2, &row3, &row4 };
static const struct gpio_dt_spec *cols[] = { &col1, &col5, &col2, &col3,
                                             &col4 };

#define SIDEKEY_NODE DT_NODELABEL(sidekey)

static const struct gpio_dt_spec sidekey = GPIO_DT_SPEC_GET_OR(SIDEKEY_NODE,
                                                               gpios, { 0 });

/*
 * One entry per matrix position, row-major from the stock index 2:
 *
 *   row 0 (PB9)   MENU  P1  3  7  0
 *   row 1 (PA16)  UP    P2  4  8  *
 *   row 2 (PB10)  DOWN  1   5  9  -
 *   row 3 (PA20)  A/B   2   6  #  -
 *
 * MENU is OpenRTX's ENTER (it opens the menu and confirms input), A/B is ESC
 * (it backs out of every menu on the stock firmware), P1 is row 0 col 1 and P2
 * is row 1 col 1: the two programmable keys of the second column, which sit
 * left of the "3" and the "4". The positions marked "-" have no switch.
 */
static const uint32_t keyMap[] = { KEY_ENTER, KEY_F1, KEY_3,   KEY_7, KEY_0,
                                   KEY_UP,    KEY_F2, KEY_4,   KEY_8, KEY_STAR,
                                   KEY_DOWN,  KEY_1,  KEY_5,   KEY_9, 0,
                                   KEY_ESC,   KEY_2,  KEY_6,   KEY_HASH, 0 };

#define KEYMAP_SIZE (sizeof(keyMap) / sizeof(keyMap[0]))


void kbd_init()
{
}

void kbd_terminate()
{
}

keyboard_t kbd_getKeys()
{
    uint32_t keys = 0;

    // Configure the four row lines as outputs, all high (inactive)
    for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
        gpio_pin_configure_dt(rows[i], GPIO_OUTPUT);
        gpio_pin_set_dt(rows[i], 0);
    }

    // Configure the five column lines as inputs with pull-ups
    for (size_t i = 0; i < ARRAY_SIZE(cols); i++) {
        gpio_pin_configure_dt(cols[i], GPIO_INPUT | GPIO_PULL_UP);
    }

    // Drive one row low at a time and read the columns. Both groups are
    // active low in the device tree, so gpio_pin_get_dt() returns 1 for a
    // column connected to the driven row through a pressed switch.
    for (size_t r = 0; r < ARRAY_SIZE(rows); r++) {
        gpio_pin_set_dt(rows[r], 1);

        // Small delay to let the line settle
        k_busy_wait(1);

        for (size_t c = 0; c < ARRAY_SIZE(cols); c++) {
            if (gpio_pin_get_dt(cols[c]) == 1)
                keys |= (1UL << (r * ARRAY_SIZE(cols) + c));
        }

        gpio_pin_set_dt(rows[r], 0);
    }

    // Map raw keys to KEY_ enum values
    keyboard_t result = 0;

    for (size_t i = 0; i < KEYMAP_SIZE; i++) {
        if (keys & (1UL << i))
            result |= keyMap[i];
    }

    // The side key is the secondary PTT, active low with a pull-up
    gpio_pin_configure_dt(&sidekey, GPIO_INPUT);
    if (gpio_pin_get_dt(&sidekey) == 1)
        result |= KEY_MONI;

    /*
     * Log the raw matrix word on change: the one diagnostic that says whether
     * a key is wired where keyMap[] claims. Enable with
     * CONFIG_LOG_RUNTIME_FILTERING and `log_filter_set(keyboard, 4, LOG_LEVEL_DBG)`.
     */
    static uint32_t prior;
    if (keys != prior) {
        LOG_DBG("matrix 0x%05x -> keys 0x%08x", keys, result);
        prior = keys;
    }

    return result;
}
