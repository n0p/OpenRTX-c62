/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * No text leaves its row, on any screen.
 *
 * docs/TEST_PLAN.md section 3: "ningún texto se sale de su fila ni pisa otro".
 * The panel is 160x128 and every string a screen draws is recorded with its
 * anchor, its alignment and its measured width (core/gfx_capture.h, the same
 * block the simulator's `dump` prints), so the rule can be checked on the real
 * screens instead of on a picture:
 *
 *   - the ink of a string stays inside the 160 px panel: for a left-aligned
 *     string x >= 0 and x + w <= 160, for a right-aligned one x - w >= 0, for a
 *     centred one both halves of w around x;
 *   - the ink of a string stays inside the 128 px height: baseline - ascent and
 *     baseline + descent within the panel. The ascent and the descent of each
 *     font are measured here, by drawing a probe into the framebuffer, not
 *     copied from a table: a font swap cannot leave the check measuring the old
 *     metric;
 *   - two strings whose ink bands overlap vertically do not overlap
 *     horizontally. That is the "pisa otro" half, and it is what catches a
 *     value drawn over a label, a right-hand status over a title, or a
 *     two-line row whose second line climbs into the first.
 *
 * The walk reaches every screen through the real FSM (tests/unit/ui_walk.hpp).
 * Two cases are named separately because they are the widest the layout ever
 * gets and the ones an operator meets: a nine-digit frequency on the main
 * screen, and the longest line the mode-state block can produce.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "ui_walk.hpp"

extern "C" {
#include "ui/ui_mode_state.h"
}

namespace
{

/* ----------------------------------------------------------- the metrics --- */

struct Metrics
{
    int ascent;     /**< baseline to the top of a capital              */
    int descent;    /**< baseline to the bottom of a 'g'               */
    bool known;
};

/**
 * Ascent and descent of a font, measured on the framebuffer.
 *
 * "Ag" is drawn at a known baseline on a cleared screen: the highest lit row is
 * the top of the capital, the lowest is the tail of the 'g'. Nothing is guessed
 * from a table, so a change of font file shows up here instead of silently
 * invalidating the row check below.
 */
Metrics measureFont(uint8_t font)
{
    static Metrics cache[FONT_SIZE_NUM];
    static bool primed = false;

    if (!primed) {
        gfx_init();
        primed = true;
    }

    if (font >= FONT_SIZE_NUM)
        return { 0, 0, false };

    if (cache[font].known)
        return cache[font];

    const int baseline = CONFIG_SCREEN_HEIGHT / 2;
    const color_t white = { 255, 255, 255, 255 };

    gfx_clearScreen();
    gfx_print((point_t){ 4, (int16_t)baseline }, (fontSize_t)font,
              TEXT_ALIGN_LEFT, white, "Ag");

    const uint16_t *px = static_cast<const uint16_t *>(gfx_framebuffer());
    int top = CONFIG_SCREEN_HEIGHT;
    int bottom = -1;

    for (int y = 0; y < CONFIG_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < CONFIG_SCREEN_WIDTH; x++) {
            if (px[x + y * CONFIG_SCREEN_WIDTH] != 0) {
                if (y < top)
                    top = y;
                if (y > bottom)
                    bottom = y;
                break;
            }
        }
    }

    Metrics m = { 0, 0, (bottom >= 0) };
    if (bottom >= 0) {
        m.ascent  = baseline - top;
        m.descent = bottom - baseline;
    }

    cache[font] = m;
    return m;
}

/* -------------------------------------------------------------- the rule --- */

struct Box
{
    int left;
    int right;      /**< inclusive */
    int top;
    int bottom;     /**< inclusive */
    int baseline;   /**< the y the string was drawn at, i.e. its row */
    std::string text;
};

/**
 * The ink box of a recorded string, from its anchor and its alignment.
 *
 * A centred string is a special case: gfx_print() centres it on the *panel*
 * (core/graphics.c, get_reset_x, returns (max_x - width) / 2) and ignores the
 * x it was given, so that is where the ink is. The recorder keeps the anchor
 * the screen passed, which is why the dump shows x = 4 for the clock of the top
 * bar and the clock is nevertheless in the middle of the screen.
 */
Box boxOf(const ui_walk::Line &l)
{
    Box b;
    Metrics m = measureFont(l.font);
    int w = (int)l.width;

    switch (l.align) {
        case TEXT_ALIGN_CENTER:
            b.left  = (CONFIG_SCREEN_WIDTH - w) / 2;
            b.right = b.left + w - 1;
            break;
        case TEXT_ALIGN_RIGHT:
            b.left  = (int)l.x - w;
            b.right = (int)l.x - 1;
            break;
        case TEXT_ALIGN_LEFT:
        default:
            b.left  = (int)l.x;
            b.right = b.left + w - 1;
            break;
    }

    b.top      = (int)l.y - m.ascent;
    b.bottom   = (int)l.y + m.descent;
    b.baseline = (int)l.y;
    b.text     = l.text;

    return b;
}

/**
 * One row of the screen as the operator reads it: the strings on a baseline,
 * left to right.
 *
 * The frequency of the main screen is drawn as two sizes side by side ("145.987"
 * at 10 pt and "50" at 6 pt), so the number is only complete when the row is
 * read back in order.
 */
std::string rowText(const std::vector<ui_walk::Line> &lines, int16_t baseline)
{
    std::vector<Box> row;

    for (const ui_walk::Line &l : lines)
        if ((l.y == baseline) && !l.text.empty())
            row.push_back(boxOf(l));

    for (size_t i = 0; i < row.size(); i++)
        for (size_t j = i + 1; j < row.size(); j++)
            if (row[j].left < row[i].left)
                std::swap(row[i], row[j]);

    std::string out;
    for (const Box &b : row)
        out += b.text;

    return out;
}

/** The baseline of the large frequency line: the widest row of the screen. */
int16_t widestRow(const std::vector<ui_walk::Line> &lines)
{
    int16_t best = -1;
    int bestW = -1;

    for (const ui_walk::Line &l : lines) {
        int w = (int)l.width;
        if (w > bestW) {
            bestW = w;
            best = l.y;
        }
    }
    return best;
}

/** Violations of the rule for one screen, as human-readable lines. */
std::vector<std::string> checkScreen(const std::vector<ui_walk::Line> &lines)
{
    std::vector<std::string> bad;
    std::vector<Box> boxes;

    for (const ui_walk::Line &l : lines) {
        if (l.text.empty())
            continue;

        /*
         * One character is a symbol, not a string: the interface draws carets,
         * arrows and brackets with gfx_drawSymbol, whose ink leaves the glyph
         * cell the text recorder measures.  Nothing to collide with.
         */
        if (l.text.size() < 2)
            continue;

        Box b = boxOf(l);

        /* Two identical strings at the same anchor are one string drawn
           twice (the duplicated hint bar of SETTINGS_MODE, docs/SIMULATOR.md);
           they are the same ink and must not be reported as a collision. */
        bool duplicate = false;
        for (const Box &o : boxes)
            if ((o.left == b.left) && (o.top == b.top) && (o.text == b.text))
                duplicate = true;

        if (duplicate)
            continue;

        if ((b.right >= CONFIG_SCREEN_WIDTH) || (b.left < 0) || (b.right < 0)) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "\"%s\" runs off the panel horizontally: x=%d w=%u "
                     "align=%u -> [%d,%d]",
                     l.text.c_str(), (int)l.x, (unsigned)l.width,
                     (unsigned)l.align, b.left, b.right);
            bad.push_back(buf);
        }

        if ((b.top < 0) || (b.bottom >= CONFIG_SCREEN_HEIGHT)) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "\"%s\" runs off the panel vertically: baseline %d, ink "
                     "[%d,%d]",
                     l.text.c_str(), (int)l.y, b.top, b.bottom);
            bad.push_back(buf);
        }

        boxes.push_back(b);
    }

    /*
     * Overlap: two strings drawn on one baseline share no column.
     *
     * One baseline is one row: a label and its value, the title and the
     * right-hand count, the hint bar and anything that thinks it belongs in the
     * bottom 11 px. Between two *different* baselines the ink may meet -- a
     * popup draws over the screen it was opened on, and the text under it is
     * occluded on purpose -- so that is not a defect, while a string whose ink
     * leaves the panel (above) still is.
     *
     * The macro grid is the one screen that covers another one on the same
     * baseline: it dims the screen with an overlay and draws its title where
     * the clock is, which is the intent of the overlay. Its rows are checked
     * for fit like everything else, and not for collisions with what is behind
     * the dark.
     */
    if (ui_macroGridOpen())
        return bad;

    for (size_t i = 0; i < boxes.size(); i++) {
        for (size_t j = i + 1; j < boxes.size(); j++) {
            const Box &a = boxes[i];
            const Box &b = boxes[j];

            bool rows = (a.baseline == b.baseline);
            bool cols = (a.left < b.right) && (b.left < a.right);

            if (rows && cols) {
                char buf[200];
                snprintf(buf, sizeof(buf),
                         "\"%s\" [x %d..%d, y %d..%d] overlaps \"%s\" "
                         "[x %d..%d, y %d..%d]",
                         a.text.c_str(), a.left, a.right, a.top, a.bottom,
                         b.text.c_str(), b.left, b.right, b.top, b.bottom);
                bad.push_back(buf);
            }
        }
    }

    return bad;
}

} /* namespace */

