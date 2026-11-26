/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The TETRA demodulator must return, for any input.
 *
 * It did not: with digital silence at the start of a block the gain fit
 * divided by a zero deviation, the gain became infinite, and wrapf() spun in
 * `while (phi >= PI) phi -= 2*PI` for ever — the RTX thread inside
 * OpMode_TETRA::update(), with the radio deaf and the watchdog unable to
 * restart it. A muted codec, a DMA underrun or the 0.5 s the mode drops after
 * a retune all deliver exactly that block.
 *
 * These cases are the shapes that broke it (silence, DC, full-scale square,
 * alternating, a burst after silence) plus the invariants that make the
 * symbol loop bounded: the phase has to stay finite, the next symbol position
 * has to stay one window behind the samples written, and a block of n samples
 * can never produce more soft bits than the buffer contract of
 * TetraDemod.h says (3/4 of n, plus a margin).
 *
 * A hang here is a failing test: meson kills the binary on its timeout.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/TETRA/TetraDemod.h"
}

namespace
{

constexpr size_t BLOCK = 1440; /* 30 ms at 48 kHz, the mode's half buffer */

struct Run
{
    size_t soft = 0;      /* soft bits out */
    uint32_t softMax = 0; /* largest single-block output */
    bool finite = true;
};

Run feed(const std::vector<int16_t> &in)
{
    static struct tetraDemod dm;
    static int8_t soft[BLOCK * 3 / 4 + 8];
    Run r;

    tetraDemod_init(&dm);
    for (size_t off = 0; off + BLOCK <= in.size(); off += BLOCK) {
        size_t n = tetraDemod_process(&dm, in.data() + off, BLOCK, soft);

        if (n > sizeof(soft))
            FAIL("the demodulator wrote more soft bits than the buffer holds");
        if (n > r.softMax)
            r.softMax = (uint32_t)n;
        r.soft += n;

        /*
         * The invariants of the symbol loop: the phase state stays finite (an
         * infinite gain or offset is what used to hang wrapf) and the next
         * symbol position never falls more than one window behind, which is
         * what keeps the loop bounded.
         */
        if (!std::isfinite(dm.gain) || !std::isfinite(dm.offset)
            || !std::isfinite(dm.nextFrac))
            r.finite = false;
        int32_t behind = (int32_t)(dm.written - dm.nextInt);
        if (behind > (int32_t)(TETRA_DEMOD_SPS / 2 + 2) * 2)
            r.finite = false;
    }
    return r;
}

std::vector<int16_t> constant(size_t n, int16_t v)
{
    return std::vector<int16_t>(n, v);
}

} // namespace

TEST_CASE("Digital silence does not hang the demodulator", "[tetra][demod]")
{
    /* The case that hung: half a second of zeros, as a muted codec gives */
    std::vector<int16_t> in = constant(48000 / 2, 0);

    Run r = feed(in);
    REQUIRE(r.finite);
    /* Symbols keep coming (the receiver's bit count stays continuous), at
     * about 3/4 of a soft bit per input sample, and nothing overruns. */
    REQUIRE(r.soft > 0);
    REQUIRE(r.soft <= in.size() * 3 / 4);
}

TEST_CASE("Silence followed by a burst does not hang the demodulator",
          "[tetra][demod]")
{
    /* The real shape: a retune drops the first half second, then signal */
    std::vector<int16_t> in = constant(24000, 0);
    for (size_t i = 0; i < 24000; i++) {
        /* a loud pi/4-ish tone: ±6 kHz at 48 kHz, full of phase steps */
        double t = (double)i / 48000.0;
        in.push_back((int16_t)lrint(6000.0
                                    * sin(2 * M_PI * 2250.0 * t + 0.7)));
    }

    Run r = feed(in);
    REQUIRE(r.finite);
    REQUIRE(r.soft > 0); /* the tone is decoded as symbols, wrongly but safely */
}

TEST_CASE("Degenerate inputs are processed and bounded", "[tetra][demod]")
{
    SECTION("DC at full scale")
    {
        Run r = feed(constant(24000, 32767));
        REQUIRE(r.finite);
    }

    SECTION("Square wave at full scale (the worst phase steps)")
    {
        std::vector<int16_t> in;
        for (size_t i = 0; i < 24000; i++)
            in.push_back((i / 3) % 2 ? 32767 : -32768);
        Run r = feed(in);
        REQUIRE(r.finite);
    }

    SECTION("Alternating full scale, sample by sample")
    {
        std::vector<int16_t> in;
        for (size_t i = 0; i < 24000; i++)
            in.push_back((i & 1) ? 32767 : -32768);
        Run r = feed(in);
        REQUIRE(r.finite);
    }

    SECTION("A ramp that saturates")
    {
        std::vector<int16_t> in;
        for (size_t i = 0; i < 24000; i++)
            in.push_back((int16_t)((i * 37) & 0xffff));
        Run r = feed(in);
        REQUIRE(r.finite);
    }

    SECTION("Pseudorandom noise, the floor of an empty channel")
    {
        std::vector<int16_t> in;
        uint32_t s = 0x12345678u;
        for (size_t i = 0; i < 48000; i++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            in.push_back((int16_t)(s >> 12));
        }
        Run r = feed(in);
        REQUIRE(r.finite);
    }
}

TEST_CASE("The soft-bit buffer contract holds block by block", "[tetra][demod]")
{
    /*
     * TetraDemod.h: `soft` must hold at least 3/4 of the samples plus a small
     * margin. One symbol per interpolated sample and the window clamp are what
     * keep it true; the old unbounded catch-up wrote far past it (a silent
     * block after a broken update asked for 2^31 symbols).
     */
    std::vector<int16_t> in;
    uint32_t s = 0x9e3779b9u;

    for (size_t i = 0; i < 96000; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        in.push_back((int16_t)((s >> 11) & 0x1fff) - 4096);
    }

    Run r = feed(in);
    REQUIRE(r.finite);
    REQUIRE(r.softMax <= BLOCK * 3 / 4 + 8);
}
