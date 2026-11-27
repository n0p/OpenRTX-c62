/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The in-radio channel editor (ui/ui_channels.c), driven through the keypad it
 * is written for: create, edit, save, delete behind a confirmation, and the
 * list that opens on the channel the radio was on at power-off.
 *
 * docs/TEST_PLAN.md section 3 lists the channel editor among the state machines
 * the host has to decide, and it is the screen with the most ways to leave a
 * half-made record behind: P1 duplicates the selected channel or the VFO, green
 * enters a field, UP/DOWN change it, P1/P2 move the caret inside a name or a
 * frequency (the C62 has no arrow keys, see keyboard_c62.c), P2 saves it and
 * red cancels or goes back. Deleting is a long P2 and a second green, so that
 * the key that edits a channel is never the key that erases it.
 *
 * The assertions are on what the editor draws (the simulator's frame recorder,
 * core/gfx_capture.h: the title, the rows, the inverted selection and the caret
 * rectangle) and on what the channel bank holds afterwards -- never on a static
 * of the module, which is the state that can be wrong without anybody seeing
 * it.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <cstdio>
#include <string>

#include "ui_walk.hpp"

extern "C" {
#include "core/channel_store.h"
#include "core/cps.h"
#include "ui/ui_layout.h"
}

namespace
{

/* CH_ROW_H in ui_channels.c: a 20 px two-line row (6 pt name over 5 pt) */
constexpr int ROW_H = 20;

/* The field rows of the editor are 12 px, one line of 5 pt text each */
constexpr int FIELD_H = 12;

/** The frame of the screen the last key left, drawn and read back. */
gfx_capture_frame_t frame()
{
    gfx_capture_frame_t f;

    ui_walk::drawNow(&f);
    return f;
}

/** Every string the frame holds, for an INFO() when a check fails. */
std::string frameText(const gfx_capture_frame_t &f)
{
    std::string s = std::string("[") + f.title + "] ";

    for (uint16_t i = 0; i < f.lines; i++)
        s += std::string(f.line[i].text) + " | ";

    return s;
}

/** The first line that holds `needle`, NULL if the screen does not show it. */
const gfx_capture_line_t *findLine(const gfx_capture_frame_t &f,
                                   const char *needle)
{
    for (uint16_t i = 0; i < f.lines; i++)
        if (strstr(f.line[i].text, needle) != NULL)
            return &f.line[i];
    return NULL;
}

bool hasLine(const gfx_capture_frame_t &f, const char *needle)
{
    return findLine(f, needle) != NULL;
}

/** The colour a line was drawn in, so that a selected row can be told apart. */
uint32_t colorOf(const gfx_capture_frame_t &f, const char *needle)
{
    const gfx_capture_line_t *l = findLine(f, needle);

    REQUIRE(l != NULL);
    return l->rgb & 0xFFFFFFu;
}

/**
 * Which row carries the inverted rectangle, -1 if none.
 *
 * The list and the editor have different row heights (CH_ROW_H and
 * CH_FIELD_H in ui_channels.c), so the caller says which screen it is looking
 * at; the rectangle's top is measured from the first row either way.
 */
int selectedRow(const gfx_capture_frame_t &f, int rowH = ROW_H)
{
    if (!f.selection)
        return -1;
    return (f.sel_top - UI_LIST_TOP) / rowH;
}

/** The bank entry, as it was written to flash. */
chstore_channel_t stored(uint16_t idx)
{
    chstore_channel_t c;

    memset(&c, 0, sizeof(c));
    REQUIRE(chstore_get(idx, &c) == 0);
    return c;
}

/* One clean radio with the harness's stores (one channel, "SEED"). */
void boot(void)
{
    ui_walk::seedStores();
    ui_walk::resetUi();

    REQUIRE(chstore_count() == 1);
    REQUIRE(std::string(stored(0).name) == "SEED");
}

/** Enter the list, the way the tools menu does. */
void openList(void)
{
    ui_channels_open();
    REQUIRE(ui_channels_isOpen());
    REQUIRE(selectedRow(frame()) == 0);
}

} /* namespace */

/* ======================================================================== */

