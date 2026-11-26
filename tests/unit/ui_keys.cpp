/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * What the four keys do on every screen, and whether they do it.
 *
 * docs/TEST_PLAN.md section 3 asks for two things of the key dispatch, and the
 * second one is the half no other test checks:
 *
 *   1. every screen declares what each of the four buttons does, on the hint
 *      row it draws (docs/UI.md, "Bottom hint line": green, P1, P2, red);
 *   2. a key that the row documents must *do* something when it is pressed.
 *
 * The existing walk (tests/unit/ui_menu_audit.cpp) proves that every key is
 * claimed by exactly one block of the dispatch; that is a different statement,
 * and a screen can claim a key and then ignore it -- the operator presses the
 * key the bar says and nothing happens. This file is the other half: it reads
 * the bar the screen actually drew (the frame recorder of the simulator,
 * core/gfx_capture.h, returns the text and its position) and requires an
 * observable change for every entry on it.
 *
 * "Observable" is the state the next key depends on (screen, cursor, table,
 * edit and lock flags, module owners, the arrow run that moves the private
 * cursors) plus every string the screen draws. A key that moves a value, opens
 * a screen, closes a module, enters an edit or leaves a scratch message on the
 * bar therefore counts; a key the screen documents and does none of those
 * things is a defect here.
 *
 * The screens are reached by a breadth-first walk over the real ui_updateFSM()
 * (tests/unit/ui_walk.hpp), the same one tests/unit/sim_coverage.cpp uses to
 * find screens, so a screen added later is walked without touching this file.
 * Each distinct (screen, hint bar) pair is checked once: the bar is what decides
 * which keys are documented in that state.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <set>

#include "ui_walk.hpp"

namespace
{

/* ------------------------------------------------------------ the hints --- */

/** One entry of the bottom hint row: which key, and what the row says it does. */
struct HintEntry
{
    uint32_t    key;
    std::string action;
    int16_t     x;
};

const char *keyName(uint32_t key)
{
    switch (key) {
        case KEY_ENTER: return "GREEN";
        case KEY_F1:    return "P1";
        case KEY_F2:    return "P2";
        case KEY_ESC:   return "RED";
        default:        return "?";
    }
}

/**
 * The bar the screen drew, read from the frame.
 *
 * ui_hints_draw() prints the key label ("P1:", "P2:") in yellow and the action
 * in white, at the same baseline, next to each other; green and red carry no
 * label because their position is fixed (the first entry is the green key, the
 * last one the red key -- docs/UI.md). The bar is centred and only the entries
 * with an action are drawn, so the parse is by position, not by a table written
 * here a second time.
 *
 * @return the entries, empty when the screen has no hint row (the main screens,
 *         whose bottom row is the S-meter).
 */
std::vector<HintEntry> hintBar(const std::vector<ui_walk::Line> &lines)
{
    std::vector<ui_walk::Line> bar;
    /* ui_hints_draw() prints at y + 1, with y = HEIGHT - UI_HINTS_H */
    int16_t baseline = (int16_t)(CONFIG_SCREEN_HEIGHT - UI_HINTS_H + 1);

    for (const ui_walk::Line &l : lines) {
        if (l.y == baseline && l.text.size() > 0) {
            bool dup = false;
            for (const ui_walk::Line &o : bar)
                if ((o.x == l.x) && (o.text == l.text))
                    dup = true;
            if (!dup)
                bar.push_back(l);
        }
    }

    /* Left to right: the recorder keeps drawing order, not geometry */
    for (size_t i = 0; i < bar.size(); i++)
        for (size_t j = i + 1; j < bar.size(); j++)
            if (bar[j].x < bar[i].x)
                std::swap(bar[i], bar[j]);

    std::vector<HintEntry> out;
    uint32_t pending = 0;       /* a "P1:" seen, waiting for its action */

    for (const ui_walk::Line &l : bar) {
        if (l.text.back() == ':') {
            if (l.text == "P1:")
                pending = KEY_F1;
            else if (l.text == "P2:")
                pending = KEY_F2;
            else
                pending = 0;
            continue;
        }

        HintEntry e;
        e.key    = pending;
        e.action = l.text;
        e.x      = l.x;
        pending  = 0;
        out.push_back(e);
    }

    /*
     * The unlabelled entries are green and red, in that order: green first, red
     * last. A bar with one unlabelled entry between two labelled ones would be
     * ambiguous, and that is a defect of the bar itself -- say so instead of
     * guessing.
     */
    if (!out.empty() && out.front().key == 0)
        out.front().key = KEY_ENTER;
    if ((out.size() > 1) && out.back().key == 0)
        out.back().key = KEY_ESC;

    return out;
}

const uint32_t BAR_KEYS[] = { KEY_ENTER, KEY_F1, KEY_F2, KEY_ESC };

/*
 * Keys the bar documents that are deliberately inert, with the reason.
 *
 * The list is a decision, not an excuse: everything not listed here has to
 * change something. Each entry is printed in the report, so the day the key
 * starts doing something the exemption is visible and can be removed.
 */
struct Exempt
{
    const char *owner;
    uint32_t    key;
    const char *why;
};

const Exempt EXEMPT[] = {
    {
        "SETTINGS_TIMEDATE_SET", KEY_ENTER,
        "green saves dd/mm/yy once the six digits are in and is inert until "
        "then: there is no partial save, and the digits already typed are the "
        "only thing on the screen"
    },
};

} /* namespace */

