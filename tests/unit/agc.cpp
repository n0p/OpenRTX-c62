/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The airband AGC is a level control, so it is checked as one: with a signal
 * inside its window it must not move the audio, a step up must be brought back
 * in a few milliseconds, a step down must be let back slowly, and nothing it
 * produces may leave the sample range.
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

extern "C" {
#include "core/agc.h"
}

static constexpr int RATE = 48000;

/**
 * Peak of a block of samples.
 */
static int32_t peakOf(const std::vector<int16_t> &v)
{
    int32_t peak = 0;
    for (int16_t s : v)
        peak = (std::abs((int32_t)s) > peak) ? std::abs((int32_t)s) : peak;
    return peak;
}

/**
 * 1 kHz tone at the given fraction of full scale, `samples` long, starting at
 * the given phase.
 */
static std::vector<int16_t> tone(double level, size_t samples, size_t phase = 0)
{
    std::vector<int16_t> v(samples);
    for (size_t i = 0; i < samples; i++) {
        double t = (double)(phase + i) / RATE;
        double s = std::sin(2.0 * M_PI * 1000.0 * t);
        v[i] = (int16_t)std::lround(level * 32767.0 * s);
    }
    return v;
}

/**
 * Gain in dB that the instance currently applies.
 */
static double gainDb(const struct agc &a)
{
    return 20.0 * std::log10((double)agc_getGain(&a) / (double)AGC_UNITY);
}

TEST_CASE("AGC leaves a signal inside its window alone", "[agc][airband]")
{
    struct agc a;
    agc_init(&a, nullptr);

    /* 0.3 of full scale sits between closeLevel and openLevel */
    std::vector<int16_t> v = tone(0.3, RATE / 10);
    std::vector<int16_t> in = v;
    agc_process(&a, v.data(), v.size(), 1);

    /* The only movement is the start-up transient of the detector */
    REQUIRE(std::abs(gainDb(a)) < 0.05);
    for (size_t i = 0; i < v.size(); i++)
        REQUIRE(std::abs((int32_t)v[i] - (int32_t)in[i]) <= 2);
}

TEST_CASE("AGC attacks a step up in a few milliseconds", "[agc][airband]")
{
    struct agc a;
    agc_init(&a, nullptr);

    /* Settle on a weak signal: the AGC brings it up into its window */
    std::vector<int16_t> weak = tone(0.05, 3 * RATE / 2);
    agc_process(&a, weak.data(), weak.size(), 1);
    REQUIRE(peakOf(weak) > 6000);
    REQUIRE(peakOf(weak) < 16000);
    double levelled = gainDb(a);
    REQUIRE(levelled > 10.0);

    /* 12 ms: 10 dB of attack, which is what the defaults give */
    std::vector<int16_t> strong = tone(0.9, RATE * 12 / 1000);
    agc_process(&a, strong.data(), strong.size(), 1);
    REQUIRE(peakOf(strong) <= 32767); /* the limiter catches the step */
    REQUIRE(gainDb(a) < (levelled - 8.0));

    /* 200 ms: the level is back inside the window */
    std::vector<int16_t> more = tone(0.9, RATE / 5);
    agc_process(&a, more.data(), more.size(), 1);
    REQUIRE(peakOf(more) <= 32767); /* the step itself is caught */

    std::vector<int16_t> settled = tone(0.9, RATE / 20);
    agc_process(&a, settled.data(), settled.size(), 1);
    int32_t peak = peakOf(settled);
    REQUIRE(peak > 6000);   /* not crushed */
    REQUIRE(peak < 20000);  /* and not at full scale */
}

