/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The empty states: what a screen says when it has nothing to say.
 *
 * docs/TEST_PLAN.md section 3: "sin sincronismo se lee SRCH/- , nunca un cero".
 * A radio that prints CC0 while it is only searching has told the operator a
 * colour code it never decoded, and the same lie in a RAN, a NAC, a cell or a
 * group count is just as bad, because everywhere else in this interface a zero
 * *is* a value.
 *
 * The rules, applied by pattern instead of by a string copied per mode, so a
 * mode added later cannot skip them:
 *
 *  1. nothing decoded: `id` says it is searching and carries no digit at all;
 *     `call`, `qual` and `note` are empty;
 *  2. a receiver that is locked but has not decoded an identifier yet: the
 *     identifier is left out or reads "-", never 0;
 *  3. the text the block computes is the text the main screen draws (the block
 *     is not a diagnostic that nobody sees).
 *
 * And the same rule where the interface counts something: a menu entry with no
 * unread message carries no badge, it does not carry "!0".
 */

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstring>
#include <string>

#include "ui_walk.hpp"

extern "C" {
#include "ui/ui_mode_state.h"
}

namespace
{

/** True when the string carries any decimal digit. */
bool hasDigit(const char *s)
{
    for (const char *p = s; *p != '\0'; p++)
        if (isdigit((unsigned char)*p) != 0)
            return true;
    return false;
}

/**
 * True when the identifier carries a digit of its own -- not the ones inside the
 * mode name, which is a word and not a value: "P25 SYNC" says the receiver is
 * locked and has decoded nothing, "NAC000" would say it decoded a zero.
 */
bool hasIdentifierDigit(const char *s)
{
    std::string t(s);
    size_t sp = t.find(' ');

    if (sp == std::string::npos)
        return hasDigit(t.c_str());

    std::string prefix = t.substr(0, sp);

    for (char &c : prefix)
        c = (char)toupper((unsigned char)c);

    static const char *const names[] = { "FM", "M17", "TETRA", "DMR", "NXDN",
                                         "DPMR", "P25", "YSF", "DSTAR", "AM",
                                         "WFM" };
    bool isName = false;

    for (const char *n : names)
        if (prefix == n)
            isName = true;

    return hasDigit(isName ? t.substr(sp + 1).c_str() : t.c_str());
}

/** True when the string says the receiver is searching. */
bool saysSearching(const char *s)
{
    std::string t(s);

    for (char &c : t)
        c = (char)tolower((unsigned char)c);

    return (t.find("srch") != std::string::npos)
        || (t.find("search") != std::string::npos);
}

/** Everything the screen drew, one string per line, joined. */
std::string drawnText()
{
    ui_walk::Snapshot s = ui_walk::observe();
    std::string text;

    for (const ui_walk::Line &l : s.lines)
        text += l.text + " | ";

    return text;
}

} /* namespace */

/* ======================================================================== */

TEST_CASE("Nothing decoded reads as searching, never as a zero",
          "[ui][state][empty]")
{
    const uint8_t modes[] = {
        OPMODE_DMR, OPMODE_NXDN, OPMODE_DPMR, OPMODE_P25,
        OPMODE_YSF, OPMODE_DSTAR, OPMODE_TETRA,
    };

    for (uint8_t mode : modes) {
        ui_walk::resetUi(mode, 145500000);

        /*
         * DMR keeps its state block off the screen until a DMR ID is set -- the
         * "NO TX" warning owns line 2 instead (ui_main.c, OPMODE_DMR). That is
         * the intent, so the test sets the ID the operator is asked for and the
         * block is then the thing on the screen.
         */
        state.settings.dmr_id = 1234567;

        /* Every receiver double back to "nothing decoded" */
        dmr_sim_clear();
        tetra_sim_clearCell();

        ui_mode_state_t st;
        ui_mode_state_read(&st);

        INFO("mode " << ui_mode_title(mode) << ": id=\"" << st.id << "\"");
        REQUIRE_FALSE(st.sync);
        REQUIRE(saysSearching(st.id));
        /* The strong form of "never a zero": no identifier at all */
        REQUIRE_FALSE(hasIdentifierDigit(st.id));
        REQUIRE(st.callTxt[0] == '\0');
        REQUIRE(st.qual[0] == '\0');
        REQUIRE(st.note[0] == '\0');

        /*
         * And it reaches the screen: the main screen of that mode shows the
         * searching text in its state block. Not just the block's own idea of
         * it -- a block nobody draws is a screen that says nothing.
         */
        std::string text = drawnText();

        INFO("mode " << ui_mode_title(mode) << " screen: \"" << text << "\"");
        REQUIRE(text.find(st.id) != std::string::npos);
    }
}

