/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Zone browser: nomenclature of the R/U/PMR channels, the 500 mW mark, the two
 * screens (zones then entries) and the tuning of an entry. The tables come
 * from a test source here; the browser itself never invents a frequency, it
 * draws whatever the model hands it.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "core/state.h"
#include "core/input.h"
#include "ui/ui_default.h"
#include "ui/ui_zones.h"
#include "ui/ui_layout.h"
}

extern "C" state_t state;

/* A small stand-in for the model's zone tables: R0..R7, U0..U4, PMR1..PMR3 */
static ui_zone_entry_t rEntries[8];
static ui_zone_entry_t uEntries[5];
static ui_zone_entry_t pmrEntries[3];

static const char *testName(uint8_t zone)
{
    static const char *const names[] = { "R", "U", "PMR" };
    return (zone < 3) ? names[zone] : NULL;
}

static uint16_t testSize(uint8_t zone)
{
    switch (zone) {
        case 0: return 8;
        case 1: return 5;
        case 2: return 3;
        default: return 0;
    }
}

static const ui_zone_entry_t *testAt(uint8_t zone, uint16_t index)
{
    if (index >= testSize(zone))
        return NULL;

    switch (zone) {
        case 0: return &rEntries[index];
        case 1: return &uEntries[index];
        default: return &pmrEntries[index];
    }
}

static const ui_zones_source_t testSource = { 3, testName, testSize, testAt };

static kbd_msg_t key(uint32_t keys)
{
    kbd_msg_t m;
    m.value = 0;
    m.keys = keys;
    m.long_press = false;
    return m;
}

static void press(uint32_t keys, bool *sync)
{
    kbd_msg_t m = key(keys);
    REQUIRE(ui_zones_isOpen());
    ui_zones_handleEvent(&m, sync);
}

static void buildTables(void)
{
    for (uint16_t i = 0; i < 8; i++) {
        memset(&rEntries[i], 0, sizeof(rEntries[i]));
        rEntries[i].rx_hz = 145500000UL + i * 25000UL;
        rEntries[i].tx_hz = 145500000UL + i * 25000UL;
        rEntries[i].power_mw = 5000;
    }

    for (uint16_t i = 0; i < 5; i++) {
        memset(&uEntries[i], 0, sizeof(uEntries[i]));
        uEntries[i].rx_hz = 430000000UL + i * 1000000UL;
        uEntries[i].tx_hz = uEntries[i].rx_hz;
        uEntries[i].power_mw = 1000;
    }

    for (uint16_t i = 0; i < 3; i++) {
        memset(&pmrEntries[i], 0, sizeof(pmrEntries[i]));
        pmrEntries[i].rx_hz = 446006250UL + i * 12500UL;
        pmrEntries[i].power_mw = 500;          /* the PMR table is 500 mW */
    }
}

TEST_CASE("Zone nomenclature follows the repeater and PMR grids",
          "[ui][zones]")
{
    char buf[12];

    ui_zones_label("R", 0, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "R0") == 0);
    ui_zones_label("R", 7, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "R7") == 0);

    ui_zones_label("U", 0, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "U0") == 0);
    ui_zones_label("U", 100, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "U100") == 0);

    /* PMR channels are numbered from one, as on every PMR radio */
    ui_zones_label("PMR", 0, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "PMR1") == 0);
    ui_zones_label("PMR", 15, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "PMR16") == 0);

    /* A long name is truncated, never overflowed */
    char small[6];
    ui_zones_label("PMR", 15, small, sizeof(small));
    REQUIRE(std::strlen(small) < sizeof(small));
}

TEST_CASE("Zone rows show the real power, including the 500 mW mark",
          "[ui][zones]")
{
    char buf[12];

    ui_zones_powerText(500, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "500mW") == 0);
    REQUIRE(ui_zones_isQrp(500));

    ui_zones_powerText(1000, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "1W") == 0);
    REQUIRE_FALSE(ui_zones_isQrp(1000));

    ui_zones_powerText(2500, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "2.5W") == 0);

    /* Unknown power is not a zero that reads as a real value */
    ui_zones_powerText(0, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "-") == 0);
    REQUIRE_FALSE(ui_zones_isQrp(0));

    ui_zones_fmtFreq(446006250UL, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "446.00625") == 0);
}

