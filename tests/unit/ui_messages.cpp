/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Messaging FSM on the host: navigation, multi-tap composition, draft and the
 * refusal paths (no transport, empty text, bad callsign). The queue path that
 * needs a transport is covered by msg_service's own hooks.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "core/state.h"
#include "core/input.h"
#include "core/msg_store.h"
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

TEST_CASE("Messages: inbox, folders and back", "[ui][messages]")
{
    state.settings = default_settings;
    state.channel.mode = OPMODE_FM;
    msg_init();

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
    state.settings = default_settings;
    state.channel.mode = OPMODE_FM;
    msg_forget();
    msg_init();

    ui_msgs_compose("EA5ABC-7");
    REQUIRE(state.ui_screen == MSG_COMPOSE);

    /* Compose starts on the TO row, green starts typing there */
    press(KEY_ENTER);
    press(KEY_2);              /* A */
    press(KEY_2);              /* B: same burst, cycles the character */
    press(KEY_F1);             /* backspace */
    press(KEY_ESC);            /* leave typing, keep the draft */

    /* UP from TO is the text row */
    press(KEY_UP);
    press(KEY_ENTER);          /* typing */
    press(KEY_4);              /* G */
    press(KEY_3);              /* D: different key, new character */
    press(KEY_ENTER);          /* done, draft saved */

    REQUIRE(std::string(msg_draft()->text) == "GD");
    REQUIRE(std::string(msg_draft()->peer) == "EA5ABC-7");

    /* Leaving the composer keeps the draft and returns to the inbox */
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_MESSAGES);
}

TEST_CASE("Messages: sending is refused without a transport", "[ui][messages]")
{
    state.settings = default_settings;
    state.channel.mode = OPMODE_FM;
    msg_forget();
    msg_init();

    ui_msgs_compose("EA5ABC");
    REQUIRE(state.ui_screen == MSG_COMPOSE);

    /* DOWN from TO is the TP row: no transport is compiled in on this host,
       so cycling it refuses and nothing is queued. */
    press(KEY_DOWN);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MSG_COMPOSE);

    /* SEND row, same answer */
    press(KEY_DOWN);           /* TP -> SEND */
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MSG_COMPOSE);
    REQUIRE(msg_sent_count() == 0);
}

TEST_CASE("Messages: received message becomes read when opened", "[ui][messages]")
{
    state.settings = default_settings;
    state.channel.mode = OPMODE_FM;
    msg_forget();
    msg_init();

    msg_rec_t *in = msg_addIn(MSG_TP_APRS, "EA1XYZ", "hola", 1000);
    REQUIRE(in != NULL);
    REQUIRE(msg_unread() == 1);

    ui_msgs_open();
    press(KEY_ENTER);          /* first row is the only message */
    REQUIRE(state.ui_screen == MSG_VIEW);
    REQUIRE(msg_unread() == 0);

    press(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_MESSAGES);
}
