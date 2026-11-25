/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>

extern "C" {
#include "core/state.h"
#include "core/spectrum_service.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_modes.h"
}

/*
 * The menu after five integration passes, as one map.
 *
 * Each pass added rows to the popup of a mode, a screen behind a row, and a
 * "can this key the transmitter" answer somewhere else. The registry
 * (ui_modes.h) is what keeps those three in step: one entry per compiled mode,
 * pointing at the row table, and the capability flags every other screen
 * filters on. Nothing compiled checks that they agree, so this does:
 *
 *  - the registry and the quick menu name the same tables, in the same order,
 *    once each -- the side key and the popup cannot disagree about a mode;
 *  - every row is renderable: a non-empty label of at most ten characters, one
 *    label per row, no row without a value or an action, and every QM_GOTO
 *    pointing at a screen that exists in enum uiScreen;
 *  - UI_MODE_TX agrees with opmode_canTransmit(), which is what the PTT does:
 *    a mode that says it transmits but refuses the key (or the other way
 *    round) is the exact bug the TX passes of #4 left behind;
 *  - the analyser is reachable from the two modes it can run in, and its own
 *    table carries the ESPERA row of this pass where the operator expects it.
 */

namespace
{

/// Rows of one table, checked for what a renderer and a keypad need.
void checkTable(const qm_menu_t *qm)
{
    REQUIRE(qm != nullptr);
    REQUIRE(qm->count > 0);
    REQUIRE(qm->count <= 32);

    for (uint8_t i = 0; i < qm->count; i++) {
        const qm_item_t &row = qm->items[i];

        REQUIRE(row.label != nullptr);
        REQUIRE(strlen(row.label) > 0);
        REQUIRE(strlen(row.label) <= 10);

        /* Uppercase ASCII, digits and the two separators the labels use
           ("MAX RET", "MS/PTO"): the popup's font and the voice prompts both
           assume it, and a label in lower case reads as a different key */
        for (const char *c = row.label; *c != '\0'; c++)
            REQUIRE(((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
                     || (*c == ' ') || (*c == '/')));

        /* One label per table: the tests and the operator find a row by it */
        for (uint8_t j = 0; j < i; j++)
            REQUIRE(strcmp(qm->items[j].label, row.label) != 0);

        switch (row.type) {
            case QM_INT:
            case QM_ENUM:
            case QM_TOGGLE:
                REQUIRE(row.get != nullptr);
                REQUIRE(row.set != nullptr);
                REQUIRE(row.max >= row.min);
                break;

            case QM_INFO:
                REQUIRE(row.fmt != nullptr);
                break;

            case QM_GOTO:
                /* A row that leaves the popup has to land somewhere real */
                REQUIRE(row.goto_screen <= (uint8_t)SPECTRUM);
                REQUIRE(row.goto_screen != (uint8_t)MENU_TOP);
                break;

            case QM_ACTION:
                REQUIRE(row.act != nullptr);
                break;

            default:
                FAIL("row " << row.label << " has an unknown kind");
                break;
        }

        if (row.type == QM_ENUM || row.type == QM_TOGGLE) {
            /* An enumeration is rendered from names[] or from a formatter of
               its own (the CTCSS tone row prints a frequency, not an index):
               a row with neither can only show a number the operator cannot
               read */
            INFO("row " << row.label << " of table " << qm->title);
            REQUIRE((row.names != nullptr || row.fmt != nullptr));

            if (row.names != nullptr) {
                /* The value the row can hold is an index into names[], so the
                   last name has to exist: a max one past the table walks into
                   the next string literal */
                REQUIRE(row.names[row.max] != nullptr);

                for (int k = row.min; k <= row.max; k++) {
                    REQUIRE(row.names[k] != nullptr);
                    REQUIRE(strlen(row.names[k]) > 0);
                }
            }
        }
    }
}

/// Index of the row with this label, -1 when the table has none.
int rowOf(const qm_menu_t *qm, const char *label)
{
    for (uint8_t i = 0; i < qm->count; i++)
        if (strcmp(qm->items[i].label, label) == 0)
            return i;

    return -1;
}

} /* namespace */

TEST_CASE("the registry and the quick menu are the same list", "[ui][menu]")
{
    const uint8_t count = ui_mode_count();
    REQUIRE(count >= 3);

    for (uint8_t i = 0; i < count; i++) {
        const ui_mode_t *entry = ui_mode_at(i);
        REQUIRE(entry != nullptr);

        /* The popup finds the table by mode id, and the registry is that table:
           one pointer, so a row added in one place is there in the other */
        REQUIRE(qm_findMenu(entry->mode) == entry);
        REQUIRE(ui_mode_find(entry->mode) == entry);

        /* No mode twice, and every id is one of the enum's */
        REQUIRE(entry->mode > OPMODE_NONE);
        REQUIRE(entry->mode <= OPMODE_WFM_BC);

        for (uint8_t j = 0; j < i; j++)
            REQUIRE(ui_mode_at(j)->mode != entry->mode);

        /* The side key and the channel editor only walk what they are told to */
        REQUIRE((entry->flags & (UI_MODE_SELECT | UI_MODE_CHANNEL)) != 0);

        checkTable(entry);
    }

    /* Past the end there is nothing, and an unknown mode falls back to FM's
       table rather than to a null pointer */
    REQUIRE(ui_mode_at(count) == nullptr);
    REQUIRE(qm_findMenu(0x7F) == qm_findMenu(OPMODE_FM));
}

TEST_CASE("a mode that says it transmits is a mode the PTT keys",
          "[ui][menu][tx]")
{
    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *entry = ui_mode_at(i);
        const bool flag = (entry->flags & UI_MODE_TX) != 0;
        const bool can = opmode_canTransmit(entry->mode);

        INFO("mode " << (int)entry->mode << " (" << entry->title << "): flag "
                     << flag << ", PTT " << can);
        REQUIRE(flag == can);
    }