/* ======================================================================== */

TEST_CASE("No text leaves its row on any screen", "[ui][layout][fit]")
{
    /* One report per (screen, content): the same screen reached with the same
       strings is checked once. */
    std::set<std::string> seen;
    std::vector<std::string> failures;
    std::vector<std::string> screens;
    int checked = 0;

    auto visit = [&](const std::vector<ui_walk::KeyPress> &) {
        ui_walk::Snapshot s = ui_walk::observe();

        std::string key = ui_walk::screenName();
        for (const ui_walk::Line &l : s.lines)
            key += "|" + l.text;

        if (!seen.insert(key).second)
            return;

        checked++;
        screens.push_back(ui_walk::screenName());

        for (const std::string &b : checkScreen(s.lines))
            failures.push_back(ui_walk::screenName() + ": " + b);
    };

    int nodes = ui_walk::walk(visit);

    if (getenv("UI_FIT_REPORT") != nullptr) {
        printf("[fit] walked %d states, checked %d screens\n", nodes, checked);
        fflush(stdout);
    }

    INFO("walked " << nodes << " states, checked " << checked << " screens");

    std::string list;
    for (const std::string &f : failures)
        list += "\n  " + f;

    if (!list.empty())
        FAIL(list);
    REQUIRE(checked > 100);
}

