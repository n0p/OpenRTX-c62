/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The airband amplitude modulator is a transmitter's audio chain, so it is
 * checked on the four things that make a transmission legal and legible: the
 * band it passes, the depth it produces, the carrier it never lets fall to
 * zero, and the ramps that keep it from clicking. It also has one promise of
 * its own, that a depth the calibrated window cannot take is capped instead of
 * flat-topped.
 *
 * The envelope is what the PA radiates: duty = carrier + slope * m * x, so a
 * test that knows the carrier and the slope can read the modulation depth back
 * out of the duty values.
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

extern "C" {
#include "core/am_tx.h"
}

static constexpr uint16_t CARRIER = 330; /* 33 % of a 1000 count period */

/**
 * Calibration with a window wide enough for any depth: 10 % to 190 % of the
 * carrier, which is what 90 % modulation asks for.
 */
static amTxConfig wideWindow()
{
    amTxConfig cfg = am_tx_defaultConfig(CARRIER);
    cfg.dutyMin = CARRIER / 10;
    cfg.dutyMax = (uint16_t)((CARRIER * 19) / 10);
    cfg.slope = CARRIER;
    return cfg;
}

/**
 * 1 kHz tone at the given fraction of full scale.
 */
static std::vector<int16_t> tone(double level, size_t samples)
{
    std::vector<int16_t> v(samples);

    for (size_t i = 0; i < samples; i++)
        v[i] = (int16_t)std::lround(
            level * 32767.0
            * std::sin(2.0 * M_PI * 1000.0 * (double)i / 48000.0));
    return v;
}

/**
 * Run the modulator over a block of audio and collect the duty values.
 */
static std::vector<uint16_t> run(amTx &t, const std::vector<int16_t> &audio)
{
    std::vector<uint16_t> out(audio.size() / AMTX_DECIM + 1);
    size_t n = am_tx_process(&t, audio.data(), audio.size(), out.data());

    out.resize(n);
    return out;
}

/**
 * Peak-to-peak of the settled part of a run, in counts.
 */
static uint16_t swingOf(const std::vector<uint16_t> &duty, size_t skip)
{
    uint16_t lo = 0xFFFF, hi = 0;

    for (size_t i = skip; i < duty.size(); i++) {
        lo = (duty[i] < lo) ? duty[i] : lo;
        hi = (duty[i] > hi) ? duty[i] : hi;
    }
    return (uint16_t)(hi - lo);
}

TEST_CASE("The modulator passes the voice band and nothing else", "[am_tx]")
{
    /*
     * 300 to 2700 Hz is the transmit channel of the band. What matters for the
     * neighbours is the top: at 4 kHz, half a channel away, the envelope has
     * to be well down, or the 8.33 kHz channel next door hears the sideband.
     * The measurement is the swing of the envelope for a constant tone, which
     * is the gain of the chain at that frequency.
     */
    const double freqs[] = { 300.0, 1000.0, 2000.0, 2700.0, 4000.0 };

    double ref = 0.0;

    for (double f : freqs) {
        amTxConfig cfg = wideWindow();
        amTx t;

        am_tx_init(&t, &cfg);
        am_tx_setDepth(&t, 60);
        am_tx_keyDown(&t);

        std::vector<int16_t> audio(48000 / 2);
        for (size_t i = 0; i < audio.size(); i++)
            audio[i] = (int16_t)std::lround(
                8000.0 * std::sin(2.0 * M_PI * f * (double)i / 48000.0));

        std::vector<uint16_t> duty = run(t, audio);
        double swing = swingOf(duty, duty.size() - 800);

        if (f == 1000.0)
            ref = swing;

        if (f == 300.0)
            REQUIRE(20.0 * std::log10(swing / ref) > -6.0);
        if (f == 2700.0)
            REQUIRE(20.0 * std::log10(swing / ref) > -6.0);
        if (f == 4000.0)
            REQUIRE(20.0 * std::log10(swing / ref) < -8.0);
    }
}