/* ======================================================================== */

TEST_CASE("Every key the hint row documents does something", "[ui][keys]")
{
    /*
     * The check is per (screen, bar, key) and not per state: a key that does
     * nothing is a key bound to nothing, while the same key on a value that is
     * already at its maximum, on an empty list or with a draft that starts with
     * a space legitimately changes nothing in that one state. So every state
     * that shows a bar is tried, and the failure is a key that changed nothing
     * in *any* of them.
     */
    struct BarUse
    {
        std::string owner;          /* who draws the bar                    */
        std::string report;         /* "MENU_TOP GREEN=\"OK\" RED=\"Back\"" */
        int states = 0;             /* states that showed this bar          */
        std::map<uint32_t, bool> effect;    /* per key, seen to change   */
        std::map<uint32_t, std::string> action;
    };

    std::map<std::string, BarUse> bars;
    std::vector<std::string> keyless;   /* a bar entry that names no key */

    auto visit = [&](const std::vector<ui_walk::KeyPress> &path) {
        ui_walk::Snapshot before = ui_walk::observe();
        std::vector<HintEntry> bar = hintBar(before.lines);

        if (bar.empty())
            return;

        std::string screen = ui_walk::screenName();

        /*
         * Name the bar by who drew it: a module that owns the keypad draws its
         * own bar over whatever screen is underneath (the channel list over the
         * analyser, the composer over the inbox), and calling that "SPECTRUM"
         * would describe neither.
         */
        std::string owner = screen;
        if (ui_channels_isOpen())      owner = "CHANNELS";
        else if (ui_contacts_isOpen()) owner = "CONTACTS";
        else if (ui_zones_isOpen())    owner = "ZONES";
        else if (ui_tools_isOpen())    owner = "TOOLS";
        else if (ui_msgs_isScreen(state.ui_screen)) owner = "MESSAGES";
        else if (ui_aprs_isScreen(state.ui_screen)) owner = "APRS";

        /* The main screens have no hint row: their bottom row is the S-meter,
           or the carrier search's status line while it runs (docs/UI.md, and
           ui_hints_for_screen() returns NULL for them). */
        if ((screen == "MAIN_VFO") || (screen == "MAIN_MEM"))
            return;

        std::string barId = owner;

        for (const HintEntry &e : bar)
            barId += "|" + std::string(keyName(e.key)) + ":" + e.action;

        BarUse &use = bars[barId];
        use.states++;

        if (use.report.empty()) {
            use.owner  = owner;
            use.report = owner;
            for (const HintEntry &e : bar)
                use.report += " " + std::string(keyName(e.key)) + "=\""
                            + e.action + "\"";
        }

        for (const HintEntry &e : bar) {
            bool isBarKey = false;
            for (uint32_t k : BAR_KEYS)
                if (k == e.key)
                    isBarKey = true;

            if (!isBarKey) {
                /* An entry with no key after it, or a label the bar does not
                   use: the bar itself is wrong */
                keyless.push_back(owner + ": entry \"" + e.action
                                  + "\" has no key");
                continue;
            }

            use.action[e.key] = e.action;

            /* Already seen to work here: no need to press it again */
            if ((use.effect.count(e.key) != 0) && use.effect[e.key])
                continue;

            ui_walk::replay(path);
            ui_walk::pressKey(e.key, false);
            ui_walk::Snapshot after = ui_walk::observe();

            if (after.digest != before.digest)
                use.effect[e.key] = true;
            else
                use.effect[e.key] = use.effect.count(e.key)
                                  ? use.effect[e.key] : false;
        }
    };

    int nodes = ui_walk::walk(visit);

    std::vector<std::string> dead;
    std::vector<std::string> report;

    for (const auto &kv : bars) {
        const BarUse &use = kv.second;

        report.push_back(std::to_string(use.states) + " states  " + use.report);

        for (const auto &e : use.action) {
            if (use.effect.count(e.first) && use.effect[e.first])
                continue;

            const Exempt *ex = nullptr;
            for (const Exempt &x : EXEMPT)
                if ((use.owner == x.owner) && (e.first == x.key))
                    ex = &x;

            if (ex != nullptr) {
                report.push_back("exempt  " + use.report + ": "
                                 + keyName(e.first) + " -- " + ex->why);
                continue;
            }

            dead.push_back(use.report + ": " + keyName(e.first) + " is "
                           "documented as \"" + e.second + "\" and changes "
                           "nothing in any of the " + std::to_string(use.states)
                           + " states that show this bar");
        }
    }

    if (getenv("UI_KEYS_REPORT") != nullptr) {
        printf("[keys] walked %d states, %zu bars\n", nodes, bars.size());
        for (const std::string &s : report)
            printf("[keys]   %s\n", s.c_str());
        fflush(stdout);
    }

    INFO("walked " << nodes << " states, " << bars.size() << " bars");

    std::string list;
    for (const std::string &d : keyless)
        list += "\n  " + d;
    for (const std::string &d : dead)
        list += "\n  " + d;

    if (!list.empty())
        FAIL(list);

    /* The walk is measuring something: the interface has many bars, and every
       one of them was read from a screen the keys reached. */
    REQUIRE(nodes > 200);
    REQUIRE(bars.size() >= 25);
}