TEST_CASE("AGC releases slowly, so a fade does not pump the receiver",
          "[agc][airband]")
{
    struct agc a;
    agc_init(&a, nullptr);

    /* Land on a signal that sits in the window, and let it settle */
    std::vector<int16_t> strong = tone(0.3, RATE / 5);
    agc_process(&a, strong.data(), strong.size(), 1);
    double settled = gainDb(a);

    /* 50 ms of a signal 20 dB weaker: less than 2 dB of the gain comes back */
    std::vector<int16_t> weak = tone(0.03, RATE / 20);
    agc_process(&a, weak.data(), weak.size(), 1);
    REQUIRE((gainDb(a) - settled) < 2.0);

    /* Two seconds of it: the gain walks up to what the weak signal needs */
    std::vector<int16_t> longer = tone(0.03, 2 * RATE);
    agc_process(&a, longer.data(), longer.size(), 1);
    double levelled = gainDb(a);
    REQUIRE(levelled > 15.0);

    /* And there it stays: the window is where the loop parks the level */
    std::vector<int16_t> still = tone(0.03, RATE / 5);
    agc_process(&a, still.data(), still.size(), 1);
    REQUIRE(std::abs(gainDb(a) - levelled) < 0.1);
}


TEST_CASE("AGC and limiter never leave the sample range", "[agc][airband]")
{
    struct agc a;
    agc_init(&a, nullptr);

    /* A signal 40 dB below full scale: the AGC boosts it into the window */
    std::vector<int16_t> quiet = tone(0.01, 2 * RATE);
    agc_process(&a, quiet.data(), quiet.size(), 1);
    REQUIRE(peakOf(quiet) > 3000); /* +24 dB of gain is available */
    /* A signal at full scale with the gain already at its ceiling */
    std::vector<int16_t> loud(4800, 32767);
    agc_process(&a, loud.data(), loud.size(), 1);
    REQUIRE(peakOf(loud) <= 32767);

    /* -32768 is the one magnitude that does not fit an int16 */
    std::vector<int16_t> bottom(480, (int16_t)-32768);
    agc_process(&a, bottom.data(), bottom.size(), 1);
    for (size_t i = 0; i < bottom.size(); i++)
        REQUIRE(bottom[i] >= -32768);
}

TEST_CASE("AGC processes only the samples of its channel", "[agc][airband]")
{
    struct agc a;
    agc_init(&a, nullptr);

    /* Stereo, speaker on the left: the right channel must come out untouched */
    std::vector<int16_t> weak = tone(0.05, 2 * RATE);
    std::vector<int16_t> other(weak.size());
    for (size_t i = 0; i < other.size(); i++)
        other[i] = (int16_t)((i & 1) ? 12345 : -12345);

    for (size_t i = 0; i < other.size(); i += 2)
        weak[i + 1] = other[i + 1];

    agc_process(&a, weak.data(), weak.size() / 2, 2);

    for (size_t i = 1; i < weak.size(); i += 2)
        REQUIRE(weak[i] == other[i]);

    REQUIRE(gainDb(a) > 8.0); /* the left channel was a weak signal */
}

TEST_CASE("AGC limiter has a soft knee", "[agc][airband]")
{
    /* A frozen gain, so that only the limiter acts */
    struct agcConfig cfg = agc_defaultConfig();
    cfg.gainMin = AGC_UNITY;
    cfg.gainMax = AGC_UNITY;

    struct agc a;
    agc_init(&a, &cfg);

    /* Below the knee nothing is touched */
    std::vector<int16_t> below = tone(0.5, RATE / 100);
    std::vector<int16_t> in = below;
    agc_process(&a, below.data(), below.size(), 1);
    for (size_t i = 0; i < below.size(); i++)
        REQUIRE(std::abs((int32_t)below[i] - (int32_t)in[i]) <= 1);

    /* Above it the excess is compressed, not cut off: a 0.95 signal comes out
     * between the knee and full scale, and still a sine */
    std::vector<int16_t> above = tone(0.95, RATE / 100);
    agc_process(&a, above.data(), above.size(), 1);
    int32_t peak = peakOf(above);
    REQUIRE(peak > cfg.limitKnee);
    REQUIRE(peak < 32767);
}

TEST_CASE("AGC of silence stays silent", "[agc][airband]")
{
    struct agc a;
    agc_init(&a, nullptr);

    std::vector<int16_t> v(RATE / 10, 0);
    agc_process(&a, v.data(), v.size(), 1);

    for (int16_t s : v)
        REQUIRE(s == 0);
}