TEST_CASE("the editor creates a channel and leaves the radio alone until green",
          "[ui][channels][editor]")
{
    boot();
    openList();

    /* The list: a header with the count and the four hints of the screen */
    gfx_capture_frame_t f = frame();

    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "CHANNELS 1/128");
    REQUIRE(hasLine(f, "01 SEED"));
    REQUIRE(hasLine(f, "TUNE"));
    REQUIRE(hasLine(f, "NEW"));
    REQUIRE(hasLine(f, "EDIT"));
    REQUIRE(hasLine(f, "EXIT"));

    /* P1 duplicates the selected channel and opens the editor on it */
    ui_walk::pressKey(KEY_F1);

    f = frame();
    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "NEW CHANNEL");
    /* Nothing is written yet: the bank still has one channel */
    REQUIRE(chstore_count() == 1);
    /* And the radio has not moved: an editor that tunes while editing would
       leave the operator listening to a channel he never confirmed */
    REQUIRE(state.tuner_mode == VFO);
    REQUIRE(state.channel.rx_frequency == 145500000);
    REQUIRE(state.ui_screen != MAIN_MEM);

    /* The fields, in the order the operator walks them */
    REQUIRE(hasLine(f, "NAME"));
    REQUIRE(hasLine(f, "RX"));
    REQUIRE(hasLine(f, "TX"));
    REQUIRE(hasLine(f, "SIMPLEX"));
    REQUIRE(hasLine(f, "MODE"));
    REQUIRE(hasLine(f, "POWER"));
    /* The DMR colour code is not offered on an FM channel */
    REQUIRE_FALSE(hasLine(f, "DMR CC/TS"));

    /* Green enters the name field: the selection goes from filled to hollow
       and the caret appears under the character being edited */
    REQUIRE(f.selection);
    REQUIRE(f.sel_filled);
    REQUIRE_FALSE(f.caret);

    ui_walk::pressKey(KEY_ENTER);

    f = frame();
    INFO(frameText(f));
    REQUIRE(f.selection);
    REQUIRE(f.sel_filled == false);
    REQUIRE(f.caret);
    REQUIRE(f.caret_y == UI_LIST_TOP + FIELD_H - 3);
    REQUIRE(f.caret_x > UI_TEXT_X);

    /* UP raises the character under the caret: D -> E */
    ui_walk::pressKey(KEY_UP);

    f = frame();
    INFO(frameText(f));
    REQUIRE(hasLine(f, "SEEE"));

    /* Green leaves the field, P2 saves the record */
    ui_walk::pressKey(KEY_ENTER);
    REQUIRE_FALSE(frame().caret);

    ui_walk::pressKey(KEY_F2);

    f = frame();
    INFO(frameText(f));
    REQUIRE(ui_channels_isOpen());
    REQUIRE(std::string(f.title) == "CHANNELS 2/128");

    REQUIRE(chstore_count() == 2);

    chstore_channel_t created = stored(1);

    REQUIRE(std::string(created.name) == "SEEE");
    REQUIRE(created.rx_freq == 145500000);
    REQUIRE(created.tx_freq == 145500000);
    REQUIRE(created.mode == OPMODE_FM);
    REQUIRE(created.power == 1000);
    REQUIRE((created.flags & CHSTORE_FLAG_SIMPLEX) != 0);

    /* Saved is not tuned: green on the entry is what puts it on the air */
    REQUIRE(state.tuner_mode == VFO);
    REQUIRE(state.ui_screen != MAIN_MEM);

    /* The new entry is the one selected in the list, and it is drawn */
    REQUIRE(selectedRow(f) == 1);
    REQUIRE(hasLine(f, "02 SEEE"));

    ui_walk::pressKey(KEY_ENTER);

    REQUIRE_FALSE(ui_channels_isOpen());
    REQUIRE(state.tuner_mode == CH);
    REQUIRE(state.channel_index == 1);
    REQUIRE(state.ui_screen == MAIN_MEM);
    REQUIRE(state.channel.rx_frequency == 145500000);
    REQUIRE(std::string(state.channel.name) == "SEEE");
    REQUIRE(chstore_lastUsed() == 1);
}