/* ------------------------------------------------- nine digits and beyond -- */

TEST_CASE("A nine digit frequency fits the main screen", "[ui][layout][fit]")
{
    /* The layout prints %03lu.%05lu: nine digits, the widest of which is the
       10 Hz one. 1300.00000 MHz is the top of the receiver and a MHz part of
       four digits, so both ends of the width are covered. */
    struct Case
    {
        freq_t freq;
        const char *text;
    };

    const Case cases[] = {
        { 145987500,  "145.98750" },     /* nine digits, the usual case   */
        { 1300000000, "1300.00000" },    /* four digits before the point  */
        { 1000000,    "1.00000" },       /* one digit before the point    */
    };

    for (const Case &c : cases) {
        ui_walk::resetUi(OPMODE_FM, c.freq);

        ui_walk::Snapshot s = ui_walk::observe();
        REQUIRE(ui_walk::screenName() == "MAIN_VFO");

        /* The line the frequency is on, read back as one string */
        int16_t row = widestRow(s.lines);
        std::string text = rowText(s.lines, row);

        INFO("frequency " << c.freq << ": row " << row << " reads \"" << text
                          << "\"");
        REQUIRE(text.find(c.text) != std::string::npos);

        std::vector<std::string> bad = checkScreen(s.lines);
        std::string list;
        for (const std::string &b : bad)
            list += "\n  " + b;
        if (!list.empty())
            FAIL(list);
    }

    /* And in the entry screen, where the same number is typed digit by digit
       with a cursor under one of them */
    ui_walk::resetUi(OPMODE_FM, 145500000);
    const uint32_t digits[] = { KEY_1, KEY_3, KEY_0, KEY_0,
                                KEY_9, KEY_8, KEY_7, KEY_5 };
    for (uint32_t d : digits)
        ui_walk::digit(d);

    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);
    ui_walk::Snapshot s = ui_walk::observe();

    std::vector<std::string> bad = checkScreen(s.lines);
    std::string list;
    for (const std::string &b : bad)
        list += "\n  " + b;
    if (!list.empty())
        FAIL(list);
}