TEST_CASE("Zone browser walks zones, scrolls and tunes", "[ui][zones]")
{
    buildTables();
    ui_zones_setSource(&testSource);
    REQUIRE(ui_zones_source() == &testSource);

    /* The list uses the shared 12 px row: 8 entries per screen */
    REQUIRE(ui_zones_visibleRows() == ui_rows_fit(ui_row_height()));

    state.ui_screen = MENU_ZONES;
    state.txDisable = true;
    ui_zones_open();
    REQUIRE(ui_zones_isOpen());

    bool sync = false;

    /* Zone list: UP on the first wraps to the last, DOWN follows */
    press(KEY_UP, &sync);
    press(KEY_DOWN, &sync);
    press(KEY_DOWN, &sync);      /* U */
    press(KEY_ENTER, &sync);     /* open U */
    REQUIRE(ui_zones_isOpen());

    /* Entries: 5 of them, so a wrap goes back to the first */
    press(KEY_DOWN, &sync);
    press(KEY_DOWN, &sync);
    press(KEY_DOWN, &sync);
    press(KEY_DOWN, &sync);      /* last (index 4) */
    press(KEY_DOWN, &sync);      /* wraps to index 0 */
    press(KEY_UP, &sync);        /* and back to the last */

    /* Green tunes the entry and closes the browser */
    press(KEY_ENTER, &sync);
    REQUIRE(sync == true);
    REQUIRE_FALSE(ui_zones_isOpen());
    REQUIRE(state.channel.rx_frequency == uEntries[4].rx_hz);
    REQUIRE(state.channel.tx_frequency == uEntries[4].tx_hz);
    REQUIRE(state.channel.power == uEntries[4].power_mw);
    REQUIRE(std::strcmp(state.channel.name, "U4") == 0);

    /* Red from the zone list closes without tuning anything */
    ui_zones_open();
    sync = false;
    press(KEY_ESC, &sync);
    REQUIRE_FALSE(ui_zones_isOpen());
    REQUIRE(sync == false);
}

TEST_CASE("Zone browser marks the PMR zone and returns to the zones",
          "[ui][zones]")
{
    buildTables();
    ui_zones_setSource(&testSource);

    state.ui_screen = MENU_ZONES;
    ui_zones_open();

    bool sync = false;
    press(KEY_DOWN, &sync);
    press(KEY_DOWN, &sync);      /* PMR */
    press(KEY_ENTER, &sync);     /* entries */
    REQUIRE(ui_zones_isOpen());

    /* Red from the entries goes back to the zone list, not out */
    press(KEY_ESC, &sync);
    REQUIRE(ui_zones_isOpen());
    REQUIRE_FALSE(sync);

    /* Enter again and tune the first PMR channel */
    press(KEY_ENTER, &sync);
    press(KEY_ENTER, &sync);
    REQUIRE(std::strcmp(state.channel.name, "PMR1") == 0);
    REQUIRE(state.channel.power == 500);
    REQUIRE(ui_zones_isQrp(state.channel.power));
}

TEST_CASE("Zone browser without tables says so and stays harmless",
          "[ui][zones]")
{
    ui_zones_setSource(NULL);

    state.ui_screen = MENU_ZONES;
    ui_zones_open();
    REQUIRE(ui_zones_isOpen());

    bool sync = false;

    /* No zone to open: every key is consumed, nothing is tuned */
    press(KEY_DOWN, &sync);
    press(KEY_ENTER, &sync);
    press(KEY_UP, &sync);
    REQUIRE(sync == false);
    REQUIRE(ui_zones_isOpen());

    press(KEY_ESC, &sync);
    REQUIRE_FALSE(ui_zones_isOpen());
    REQUIRE(sync == false);
}