TEST_CASE("P1 and P2 move the caret inside a field, not to the next field",
          "[ui][channels][editor]")
{
    boot();
    openList();

    ui_walk::pressKey(KEY_F2);          /* P2: edit the selected channel */

    gfx_capture_frame_t f = frame();

    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "EDIT 1/1");
    REQUIRE(selectedRow(f, FIELD_H) == 0);   /* NAME */

    ui_walk::pressKey(KEY_ENTER);       /* into the name */

    f = frame();
    REQUIRE(f.caret);
    /* The caret starts on the last character of "SEED", which is not the first */
    int16_t onLast = f.caret_x;

    REQUIRE(onLast > UI_TEXT_X);

    /* P2 walks it forward and, at the end, round to the first character */
    ui_walk::pressKey(KEY_F2);

    f = frame();
    INFO("caret x " << f.caret_x << " after one P2");
    REQUIRE(f.caret);
    REQUIRE(f.caret_x == 47);           /* the value column, first character */
    /* The field is still the name: the keys that move inside it must not also
       move the selection out of it */
    REQUIRE(selectedRow(f) == 0);
    REQUIRE(f.sel_filled == false);

    /* And P1 walks it back to where it was */
    ui_walk::pressKey(KEY_F1);

    f = frame();
    REQUIRE(f.caret);
    REQUIRE(f.caret_x == onLast);

    /* The frequency field has its own caret, on a digit: green enters it on the
       kHz place, the one an operator changes most */
    ui_walk::pressKey(KEY_ENTER);       /* out of the name */
    ui_walk::pressKey(KEY_DOWN);        /* NAME -> RX */

    f = frame();
    INFO(frameText(f));
    REQUIRE(f.sel_top == UI_LIST_TOP + FIELD_H);
    REQUIRE(selectedRow(f, FIELD_H) == 1);
    REQUIRE_FALSE(f.caret);

    ui_walk::pressKey(KEY_ENTER);       /* into the frequency */

    f = frame();
    INFO("caret x " << f.caret_x << " on RX");
    REQUIRE(f.caret);
    REQUIRE(f.caret_y == UI_LIST_TOP + FIELD_H + FIELD_H - 3);
    /* "145.5000": the caret on the kHz digit is past the dot */
    REQUIRE(f.caret_x > 47);

    int16_t onKHz = f.caret_x;

    ui_walk::pressKey(KEY_F1);

    f = frame();
    REQUIRE(f.caret);
    REQUIRE(f.caret_x < onKHz);

    /* And leaving the field takes the caret with it: the caret belongs to the
       field being edited, not to the screen */
    ui_walk::pressKey(KEY_ENTER);       /* out of the frequency */

    f = frame();
    REQUIRE_FALSE(f.caret);
    REQUIRE(f.sel_filled);              /* back to the filled row */
    REQUIRE(selectedRow(f, FIELD_H) == 1);
}

TEST_CASE("editing a stored channel is saved by P2 and dropped by red",
          "[ui][channels][editor]")
{
    boot();
    openList();

    /* A change that is cancelled changes nothing */
    ui_walk::pressKey(KEY_F2);          /* P2: edit channel 1 */
    REQUIRE(std::string(frame().title) == "EDIT 1/1");

    ui_walk::pressKey(KEY_DOWN);        /* NAME -> RX */
    ui_walk::pressKey(KEY_ENTER);       /* into the frequency */
    ui_walk::pressKey(KEY_UP);          /* one kHz up: 145.5010 */

    gfx_capture_frame_t f = frame();

    INFO(frameText(f));
    REQUIRE(hasLine(f, "145.5010"));

    ui_walk::pressKey(KEY_ESC);         /* out of the field */
    ui_walk::pressKey(KEY_ESC);         /* cancel the whole edit */

    f = frame();
    INFO(frameText(f));
    REQUIRE(ui_channels_isOpen());
    REQUIRE(std::string(f.title) == "CHANNELS 1/128");
    REQUIRE(stored(0).rx_freq == 145500000);
    REQUIRE(stored(0).tx_freq == 145500000);

    /* The same change, saved */
    ui_walk::pressKey(KEY_F2);
    ui_walk::pressKey(KEY_DOWN);
    ui_walk::pressKey(KEY_ENTER);
    ui_walk::pressKey(KEY_UP);
    ui_walk::pressKey(KEY_ENTER);       /* out of the field */
    ui_walk::pressKey(KEY_F2);          /* P2: save */

    f = frame();
    INFO(frameText(f));
    REQUIRE(ui_channels_isOpen());
    REQUIRE(std::string(f.title) == "CHANNELS 1/128");
    REQUIRE(hasLine(f, "145.5010"));
    /* The list row keeps saying the channel is simplex */
    REQUIRE(hasLine(f, "FM S"));

    REQUIRE(chstore_count() == 1);

    chstore_channel_t changed = stored(0);

    REQUIRE(changed.rx_freq == 145501000);
    /* SIMPLEX: the transmit frequency follows the receive one */
    REQUIRE(changed.tx_freq == 145501000);
    REQUIRE(std::string(changed.name) == "SEED");

    /* Editing a channel does not tune it: green in the list does */
    REQUIRE(state.tuner_mode == VFO);
}

