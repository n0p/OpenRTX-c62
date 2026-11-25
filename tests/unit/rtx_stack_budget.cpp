/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file rtx_stack_budget.cpp
 * @brief The radio thread's stack has to fit the deepest chain of the build.
 *
 * The size in core/threads.h is chosen by #if on the modes that run in the rtx
 * thread, and that choice has bitten once: the TETRA branch came first and
 * said 4 KiB, the DMR transmitter needs about 7.1 KiB for
 * update() -> txSlot() -> fillMic() -> dmrEnc_encode() -> enc_ambe() ->
 * e_p() -> pitch_est(), and the bench image has both modes on.  A stack that
 * small is not a slow transmitter: the first frame past the guard is a
 * CONFIG_HW_STACK_PROTECTION fatal error, Zephyr halts, nothing reaches the
 * serial port (the thread that writes it is gone too) and the application
 * watchdog resets the radio seconds later.  That is what keying in DMR looked
 * like from the outside, and it is why this test exists: the combination that
 * failed is compiled here on purpose.
 *
 * The numbers come from the build itself (-Os, Cortex-M33, -fstack-usage);
 * tests/host/dmr_tx_host/stack.sh reproduces the measurement.
 */

#include <cstddef>

#include <catch2/catch_test_macros.hpp>

/*
 * threads.h is a plain #if on these macros, so the layouts the radio ships in
 * are all compiled here by clearing the include guard between them.  Nothing
 * else is included in this unit, so the definitions cannot disturb it.
 */
#define PLATFORM_C62 1

/* 1. Every mode at once: the bench image.  This is the one that overflowed. */
#define CONFIG_TETRA 1
#define CONFIG_DMR_TX 1
#include "core/threads.h"
static_assert(RTX_THREAD_STKSIZE >= 9216,
              "TETRA + DMR_TX: the encoder chain needs about 7.1 KiB");
static constexpr size_t kBothSizes = RTX_THREAD_STKSIZE;

/* 2. The transmitter without TETRA: same chain, same size. */
#undef THREADS_H
#undef RTX_THREAD_STKSIZE
#undef CONFIG_TETRA
#include "core/threads.h"
static_assert(RTX_THREAD_STKSIZE >= 9216,
              "DMR_TX alone still encodes in the rtx thread");
static constexpr size_t kDmrSizes = RTX_THREAD_STKSIZE;

/* 3. TETRA alone: libtetra's decode chain is 1.5-2.5 KiB.  Whatever the size
 *    is, it has to fit that, or receiving TETRA dies the same way. */
#undef THREADS_H
#undef RTX_THREAD_STKSIZE
#undef CONFIG_DMR_TX
#define CONFIG_TETRA 1
#include "core/threads.h"
static_assert(RTX_THREAD_STKSIZE >= 4096, "TETRA decodes in the rtx thread");
static constexpr size_t kTetraSize = RTX_THREAD_STKSIZE;

/* 4. Neither: the analogue modes, where 1 KiB is the old figure.  Only a
 *    sanity floor: the modes that matter here are the three above. */
#undef THREADS_H
#undef RTX_THREAD_STKSIZE
#undef CONFIG_TETRA
#include "core/threads.h"
static_assert(RTX_THREAD_STKSIZE >= 1024, "the receive-only floor");
static constexpr size_t kBaseSize = RTX_THREAD_STKSIZE;

TEST_CASE("the rtx stack covers the deepest chain of the build", "[rtx]")
{
    /*
     * The assertions that matter are the static_asserts above: they fail the
     * build, not the test.  This case publishes the four figures and keeps the
     * file a test for the suite's purposes.
     */
    REQUIRE(kBothSizes == 9216);  /* TETRA + DMR_TX: what the radio ships */
    REQUIRE(kDmrSizes == 9216);   /* DMR_TX alone                        */
    REQUIRE(kTetraSize >= 4096);  /* TETRA alone                         */
    REQUIRE(kBaseSize >= 1024);   /* neither                             */
}
