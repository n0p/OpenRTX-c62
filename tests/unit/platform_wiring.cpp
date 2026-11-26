/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file platform_wiring.cpp
 * @brief The guarantees that are not a function: that the thing which has to be
 *        called is called, from the one place it has to be called from, and that
 *        the numbers come from the one place that asserts them.
 *
 * Two bugs in this project's history were of exactly this kind:
 *
 *  - a supervisor path that existed, was correct, and was never called from the
 *    main loop: everything compiled, every unit test of the rule passed, and the
 *    radio kept its PA keyed until the battery died;
 *  - a stack size that was written as a literal in the source that creates the
 *    thread, so the compile-time floor under it was bypassed -- the floor was
 *    real, the source did not use it, and the radio overflowed at 4 KiB.
 *
 * Neither is a property of a value the test can compute: they are properties of
 * the source text.  This test reads the sources (their paths come from meson,
 * so it works from any build directory) and fails when the call, the guard or
 * the include disappears.  It is deliberately textual: it is checking that a
 * line somebody could delete is still there.
 */

#include <cstdio>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifndef PLAT_SRC_ROOT
#error "PLAT_SRC_ROOT is not defined: meson passes the source root here"
#endif

/*
 * This Catch2 has no REQUIRE_MESSAGE: the message is an INFO, and it is what a
 * failing assertion prints when the source it read stops looking like this.
 */
#define REQUIRE_MSG(cond, msg) do { INFO(msg); REQUIRE(cond); } while (0)

namespace
{

std::string readSource(const std::string &relative)
{
    const std::string path = std::string(PLAT_SRC_ROOT) + "/" + relative;
    FILE *f = std::fopen(path.c_str(), "rb");
    REQUIRE_MSG(f != nullptr, "cannot open " << path);

    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    std::fclose(f);

    REQUIRE_MSG(!text.empty(), "empty source: " << path);
    return text;
}

/// How many times `needle` appears in `hay`.
size_t countOf(const std::string &hay, const std::string &needle)
{
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos)
    {
        n++;
        at += needle.size();
    }
    return n;
}

/// The text of one line containing `needle` (or "" when it does not appear).
std::string lineWith(const std::string &hay, const std::string &needle)
{
    const size_t at = hay.find(needle);
    if (at == std::string::npos)
        return "";

    size_t start = hay.rfind('\n', at);
    start = (start == std::string::npos) ? 0 : start + 1;
    size_t end = hay.find('\n', at);
    if (end == std::string::npos)
        end = hay.size();

    return hay.substr(start, end - start);
}

/// True when `#define name` in `src` takes a number, i.e. bypasses the header.
bool definesNumber(const std::string &src, const std::string &name)
{
    const std::string line = lineWith(src, "#define " + name + " ");
    if (line.empty())
        return false;

    /* The character after the name and its spaces */
    size_t at = line.find(name) + name.size();
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
        at++;

    return (at < line.size()) && (line[at] >= '0' && line[at] <= '9');
}

} // namespace

TEST_CASE("the main loop supervises the transmitter and feeds the watchdog",
          "[wiring]")
{
    const std::string src = readSource("openrtx/src/core/threads.c");

    /*
     * rtx_supervise() is the only thing that unkeys a PA whose owning thread is
     * stuck.  It runs in the main thread, and the main thread is also the only
     * thing that feeds the hardware watchdog: if the call goes away, both the
     * transmitter and the reset path go with it.
     */
    REQUIRE_MSG(countOf(src, "rtx_supervise()") == 1,
                    "the device thread has to call rtx_supervise(), once");

    /* The feed is the branch of the UI rule, not a bare call: the point of the
     * rule is that the watchdog is left to reset when the UI does not advance. */
    REQUIRE_MSG(countOf(src, "platform_wdtFeed()") == 1,
                    "the watchdog is fed from the device thread and nowhere else");

    const size_t feedAt = src.find("platform_wdtFeed()");
    REQUIRE(feedAt != std::string::npos);

    /* The lines before the feed have to carry the rule and its
     * branch: feed is what uiw_service() returned, and the feed only happens
     * when it is true. */
    const size_t window = 1200;
    const size_t from = (feedAt > window) ? feedAt - window : 0;
    const std::string before = src.substr(from, feedAt - from);
    REQUIRE_MSG(before.find("uiw_service(") != std::string::npos,
                    "the feed has to follow the UI rule (core/ui_watchdog.h)");
    REQUIRE_MSG(before.find("if (feed)") != std::string::npos,
                    "the feed has to be conditional on the UI advancing");

    /* The rule itself cannot live in the loop any more: a rule written inline
     * is a rule no test can reach, which is how the supervisor bug happened. */
    REQUIRE_MSG(src.find("core/ui_watchdog.h") != std::string::npos,
                    "the main loop includes the rule it runs");

    /* And it has to be the same call the tests walk. */
    REQUIRE(countOf(src, "uiw_service(") == 1);
}