TEST_CASE("delete asks first: NO keeps the channel, YES removes it",
          "[ui][channels][editor]")
{
    boot();
    openList();

    /* A short P2 edits; only a long one asks to delete */
    ui_walk::pressKey(KEY_F2);

    REQUIRE(std::string(frame().title) == "EDIT 1/1");
    REQUIRE(chstore_count() == 1);

    ui_walk::pressKey(KEY_ESC);
    REQUIRE(ui_channels_isOpen());

    ui_walk::pressKey(KEY_F2, true);    /* long P2: the confirmation */

    gfx_capture_frame_t f = frame();

    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "DELETE");
    REQUIRE(hasLine(f, "Delete 1?"));
    REQUIRE(hasLine(f, "SEED"));
    REQUIRE(hasLine(f, "YES"));
    REQUIRE(hasLine(f, "NO"));
    /* Nothing is gone yet: the question is the whole point */
    REQUIRE(chstore_count() == 1);

    /* Red (NO) goes back to the list, with the channel still there */
    ui_walk::pressKey(KEY_ESC);

    f = frame();
    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "CHANNELS 1/128");
    REQUIRE(hasLine(f, "01 SEED"));
    REQUIRE(chstore_count() == 1);

    /* Green (YES) is the only key that erases */
    ui_walk::pressKey(KEY_F2, true);
    REQUIRE(std::string(frame().title) == "DELETE");

    ui_walk::pressKey(KEY_ENTER);

    f = frame();
    INFO(frameText(f));
    REQUIRE(ui_channels_isOpen());
    REQUIRE(std::string(f.title) == "CHANNELS 0/128");
    REQUIRE(hasLine(f, "No channels"));
    REQUIRE(hasLine(f, "P1 adds the VFO"));
    REQUIRE(chstore_count() == 0);

    /* The bank is empty on flash too, not just in RAM */
    ui_walk::rebootBank();
    REQUIRE(chstore_count() == 0);
}

/*
 * The keypad delivers a held key as two events: the press at once and the long
 * press 700 ms later.  The list used to act on the press edge (it opened the
 * editor), so the long press arrived in the editor and saved the channel there:
 * the confirmation that deletes a channel could not be reached, and every
 * attempt rewrote the 8 KiB slot instead.  This walks the two events in the
 * order keyboard_c62.c produces them.
 */
TEST_CASE("a held P2 asks to delete instead of opening the editor",
          "[ui][channels][editor]")
{
    boot();
    openList();

    /* The press: nothing has happened yet, the list is still on screen */
    ui_walk::pressEvent(KEY_F2, false);

    gfx_capture_frame_t f = frame();

    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "CHANNELS 1/128");
    REQUIRE(chstore_count() == 1);

    /* 700 ms later, still held: the delete confirmation */
    ui_walk::pressEvent(KEY_F2, true);

    f = frame();
    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "DELETE");
    REQUIRE(hasLine(f, "Delete 1?"));

    /* And letting go decides nothing else: no editor, no save */
    ui_walk::pressEvent(0u, false);

    f = frame();
    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "DELETE");
    REQUIRE(chstore_count() == 1);

    /* The release of a plain tap is what opens the editor */
    ui_walk::pressKey(KEY_ESC);          /* NO: back to the list */
    REQUIRE(std::string(frame().title) == "CHANNELS 1/128");

    ui_walk::pressEvent(KEY_F2, false);
    REQUIRE(std::string(frame().title) == "CHANNELS 1/128");
    ui_walk::pressEvent(0u, false);
    REQUIRE(std::string(frame().title) == "EDIT 1/1");
}