TEST_CASE("The depth is the one the operator asked for", "[am_tx]")
{
    for (uint8_t depth : { 30, 60, 90 }) {
        amTxConfig cfg = wideWindow();
        amTx t;

        am_tx_init(&t, &cfg);
        am_tx_setDepth(&t, depth);
        REQUIRE(am_tx_getDepth(&t) == depth);
        am_tx_keyDown(&t);

        /* A tone at half scale, two seconds so the levelling settles */
        std::vector<uint16_t> duty = run(t, tone(0.5, 96000));
        double measured = 100.0 * (double)swingOf(duty, duty.size() - 800)
                        / (2.0 * (double)CARRIER);

        /*
         * The levelling stops at 0.95 of full scale and a 1 kHz tone is only
         * sampled at 8 kHz, so the peak the envelope sees is 0.88 to 0.95 of
         * full scale: the depth reached is 80 to 95 % of the one asked for,
         * and the slope of the calibration is what makes up the difference on
         * the air (docs/TX_AIRBAND.md, section 6).
         */
        REQUIRE(measured > 0.80 * depth);
        REQUIRE(measured < 1.02 * depth);
    }
}

TEST_CASE("The depth is capped by the PA window, never clipped", "[am_tx]")
{
    /*
     * A window from half to one and a half times the carrier can only take
     * 50 % of modulation. Asking for 90 % must give 50 % and a clean
     * envelope, not 90 % of which the positive peaks are flat.
     */
    amTxConfig cfg = am_tx_defaultConfig(CARRIER);
    cfg.dutyMin = CARRIER / 2;
    cfg.dutyMax = (uint16_t)((CARRIER * 3) / 2);
    cfg.slope = CARRIER;

    amTx t;
    am_tx_init(&t, &cfg);
    am_tx_setDepth(&t, 90);

    REQUIRE(am_tx_getDepth(&t) == 50);

    am_tx_keyDown(&t);
    std::vector<uint16_t> duty = run(t, tone(0.5, 96000));

    uint16_t lo = 0xFFFF, hi = 0;

    for (size_t i = duty.size() - 800; i < duty.size(); i++) {
        lo = (duty[i] < lo) ? duty[i] : lo;
        hi = (duty[i] > hi) ? duty[i] : hi;
    }

    REQUIRE(lo >= cfg.dutyMin);
    REQUIRE(hi <= cfg.dutyMax);
}

TEST_CASE("The carrier never falls to zero", "[am_tx]")
{
    /*
     * DSB with carrier: the envelope may fall to a tenth of the carrier at
     * 90 % depth and not below, or the receiver rectifies it and the
     * modulation is heard as distortion. A full scale tone is the worst case,
     * and the window clamps it there.
     */
    amTxConfig cfg = wideWindow();
    amTx t;

    am_tx_init(&t, &cfg);
    am_tx_setDepth(&t, 90);
    am_tx_keyDown(&t);

    std::vector<uint16_t> duty = run(t, tone(1.0, 96000));
    uint16_t lo = 0xFFFF;

    for (size_t i = duty.size() - 800; i < duty.size(); i++)
        lo = (duty[i] < lo) ? duty[i] : lo;

    REQUIRE(lo >= cfg.dutyMin);
    REQUIRE(lo > 0);
}

TEST_CASE("Keying ramps, and never jumps the envelope", "[am_tx]")
{
    amTxConfig cfg = wideWindow();
    amTx t;

    am_tx_init(&t, &cfg);
    am_tx_setDepth(&t, 60);

    /* Unkeyed: silence, no duty at all */
    std::vector<uint16_t> idle = run(t, tone(0.5, 4800));
    for (uint16_t d : idle)
        REQUIRE(d == 0);

    am_tx_keyDown(&t);
    REQUIRE(am_tx_busy(&t));

    std::vector<uint16_t> up = run(t, tone(0.5, 24000));

    /* The first value is a small fraction of the carrier, not the carrier */
    REQUIRE(up[0] < CARRIER / 4);

    /* It takes at least 10 ms to get there: a key click would be instant */
    size_t reached = up.size();
    for (size_t i = 0; i < up.size(); i++) {
        if (up[i] >= CARRIER) {
            reached = i;
            break;
        }
    }
    REQUIRE(reached != up.size());
    REQUIRE((double)reached / AMTX_DUTY_RATE > 0.010);

    /* And the depth needs its own ramp after the carrier is up */
    REQUIRE(up.size() > reached + 100);
}

