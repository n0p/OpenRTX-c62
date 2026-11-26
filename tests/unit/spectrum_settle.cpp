/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>

extern "C" {
#include "core/spectrum_service.h"
}

/*
 * The analyser's sweep cost, as the service describes it.
 *
 * The fifth integration pass brought the BK4819 sweep from 694 us per point to
 * about 170 us by asking the chip instead of the datasheet (docs/SPECTRUM.md
 * §3): the driver polls the glitch indicator REG_63 and reads the RSSI as soon
 * as it comes down, with a bounded number of polls, and it falls back to the
 * datasheet's 400 us when the indicator does not behave. The operator chooses
 * between the gate and four fixed waits with the ESPERA row of the quick menu.
 *
 * This test is about the contract between that row, the service and the
 * screen, which is all the host can see: the table of waits, the index the
 * menu walks, the default the build asks for, the status the screen reads, and
 * the new 96 MHz span that only fits because the sweep got cheaper. The
 * measurement itself is a radio one (tests/platform/c62_spectrum_cost.c and
 * docs/SPECTRUM.md §3.3).
 */

namespace
{

/* The waits the quick menu offers, in order, exactly as docs/SPECTRUM.md §5
   lists them: AUTO is the chip's own gate, 0 us in the service. */
constexpr uint32_t WAITS[] = { 0, 50, 100, 200, 400 };
constexpr uint8_t WAIT_COUNT = sizeof(WAITS) / sizeof(WAITS[0]);

} /* namespace */

TEST_CASE("the settle row walks the table the driver understands",
          "[spectrum][settle]")
{
    REQUIRE(SPECTRUM_SPAN_COUNT == 9);

    /* The build asks for no fixed wait: the gate, which is the fast path */
    REQUIRE(spectrumService_settleIndex() == 0);
    REQUIRE(spectrumService_settleUs() == 0);

    for (uint8_t i = 0; i < WAIT_COUNT; i++) {
        spectrumService_setSettleIndex(i);
        REQUIRE(spectrumService_settleIndex() == i);
        REQUIRE(spectrumService_settleUs() == WAITS[i]);
    }

    /* Past the end the last entry is kept: the menu's max is WAIT_COUNT - 1,
       and a stored or forged index cannot make the driver wait for nothing */
    spectrumService_setSettleIndex(WAIT_COUNT);
    REQUIRE(spectrumService_settleIndex() == WAIT_COUNT - 1);
    REQUIRE(spectrumService_settleUs() == 400);

    spectrumService_setSettleIndex(200);
    REQUIRE(spectrumService_settleUs() == 400);

    /* The table is monotonic and starts at the gate: a binary search for the
       right wait on the radio only works if it is ordered */
    for (uint8_t i = 1; i < WAIT_COUNT; i++)
        REQUIRE(WAITS[i] > WAITS[i - 1]);

    spectrumService_setSettleIndex(0);
}

TEST_CASE("the status carries what the screen and the menu read",
          "[spectrum][settle]")
{
    constexpr uint32_t CENTRE = 145500000U;

    /* Each case leaves the service as it found it: the tests in this binary run
       in a random order, and an open analyser ignores a second open() */
    spectrumService_close();
    spectrumService_setSpanIndex(2);
    spectrumService_setSettleIndex(0);

    REQUIRE(spectrumService_open(CENTRE));
    REQUIRE(spectrumService_active());

    spectrumStatus_t st;
    spectrumService_status(&st);

    /* The span the screen shows and the default it opens on: 1 MHz, 41 bins of
       25 kHz, which is what docs/SPECTRUM.md's measured figures are for */
    REQUIRE(st.centre == CENTRE);
    REQUIRE(st.span == 1000000);
    REQUIRE(st.step == 25000);
    REQUIRE(st.bins == 41);
    REQUIRE(spectrumService_spanName(spectrumService_spanIndex()) != nullptr);

    /* The wait in use is reported, and 0 means the gate: the host has no
       receiver to poll, so the gate is reported on with no polls */
    REQUIRE(st.settleUs == 0);
    REQUIRE(st.gateOn);
    REQUIRE(st.gatePolls == 0);

    /* A fixed wait is reported as such; the screen shows the sweep is not on
       the chip's indicator any more */
    spectrumService_setSettleIndex(4);
    spectrumService_status(&st);
    REQUIRE(st.settleUs == 400);

    /* A sweep driven the way the RTX thread drives it completes, and the host
       has no receiver clock to report: the cost per point stays zero, so the
       MS/PTO row shows "--" rather than a made-up number */
    for (int i = 0; i < 400; i++) {
        if (spectrumService_poll())
            break;
    }

    spectrumService_status(&st);
    REQUIRE(st.pointUs == 0);

    spectrumService_close();
    REQUIRE(!spectrumService_active());

    spectrumService_setSettleIndex(0);
}

TEST_CASE("the widest span is the one the cheaper sweep made possible",
          "[spectrum][settle]")
{
    /* 96 MHz is the engine's maximum, not a menu convenience: it is the span
       whose raster is only one pass of the RTX thread now that a point costs
       about 170 us instead of 694 (docs/SPECTRUM.md §3.3), and the span table's
       last entry is where the panel still makes sense (600 kHz per pixel on
       160 px). On the radio the band clamp then narrows it to what the receiver
       can tune in one go; the host reports a 136-174 MHz band, so here it ends
       up at 38 MHz with the step raised by the ladder. */
    REQUIRE(SPECTRUM_SPAN_COUNT == 9);

    const char *widest = spectrumService_spanName(SPECTRUM_SPAN_COUNT - 1);
    REQUIRE(widest != nullptr);
    REQUIRE(strcmp(widest, "96M") == 0);

    /* Every span has a name, and the widest is the last one offered */
    for (uint8_t i = 0; i < SPECTRUM_SPAN_COUNT; i++) {
        REQUIRE(spectrumService_spanName(i) != nullptr);
        REQUIRE(strlen(spectrumService_spanName(i)) > 0);
    }

    /* An index past the table is clamped, never read out of bounds */
    spectrumService_setSpanIndex(SPECTRUM_SPAN_COUNT);
    REQUIRE(spectrumService_spanIndex() == SPECTRUM_SPAN_COUNT - 1);

    spectrumService_close();
    spectrumService_setSpanIndex(SPECTRUM_SPAN_COUNT - 1);

    REQUIRE(spectrumService_open(100000000));
    REQUIRE(spectrumService_active());

    spectrumStatus_t st;
    spectrumService_status(&st);

    /* The widest span asks for a coarser step than the 25 kHz default: 160
       bins cannot cover tens of MHz at 25 kHz, and the ladder is what makes the
       raster fit without a second pass */
    REQUIRE(st.step > 25000);
    REQUIRE(spectrumService_effectiveStep() == st.step);

    REQUIRE(st.span > 1000000);
    REQUIRE(st.span <= 96000000);
    REQUIRE(st.bins <= SPECTRUM_MAX_BINS);

    /* The raster's own invariants: odd, so the centre is a bin, and it covers
       the span it reports */
    REQUIRE((st.bins & 1U) == 1U);
    REQUIRE((st.bins - 1) * st.step == st.span);

    spectrumService_close();
    spectrumService_setSpanIndex(2);
}