TEST_CASE("the list opens on the channel that was in use at power-off",
          "[ui][channels][editor]")
{
    boot();

    /* Two more channels, as a codeplug or an earlier session would have left
       them */
    for (const char *name : { "SECOND", "THIRD" }) {
        chstore_channel_t c;

        memset(&c, 0, sizeof(c));
        snprintf(c.name, sizeof(c.name), "%s", name);
        c.rx_freq = (strcmp(name, "SECOND") == 0) ? 430250000u : 438800000u;
        c.tx_freq = c.rx_freq + 5000000u;
        c.power   = 1000;
        c.mode    = OPMODE_FM;
        c.bandwidth = BW_12_5;
        REQUIRE(chstore_add(&c) >= 0);
    }

    REQUIRE(chstore_count() == 3);

    ui_walk::resetUi();
    openList();

    gfx_capture_frame_t f = frame();

    INFO(frameText(f));
    REQUIRE(std::string(f.title) == "CHANNELS 3/128");
    REQUIRE(hasLine(f, "03 THIRD"));
    /* No channel has been used yet: the list starts at the top */
    REQUIRE(selectedRow(f) == 0);

    /* Down twice, green: the radio is on the third channel */
    ui_walk::pressKey(KEY_DOWN);
    ui_walk::pressKey(KEY_DOWN);

    f = frame();
    REQUIRE(selectedRow(f) == 2);
    /* Selected: black ink on the inverted rectangle; the others, white */
    REQUIRE(colorOf(f, "03 THIRD") == 0x000000);
    REQUIRE(colorOf(f, "01 SEED") == 0xFFFFFF);

    ui_walk::pressKey(KEY_ENTER);

    REQUIRE_FALSE(ui_channels_isOpen());
    REQUIRE(state.channel_index == 2);
    REQUIRE(state.channel.rx_frequency == 438800000);
    REQUIRE(chstore_lastUsed() == 2);

    /* Power off (the shutdown write) and on again: the bank comes back from
       flash, and the list has to open on the channel that was in use -- that is
       the whole point of remembering it */
    REQUIRE(chstore_commit() == 0);

    ui_walk::rebootBank();
    REQUIRE(chstore_count() == 3);
    REQUIRE(chstore_lastUsed() == 2);

    ui_channels_open();

    REQUIRE(ui_channels_isOpen());

    f = frame();
    INFO(frameText(f));
    REQUIRE(f.selection);
    REQUIRE(f.sel_top == UI_LIST_TOP + 2 * ROW_H);
    REQUIRE(selectedRow(f) == 2);
    REQUIRE(colorOf(f, "03 THIRD") == 0x000000);
    REQUIRE(colorOf(f, "02 SECOND") == 0xFFFFFF);

    /* And green on it tunes it again, as it did before the power cycle */
    ui_walk::pressKey(KEY_ENTER);
    REQUIRE(state.channel_index == 2);
    REQUIRE(state.channel.rx_frequency == 438800000);
}

/*
 * An out-of-range CTCSS index must not be read out of the tone table.
 *
 * fmInfo_t.txTone is a 7-bit field and ctcss_tone[] has CTCSS_FREQ_NUM entries,
 * so a record from another codeplug, a restored backup or this union read in a
 * mode that is not FM can carry 50..127.  The main screen indexed the table
 * with it and drew whatever followed in .rodata ("27.9", "63.1"), and a value
 * copied from there into a channel was then stored.  ctcssFreqFromIndex() is
 * the one conversion the tree already had ("Wrapping is what channel_store.c
 * has always done"); the screen has to use it too.
 */
TEST_CASE("An out-of-range CTCSS index shows a tone that exists", "[ui][fm]")
{
    boot();
    ui_walk::resetUi();

    state.channel.mode        = OPMODE_FM;
    state.channel.fm.txToneEn = 1;
    state.channel.fm.rxToneEn = 0;
    state.channel.fm.txTone   = 91;   /* 7 bits wide, the table holds 50 */
    state.channel.fm.rxTone   = 91;

    gfx_capture_frame_t f = frame();

    uint16_t tone = ctcssFreqFromIndex(91);
    char want[16];

    snprintf(want, sizeof(want), "%u.%u", (unsigned)(tone / 10),
             (unsigned)(tone % 10));

    REQUIRE(hasLine(f, want));
}