TEST_CASE("A block of 48 samples gives 8 duty values", "[am_tx]")
{
    amTxConfig cfg = wideWindow();
    amTx t;

    am_tx_init(&t, &cfg);
    am_tx_keyDown(&t);

    std::vector<int16_t> audio(48, 0);
    std::vector<uint16_t> out(16);
    size_t n = am_tx_process(&t, audio.data(), audio.size(), out.data());

    REQUIRE(n == 48 / AMTX_DECIM);

    /* Silence is the carrier, once the ramp has finished */
    am_tx_reset(&t);
    am_tx_keyDown(&t);
    for (int i = 0; i < 40; i++)
        am_tx_process(&t, audio.data(), audio.size(), out.data());

    REQUIRE(out[0] == CARRIER);
}

/**
 * The limiter of core/am_tx.c, in floating point: linear to the knee, 4:1
 * above it, top at full scale.
 */
static double limitOf(double v)
{
    const double knee = 32112.0;
    const double span = 32767.0 - knee;
    double a = std::fabs(v);
    double over = a - knee;

    if (over <= 0)
        return v;

    over = (over * span) / (4.0 * span + over);
    a = knee + over;

    return (v < 0) ? -a : a;
}

TEST_CASE("The levelling gain cannot wrap a sample at the limiter", "[am_tx]")
{
    /*
     * The gain is Q16 and reaches +36 dB (64.0).  A sample at full scale
     * multiplied by it is 1.4e11, which does not fit an int32_t: the product
     * wrapped into an arbitrary sign and magnitude exactly when the limiter
     * was about to cap the sample, i.e. on every loud peak -- the one case the
     * limiter exists for.  On the air that is an AM transmission splattering
     * into the 8.33 kHz neighbours.
     *
     * The check is against the arithmetic: with the gain pinned for every
     * sample the envelope must be limit(gain * x).  The 0 dB run gives x
     * itself, because a tone at 12 % of full scale stays under the knee.
     */
    const double GAIN_MAX = 64.0 * 65536.0;

    std::vector<int16_t> audio = tone(0.12, 4800);

    amTxConfig cfg = wideWindow();
    amTx a, b;

    am_tx_init(&a, &cfg);
    am_tx_init(&b, &cfg);

    std::vector<int32_t> unity(audio.size());
    std::vector<int32_t> boosted(audio.size());

    for (size_t i = 0; i < audio.size(); i++) {
        /* Pin the gain: the detector moves it on every sample and this test is
           about the product, not about the levelling */
        a.gain = 65536;
        unity[i] = am_tx_envelope(&a, audio[i]);

        b.gain = (int32_t)GAIN_MAX;
        boosted[i] = am_tx_envelope(&b, audio[i]);
    }

    int saturated = 0;

    for (size_t i = 0; i < audio.size(); i++) {
        double expected = limitOf((double)unity[i] * 64.0);
        double got = (double)boosted[i];

        INFO("sample " << i << ": unity=" << unity[i] << " got=" << got
                       << " expected=" << expected);
        REQUIRE(std::fabs(got - expected) <= 2.0);
        REQUIRE(std::abs(boosted[i]) <= 32767);

        if (std::abs(boosted[i]) > 32700)
            saturated++;
    }

    /* The run must actually reach the limiter, or it proves nothing */
    REQUIRE(saturated > 100);
}