    /* FM is the one mode every build has a transmitter for */
    REQUIRE(opmode_canTransmit(OPMODE_FM));
    /* TETRA is receive only: the mode forces the status back to RX */
    REQUIRE_FALSE(opmode_canTransmit(OPMODE_TETRA));
}

TEST_CASE("the analyser is reachable and its table carries this pass's rows",
          "[ui][menu][spectrum]")
{
    /* Both analogue modes the sweep can run in lead to the screen */
    const uint8_t analogueModes[] = { OPMODE_FM, OPMODE_AIRBAND };

    for (uint8_t mode : analogueModes) {
        const qm_menu_t *qm = qm_findMenu(mode);
        REQUIRE(qm != nullptr);

        const int row = rowOf(qm, "ESPECTRO");
        REQUIRE(row >= 0);
        REQUIRE(qm->items[row].type == QM_GOTO);
        REQUIRE(qm->items[row].goto_screen == (uint8_t)SPECTRUM);
    }

    const qm_menu_t *qm = qm_spectrumMenu();
    checkTable(qm);

    /* The rows of docs/SPECTRUM.md §5, in that order: the span and the step the
       sweep is configured with, the wait this pass added, the maximum hold and
       its clear action, and the two read-only numbers */
    REQUIRE(rowOf(qm, "SPAN") == 0);
    REQUIRE(rowOf(qm, "PASO") == 1);
    REQUIRE(rowOf(qm, "ESPERA") == 2);
    REQUIRE(rowOf(qm, "MAX RET") == 3);
    REQUIRE(rowOf(qm, "BORRAR MAX") == 4);
    REQUIRE(rowOf(qm, "PICO") == 5);
    REQUIRE(rowOf(qm, "MS/PTO") == 6);

    /* ESPERA walks the five waits the driver understands, and the row and the
       service agree on which one is in use */
    const qm_item_t &settle = qm->items[rowOf(qm, "ESPERA")];

    REQUIRE(settle.type == QM_ENUM);
    REQUIRE(settle.min == 0);
    REQUIRE(settle.max == 4);
    REQUIRE(strcmp(settle.names[0], "AUTO") == 0);
    REQUIRE(strcmp(settle.names[1], "50us") == 0);
    REQUIRE(strcmp(settle.names[4], "400us") == 0);

    for (int i = settle.min; i <= settle.max; i++) {
        settle.set(i);
        REQUIRE(settle.get() == i);
        REQUIRE(spectrumService_settleIndex() == (uint8_t)i);
    }

    settle.set(0);
    REQUIRE(spectrumService_settleUs() == 0);

    /* The two read-only rows are the numbers the operator writes down, and the
       font has to fit whatever they print */
    const char *info[] = { "PICO", "MS/PTO" };

    for (const char *label : info) {
        const qm_item_t &row = qm->items[rowOf(qm, label)];
        char buf[24] = { 0 };

        REQUIRE(row.type == QM_INFO);
        row.fmt(buf, sizeof(buf));
        REQUIRE(strlen(buf) > 0);
        REQUIRE(strlen(buf) < sizeof(buf));
    }
}

TEST_CASE("the spectrum screen is the last of the enum", "[ui][menu]")
{
    /*
     * The screens above it are reached by number from the quick menu's QM_GOTO
     * rows and from the UI tests, so inserting a screen in the middle renumbers
     * them: a new one goes at the end, next to SPECTRUM.
     */
    REQUIRE((int)SPECTRUM > (int)MENU_TOP);
    REQUIRE((int)SPECTRUM > (int)LOW_BAT);
    REQUIRE((int)SPECTRUM > (int)MSG_STATUS);
    REQUIRE((int)SPECTRUM > (int)PKT_STATUS);
}