TEST_CASE("every thread takes its stack size from the budget header",
          "[wiring][stacks]")
{
    struct threadSource
    {
        const char *path;
        const char *macro; ///< The local name, as the source uses it
    };

    const threadSource sources[] = {
        { "platform/drivers/keyboard/keyboard_c62.c", "KBD_STACK" },
        { "platform/drivers/audio/csk6_codec.c", "WORKER_STACK_SIZE" },
        { "platform/targets/c62/pc_proto_c62.c", "THREAD_STACK" },
        { "platform/targets/c62/blackbox_c62.c", "BB_SUP_STACK" },
        { "openrtx/src/protocols/TETRA/TetraSpeech.c", "THREAD_STACK" },
        { "openrtx/src/protocols/DSTAR/DStarSpeech.c", "THREAD_STACK" },
        { "openrtx/src/protocols/DMR/DmrSpeech.c", "THREAD_STACK" },
    };

    for (const threadSource &t : sources)
    {
        INFO("source: " << t.path);
        const std::string src = readSource(t.path);

        /* The header is where the floor is asserted.  A source that does not
         * include it has no floor, whatever the header says. */
        REQUIRE_MSG(src.find("core/stack_budget.h") != std::string::npos,
                        "the thread has to include core/stack_budget.h");

        /* A literal of its own is how the DMR voice thread got 4 KiB: it is
         * not that the number was wrong, it is that nobody was asserting it. */
        REQUIRE_MSG(!definesNumber(src, t.macro),
                        t.macro << " has to come from the budget header, not a "
                                   "literal in this file");

        /* And it really is the header that defines it. */
        const std::string line = lineWith(src, "#define " + std::string(t.macro) + " ");
        REQUIRE_MSG(line.find("STACK_BUDGET_") != std::string::npos,
                        t.macro << " has to be defined from a STACK_BUDGET_* "
                                   "constant");
    }
}

TEST_CASE("the bench token gate stays on every radio-moving message",
          "[wiring][bench]")
{
    /*
     * The token is the only thing between a stale client (a second HELLO
     * invalidates the first one's) and a keyed PA.  One handler that forgets
     * benchTokenOk() is a whole session that can move the radio without a
     * token, and the calls are scattered: this counts them.
     *
     * ARM, TUNE, PTT, CAPTURE, RSSI and SWEEP all move the radio or hand out
     * its audio; the report and the state reads do not.
     */
    const std::string src = readSource("openrtx/src/core/pc_proto_bench.c");

    /* One per handler, plus the definition itself. */
    const size_t calls = countOf(src, "benchTokenOk(r)");
    REQUIRE_MSG(calls == 6,
                    "six radio-moving bench messages need the token gate, "
                    "found " << calls << " uses");

    /* The gate answers NOT_ALLOWED, not BAD_REQUEST: the client has to be able
     * to tell "wrong token, re-HELLO" from "malformed frame". */
    const std::string gate = lineWith(src, "r->err = PC_ERR_NOT_ALLOWED;");
    REQUIRE_MSG(!gate.empty(), "the token gate sets PC_ERR_NOT_ALLOWED");
}

TEST_CASE("the audio thread stacks in core/threads.h keep their floors",
          "[wiring][stacks]")
{
    /*
     * The rtx thread has tests/unit/rtx_stack_budget.cpp with the #if
     * combinations; this is the cheap guard for the rest of the file: the
     * speech decoder (codec2) and the audio thread are also on a fixed budget,
     * and a change of those numbers is a change of the RAM map.
     */
    const std::string src = readSource("openrtx/include/core/threads.h");

    REQUIRE(countOf(src, "#define RTX_THREAD_STKSIZE") == 4);
    REQUIRE(countOf(src, "#define CODEC2_THREAD_STKSIZE") == 1);
    REQUIRE(countOf(src, "#define AUDIO_THREAD_STKSIZE") == 1);
}
