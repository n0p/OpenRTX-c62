/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Messaging FSM on the host: navigation, multi-tap composition, the draft and
 * the refusal paths (no transport, empty text, bad callsign).  The messages
 * themselves are the shared store (core/messaging.h): this test looks only at
 * what the screens do with it.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "core/state.h"
#include "core/input.h"
#include "core/messaging.h"
#include "core/msg_service.h"
#include "core/graphics.h"
#include "ui/ui_default.h"
#include "ui/ui_messages.h"
}

extern "C" state_t state;

static kbd_msg_t key(uint32_t keys, bool longPress = false)
{
    kbd_msg_t m;
    m.value = 0;
    m.keys = keys;
    m.long_press = longPress;
    return m;
}

static void press(uint32_t keys, bool longPress = false)
{
    kbd_msg_t m = key(keys, longPress);
    bool sync = false;
    REQUIRE(ui_msgs_isScreen(state.ui_screen));
    ui_msgs_handleEvent(&m, &sync);
}

/* A clean store: no messages and no draft */
static void resetStore(void)
{
    state.settings = default_settings;
    state.channel.mode = OPMODE_FM;
    messaging_init();
    messaging_clear();
}

TEST_CASE("Messages: inbox, folders and back", "[ui][messages]")
{
    resetStore();

    ui_msgs_open();
    REQUIRE(state.ui_screen == MENU_MESSAGES);

    press(KEY_F1);
    REQUIRE(state.ui_screen == MSG_SENT);
    press(KEY_F1);
    REQUIRE(state.ui_screen == MENU_MESSAGES);

    press(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_TOP);
}

TEST_CASE("Messages: multi-tap composition and draft", "[ui][messages]")
{
    resetStore();

    ui_msgs_compose("EA5ABC-7");
    REQUIRE(state.ui_screen == MSG_COMPOSE);

    /* Compose starts on the TO row, green starts typing there */
    press(KEY_ENTER);
    press(KEY_2);              /* A */
    press(KEY_2);              /* B: same burst, cycles the character */
    press(KEY_F1);             /* backspace */
    press(KEY_ESC);            /* leave typing, keep the draft */

    /* DOWN from TO is the text row */
    press(KEY_DOWN);
    press(KEY_ENTER);          /* typing */
    press(KEY_4);              /* G */
    press(KEY_3);              /* D: different key, new character */
    press(KEY_ENTER);          /* done, the draft goes to the store */

    REQUIRE(std::string(messaging_draft()->text) == "GD");
    REQUIRE(std::string(messaging_draft()->peer) == "EA5ABC-7");
    /* No transport is compiled in on the host, so the composer has none */
    REQUIRE(messaging_draft()->transport == MSG_TRANSPORT_COUNT);

    /* Leaving the composer keeps the draft and returns to the inbox */
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_MESSAGES);
}

TEST_CASE("Messages: sending is refused without a transport", "[ui][messages]")
{
    resetStore();

    ui_msgs_compose("EA5ABC");
    REQUIRE(state.ui_screen == MSG_COMPOSE);

    /* UP from TO is the TP row: no transport is compiled in on this host,
       so cycling it refuses and nothing is queued. */
    press(KEY_UP);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MSG_COMPOSE);

    /* SEND row, same answer: UP wraps from TP to SEND */
    press(KEY_UP);             /* TP -> SEND */
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MSG_COMPOSE);
    REQUIRE(messaging_outCount() == 0);
}

TEST_CASE("Messages: received message becomes read when opened",
          "[ui][messages]")
{
    resetStore();

    int id = messaging_received(MSG_TRANSPORT_DMR, 1234567, 0, "hola", false,
                                1000);
    REQUIRE(id > 0);
    REQUIRE(messaging_unread() == 1);

    ui_msgs_open();
    press(KEY_ENTER);          /* first row is the only message */
    REQUIRE(state.ui_screen == MSG_VIEW);
    REQUIRE(messaging_unread() == 0);

    press(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_MESSAGES);
}