TEST_CASE("An identifier that has not been decoded reads '-', never 0",
          "[ui][state][empty]")
{
    ui_mode_state_t st;

    /* DMR: sync and timeslot, colour code not decoded yet */
    ui_walk::resetUi(OPMODE_DMR, 145500000);
    dmr_sim_setStatus(true, -1, 2, true, 0, false, false, 0, 0, -1);
    ui_mode_state_read(&st);
    INFO("DMR id=\"" << st.id << "\"");
    REQUIRE(st.sync);
    REQUIRE(std::string(st.id).find("CC0") == std::string::npos);
    REQUIRE(std::string(st.id).find("CC-") != std::string::npos);
    REQUIRE_FALSE(st.call);            /* no identifiers, no call line */

    /* DMR: no timeslot either */
    dmr_sim_setStatus(true, 3, 0, true, 0, false, false, 0, 0, -1);
    ui_mode_state_read(&st);
    INFO("DMR id=\"" << st.id << "\"");
    REQUIRE(std::string(st.id).find("CC3 TS-") != std::string::npos);

    /* P25: locked, no NAC yet: no "NAC000" */
    ui_walk::resetUi(OPMODE_P25, 145500000);
    p25Status_simSet(true, false, -1, -1, -1, -1, 0, 0, 0);
    ui_mode_state_read(&st);
    INFO("P25 id=\"" << st.id << "\"");
    REQUIRE(st.sync);
    REQUIRE(std::string(st.id).find("NAC") == std::string::npos);
    REQUIRE_FALSE(hasIdentifierDigit(st.id));

    /* NXDN: locked, RAN not decoded yet: no "RAN0" */
    ui_walk::resetUi(OPMODE_NXDN, 145500000);
    nxdnStatus_simSet(true, -1, false, 0, false, 0, 0, 0, 0);
    ui_mode_state_read(&st);
    INFO("NXDN id=\"" << st.id << "\"");
    REQUIRE(st.sync);
    REQUIRE(std::string(st.id).find("RAN") == std::string::npos);
    REQUIRE_FALSE(hasIdentifierDigit(st.id));

    /* D-STAR: locked, no header: no ">" between two empty callsigns */
    ui_walk::resetUi(OPMODE_DSTAR, 145500000);
    dstarStatus_simSet(true, false, "", "", "", 0, "");
    ui_mode_state_read(&st);
    INFO("DSTAR id=\"" << st.id << "\"");
    REQUIRE(st.sync);
    REQUIRE(std::string(st.id).find('>') == std::string::npos);
    REQUIRE_FALSE(hasDigit(st.id));

    /* TETRA: no cell at all: the cell line never reads "0-0" */
    ui_walk::resetUi(OPMODE_TETRA, 430500000);
    tetra_sim_clearCell();
    tetra_sim_setRegistration(false, 0, 0);
    ui_mode_state_read(&st);
    INFO("TETRA id=\"" << st.id << "\"");
    REQUIRE_FALSE(hasDigit(st.id));
    REQUIRE(saysSearching(st.id));      /* it says it is searching, not "0-0" */

    dmr_sim_clear();
    tetra_sim_clearCell();
}

TEST_CASE("A menu entry with nothing unread carries no badge", "[ui][state][empty]")
{
    ui_walk::resetUi(OPMODE_FM, 145500000);
    messaging_clear();

    char buf[8];

    ui_msgs_fmt_unread(buf, sizeof(buf));
    REQUIRE(std::string(buf).empty());
    REQUIRE(std::string(buf).find('0') == std::string::npos);

    REQUIRE(messaging_received(MSG_TRANSPORT_APRS, 1, 0, "hola", false,
                               1000) > 0);
    ui_msgs_fmt_unread(buf, sizeof(buf));
    REQUIRE(std::string(buf) == "!1");

    /* Reading it clears the badge: it disappears, it does not become "!0" */
    messaging_markRead();
    ui_msgs_fmt_unread(buf, sizeof(buf));
    REQUIRE(std::string(buf).empty());
}