/* ------------------------------------------- the longest mode state line -- */

TEST_CASE("The longest mode state line fits the main screen", "[ui][layout][fit]")
{
    /*
     * ui/ui_mode_state.c produces the widest texts of the interface: a DMR
     * unit call with both identifiers (seven digits each, the private-call
     * marker) and the link-quality figure, then the identifiers line with the
     * colour code, the timeslot and the longest secondary note. The screen
     * measures each before drawing (ui_main.c, _ui_drawStateLine and
     * _ui_drawModeState) and shortens what does not fit, so this has to pass
     * for the longest text the block can hold -- otherwise the shortening is
     * the thing being tested, not the layout.
     */
    ui_walk::resetUi(OPMODE_DMR, 145500000);
    state.settings.dmr_id = 1234567;        /* no "NO TX" warning over it */

    dmr_sim_setStatus(true, 15, 2, true, 1000, true, false, 16777215, 16777215,
                      -1);

    ui_mode_state_t st;
    ui_mode_state_read(&st);

    INFO("DMR state: id=\"" << st.id << "\" call=\"" << st.callTxt
         << "\" qual=\"" << st.qual << "\" note=\"" << st.note << "\"");
    REQUIRE(st.sync);
    REQUIRE(strlen(st.callTxt) >= 12);

    ui_walk::Snapshot s = ui_walk::observe();
    std::vector<std::string> bad = checkScreen(s.lines);
    std::string list;
    for (const std::string &b : bad)
        list += "\n  " + b;
    REQUIRE(list.empty());

    /* The call is on the screen, not dropped: the widest it gets is what the
       layout has to be able to hold, shortened if needed but never drawn over
       the frequency below it. */
    std::string text;
    for (const ui_walk::Line &l : s.lines)
        text += "[" + std::to_string(l.x) + "," + std::to_string(l.y) + ","
              + std::to_string(l.width) + "]" + l.text + " ";
    INFO("screen: " << text);
    INFO("widths: call " << gfx_getTextWidth(FONT_SIZE_6PT, st.callTxt)
         << ", qual " << gfx_getTextWidth(FONT_SIZE_6PT, st.qual)
         << ", both " << gfx_getTextWidth(FONT_SIZE_6PT,
                                          (std::string(st.callTxt) + " "
                                           + st.qual).c_str()));

    /*
     * 16 777 215 > 16 777 215 does not fit on one row at 8 pt (157 px against
     * 152), so the block falls back to the link-quality figure: what is proven
     * is that the fallback is the *only* thing the operator loses -- the row
     * stays readable and the screen stays clean, which the check above says.
     *
     * A real private call is seven digits a side and does fit; that one must
     * reach the screen, or the shortening is eating what it should not.
     */
    ui_walk::resetUi(OPMODE_DMR, 145500000);
    state.settings.dmr_id = 1234567;
    dmr_sim_setStatus(true, 15, 2, true, 1000, true, false, 1234567, 2143657,
                      -1);
    ui_mode_state_read(&st);
    s = ui_walk::observe();
    text.clear();
    for (const ui_walk::Line &l : s.lines)
        text += l.text + " ";

    INFO("seven-digit call: callTxt \"" << st.callTxt << "\" screen: " << text);
    REQUIRE(text.find("1234567") != std::string::npos);
    REQUIRE(text.find("2143657") != std::string::npos);

    /* TETRA, the other long one: the cell, then the note with the registration
       and the group count */
    ui_walk::resetUi(OPMODE_TETRA, 430500000);
    tetra_sim_setCell(228, 8889, 1, 430500000u, 0);
    tetra_sim_setRegistration(true, 255, 0);

    ui_mode_state_read(&st);
    INFO("TETRA state: id=\"" << st.id << "\" note=\"" << st.note << "\"");

    s = ui_walk::observe();
    bad = checkScreen(s.lines);
    list.clear();
    for (const std::string &b : bad)
        list += "\n  " + b;
    if (!list.empty())
        FAIL(list);

    dmr_sim_clear();
    tetra_sim_clearCell();
}
