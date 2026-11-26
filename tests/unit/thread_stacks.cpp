/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file thread_stacks.cpp
 * @brief Every thread that is not the rtx or the UI thread has a stack, and
 *        every one of those stacks has a floor under it.
 *
 * The two overruns that cost this project the most time were both on a thread
 * nobody had measured: the DMR voice thread at 4 KiB died with the PC inside
 * dmr_rfft256, and the rtx thread took the TETRA branch (4 KiB) in a build that
 * also had the DMR transmitter.  The rtx thread has its own test
 * (tests/unit/rtx_stack_budget.cpp) and core/threads.h; this one covers the
 * threads that are created elsewhere -- keypad, codec worker, PC protocol,
 * black box supervisor and the speech playback threads.
 *
 * The assertions that matter are the static_asserts in core/stack_budget.h:
 * they fail the build, not the test, and they fail it for the firmware and for
 * this host test at the same time because both take the numbers from the same
 * place.  This file adds what a static assert cannot say: how much of each
 * stack is measured to be in use, so the margin is visible instead of trusted,
 * and a test that a change of the numbers keeps it.
 *
 * `measured` below is one of two things, and the column says which:
 *
 *  - kHighWater: the deepest use observed in a build (or the project's own
 *    measurement when the thread was written).  The stack has to hold it with a
 *    quarter spare and the floor may not come below it plus a tenth.
 *  - kNeed: the requirement itself, measured as the depth of a call chain that
 *    has to fit.  There is no spare to ask for: the DMR voice thread ships at
 *    its need because the DTCM budget has nothing left, so the rule is only
 *    that neither the stack nor its floor may go below the need.
 *
 * `measured == 0` means it was not measured on this machine (the keypad scanner
 * and the TETRA speech thread need the radio: libopenacelp is not in this
 * tree).  Their floor is the shipped value, so the build can only refuse to go
 * lower -- the black box's thread walk (`free` per thread, docs/BLACKBOX.md) is
 * what closes them.
 */

#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "core/stack_budget.h"

namespace
{

enum measureKind
{
    kNone,      ///< Not measured here
    kHighWater, ///< Deepest use seen: the stack keeps a quarter spare
    kNeed       ///< The depth that has to fit: no spare can be promised
};

struct threadStack
{
    const char *name;
    size_t      bytes;    ///< What ships
    size_t      floor;    ///< The compile-time minimum
    size_t      measured; ///< Use, in the sense of `kind`
    measureKind kind;
};

/*
 * One row per thread.  Adding a thread without a row here is not something the
 * compiler can see, but tests/unit/platform_wiring.cpp does look for it: every
 * K_THREAD_DEFINE / k_thread_create in the C62 sources has to take its size
 * from core/stack_budget.h.
 */
const threadStack kStacks[] = {
    /* name                  ships                          floor */
    { "kbd_thread",          STACK_BUDGET_KBD,
      STACK_BUDGET_KBD_MIN,          0,    kNone },
    { "csk6 codec worker",   STACK_BUDGET_CODEC_WORKER,
      STACK_BUDGET_CODEC_WORKER_MIN, 922,  kHighWater }, /* -fcallgraph-info=su */
    { "pcproto",             STACK_BUDGET_PC_PROTO,
      STACK_BUDGET_PC_PROTO_MIN,     560,  kHighWater }, /* rx 160 + info 400  */
    { "bb_supervisor",       STACK_BUDGET_BB_SUPERVISOR,
      STACK_BUDGET_BB_SUPERVISOR_MIN, 2048, kHighWater }, /* record + flash    */
    { "TETRA speech",        STACK_BUDGET_TETRA_SPEECH,
      STACK_BUDGET_TETRA_SPEECH_MIN, 0,    kNone },
    { "D-STAR speech",       STACK_BUDGET_DSTAR_SPEECH,
      STACK_BUDGET_DSTAR_SPEECH_MIN, 2704, kHighWater }, /* parms_repeat_model */
    { "DMR speech (own)",    STACK_BUDGET_DMR_SPEECH_OWN,
      STACK_BUDGET_DMR_SPEECH_OWN_MIN, 7168, kNeed },    /* dmr_rfft256 chain  */
    { "DMR speech (mbelib)", STACK_BUDGET_DMR_SPEECH_MBELIB,
      STACK_BUDGET_DMR_SPEECH_MBELIB_MIN, 8192, kNeed },
    { "P25 speech",          STACK_BUDGET_IMBE_SPEECH,
      STACK_BUDGET_IMBE_SPEECH_MIN,   5872, kHighWater }, /* imbe_irfft256    */
    { "YSF speech",          STACK_BUDGET_IMBE_SPEECH,
      STACK_BUDGET_IMBE_SPEECH_MIN,   5872, kHighWater },
    { "NXDN speech",         STACK_BUDGET_AMBE_SPEECH,
      STACK_BUDGET_AMBE_SPEECH_MIN,   2664, kHighWater }, /* dmr_rfft256     */
    { "dPMR speech",         STACK_BUDGET_AMBE_SPEECH,
      STACK_BUDGET_AMBE_SPEECH_MIN,   2664, kHighWater },
    { "packet modem",        STACK_BUDGET_PACKET,
      STACK_BUDGET_PACKET_MIN,        0,    kNone },
    { "c62 remote",          STACK_BUDGET_REMOTE,
      STACK_BUDGET_REMOTE_MIN,        0,    kNone },
    { "baseband dump",       STACK_BUDGET_DUMP_SENDER,
      STACK_BUDGET_DUMP_SENDER_MIN,   0,    kNone },
};

constexpr size_t kCount = sizeof(kStacks) / sizeof(kStacks[0]);

} // namespace

TEST_CASE("every thread stack has a floor the build enforces", "[stacks]")
{
    for (size_t i = 0; i < kCount; i++)
    {
        const threadStack &s = kStacks[i];

        INFO("thread: " << s.name);
        REQUIRE(s.floor > 0);
        REQUIRE(s.bytes >= s.floor);

        /* The kernel stacks are aligned: a size that is not a multiple of the
         * minimum alignment would eat part of the last frame. */
        REQUIRE((s.bytes % 8) == 0);
        REQUIRE((s.floor % 8) == 0);
    }
}

TEST_CASE("a high-water mark leaves a quarter of the stack spare", "[stacks]")
{
    for (size_t i = 0; i < kCount; i++)
    {
        const threadStack &s = kStacks[i];
        if (s.kind != kHighWater)
            continue;

        INFO("thread: " << s.name << ", measured " << s.measured
                        << " of " << s.bytes);

        /* A stack that sits on its watermark fails with the next function
         * added above it, and that failure is a halted MCU, not a warning. */
        REQUIRE(s.bytes >= s.measured + s.measured / 4);
        /* The floor is the promise: never below the measurement plus a tenth,
         * the same figure the DMR voice thread overflowed inside of. */
        REQUIRE(s.floor >= s.measured + s.measured / 10);
    }
}

TEST_CASE("a measured need is the floor, and the stack may not go below it",
          "[stacks]")
{
    for (size_t i = 0; i < kCount; i++)
    {
        const threadStack &s = kStacks[i];
        if (s.kind != kNeed)
            continue;

        INFO("thread: " << s.name << ", needs " << s.measured);
        REQUIRE(s.bytes >= s.measured);
        REQUIRE(s.floor >= s.measured);
    }
}

TEST_CASE("the unmeasured threads cannot be lowered at all", "[stacks]")
{
    /*
     * Not measured here -- the keypad scanner (Zephyr GPIO and the logging
     * path) and the TETRA speech thread (libopenacelp is not in this tree).
     * For those two the floor is the value the radio ships with, so the build
     * refuses to go lower and the high-water mark from the black box is what
     * would justify raising it.
     */
    size_t unmeasured = 0;

    for (size_t i = 0; i < kCount; i++)
    {
        if (kStacks[i].kind != kNone)
            continue;

        INFO("thread: " << kStacks[i].name);
        REQUIRE(kStacks[i].floor == kStacks[i].bytes);
        unmeasured++;
    }

    /* If this changes, a thread gained or lost a measurement: the table has
     * to say which, not just the count. */
    REQUIRE(unmeasured == 5);
}

TEST_CASE("no speech thread gives its vocoder the stack that already overflowed",
          "[stacks]")
{
    /*
     * 4 096 B is the number that is not allowed to come back: it is the size
     * the DMR voice thread had when the black box caught K_ERR_STACK_CHK_FAIL
     * with the PC in dmr_rfft256 and the LR in synth_unvoiced.  A playback
     * thread that decodes a vocoder into PCM needs the vocoder's own frames on
     * top of its own loop.
     */
    constexpr size_t kOverflowed = 4096;

    /* DMR, whose synthesis overflowed at that size: more, not equal. */
    REQUIRE(STACK_BUDGET_DMR_SPEECH_OWN > kOverflowed);
    REQUIRE(STACK_BUDGET_DMR_SPEECH_MBELIB > kOverflowed);

    /* D-STAR goes through the same 1 KiB-scratch dmr_rfft256, and
     * parms_repeat_model alone puts 2.4 KiB on the frame above it: 4 KiB is
     * enough (measured 2 704), and the floor says how much lower is not. */
    REQUIRE(STACK_BUDGET_DSTAR_SPEECH >= kOverflowed);
    REQUIRE(STACK_BUDGET_DSTAR_SPEECH_MIN >= 3 * 1024);

    /* TETRA's decoder is not in this tree and its stack is malloc'd -- there
     * is no guard page behind it -- so it gets the same figure, never less. */
    REQUIRE(STACK_BUDGET_TETRA_SPEECH >= kOverflowed);
    REQUIRE(STACK_BUDGET_TETRA_SPEECH_MIN == STACK_BUDGET_TETRA_SPEECH);
}

TEST_CASE("the protocol and the supervisor keep a working margin", "[stacks]")
{
    /*
     * Both run behind a caller that cannot be allowed to fail quietly: the
     * transport thread is the cable, and the supervisor is the only thread that
     * writes the post-mortem record and keeps feeding the watchdog.
     */
    REQUIRE(STACK_BUDGET_PC_PROTO >= 3072);
    REQUIRE(STACK_BUDGET_BB_SUPERVISOR >= 2560);
    /* The codec worker holds the audio lock across the DSP calls: small on
     * purpose, but not down to its own measurement. */
    REQUIRE(STACK_BUDGET_CODEC_WORKER >= 1536);
}
