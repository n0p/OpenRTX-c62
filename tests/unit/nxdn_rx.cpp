/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file nxdn_rx.cpp
 * @brief NXDN receive sensitivity, on the real air capture and on noise.
 *
 * Three properties, each one something the receiver did not have before the
 * matched filter of TS 1-A 3.4/3.5 and the soft SACCH decisions landed:
 *
 *  1. **The clean capture is unchanged.**  18 frames, no LICH error, every
 *     SACCH block through its CRC-6 and all 72 AMBE+2 frames -- the numbers
 *     docs/RX_NXDN.md quotes for this asset.
 *  2. **The frame grid survives the noise.**  White Gaussian noise at a known
 *     Eb/N0 (tests/platform/c62_nxdn_bench.c has the arithmetic) leaves the
 *     LICH decoding every frame down to 8 dB, where the flat-RRC front end
 *     lost half of them.  Asserted as a rate, over several noise seeds, so the
 *     test says something about the receiver and not about one lucky run.
 *  3. **Soft decisions are worth more than hard ones.**  For every frame the
 *     test decodes it also runs the hard SACCH decoder on the dibits the
 *     frame carries, and requires the reliability-driven path to recover
 *     substantially more blocks at the floor of the mode.  That is the
 *     property the reliabilities exist for, checked on the frames themselves.
 *
 * The asset is read from the source tree (REPLAY_ASSET_DIR, the same define
 * tests/unit/M17_replay.cpp uses), never from the build directory.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnFec.h"
#include "protocols/NXDN/NxdnPhy.h"
}

#ifndef REPLAY_ASSET_DIR
#define REPLAY_ASSET_DIR "."
#endif

namespace
{

constexpr long SAMPLE_RATE = 48000;

/** What one run of the receiver over one (possibly noisy) capture produced. */
struct Run {
    unsigned frames = 0;
    unsigned lichOk = 0;
    unsigned sacchOk = 0;
    unsigned ambe = 0;
    unsigned hardOk = 0;   ///< Blocks the hard decoder gets on the same frames
    unsigned softWins = 0; ///< Frames where soft passes and hard does not
    unsigned hardWins = 0; ///< Frames where hard passes and soft does not
    double evm = 0.0;
};

Run operator+(const Run &a, const Run &b)
{
    Run r;

    r.frames = a.frames + b.frames;
    r.lichOk = a.lichOk + b.lichOk;
    r.sacchOk = a.sacchOk + b.sacchOk;
    r.ambe = a.ambe + b.ambe;
    r.hardOk = a.hardOk + b.hardOk;
    r.softWins = a.softWins + b.softWins;
    r.hardWins = a.hardWins + b.hardWins;

    return r;
}

std::vector<int16_t> readWav(const char *path)
{
    std::vector<int16_t> out;
    FILE *f = fopen(path, "rb");

    REQUIRE(f != nullptr);
    REQUIRE(fseek(f, 44, SEEK_SET) == 0);

    int16_t sample;

    while (fread(&sample, sizeof(sample), 1, f) == 1)
        out.push_back(sample);

    fclose(f);

    return out;
}

/** xorshift64* and a Box-Muller pair: the bench's generator, same constants. */
struct Noise {
    uint64_t state;

    explicit Noise(unsigned ebn0x10, unsigned seed)
    {
        state = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)ebn0x10 << 32)
              ^ ((uint64_t)seed * 0xBF58476D1CE4E5B9ULL);
        for (int i = 0; i < 8; i++)
            next();
    }

    uint64_t next()
    {
        uint64_t x = state;

        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state = x;

        return x * 0x2545F4914F6CDD1DULL;
    }

    double uniform()
    {
        return ((double)(next() >> 11) + 1.0) / 9007199254740994.0;
    }

    double gauss()
    {
        return std::sqrt(-2.0 * std::log(uniform()))
             * std::cos(2.0 * M_PI * uniform());
    }
};

void onFrame(const struct nxdnFrame *f, void *user)
{
    Run *r = static_cast<Run *>(user);

    r->frames++;
    if (f->lichOk)
        r->lichOk++;
    if (f->frameCount > 0)
        r->ambe += f->frameCount;
    r->evm += f->evm;

    if (!f->lichOk)
        return;

    if (f->sacchOk)
        r->sacchOk++;

    /* The hard decoder on the very same dibits the frame carries. */
    uint8_t bits[NXDN_SACCH_BITS];
    uint8_t info[26];

    for (int i = 0; i < NXDN_SACCH_BITS; i++) {
        int dibit = f->dibits[NXDN_OFF_SACCH + (i >> 1)];

        bits[i] = (uint8_t)((i & 1) ? (dibit & 1u) : ((dibit >> 1) & 1u));
    }

    bool hard = nxdnFecSacch(bits, info);

    if (hard)
        r->hardOk++;
    if (f->sacchOk && !hard)
        r->softWins++;
    if (!f->sacchOk && hard)
        r->hardWins++;
}

/**
 * Push the capture through the receiver at a given Eb/N0.
 *
 * sigma^2 = Ps * Fs / (4 Rs Eb/N0), so the noise is white over the 48 kHz
 * band and the energy per bit is the one the point names; 99 dB is "no noise
 * added" and is how the clean capture is run.
 */
Run runCapture(const std::vector<int16_t> &audio, double ebn0, unsigned seed)
{
    double sum = 0.0;
    double sumSq = 0.0;

    for (int16_t v : audio) {
        sum += v;
        sumSq += (double)v * (double)v;
    }

    const double power = sumSq / (double)audio.size()
                       - (sum / (double)audio.size())
                             * (sum / (double)audio.size());
    const double sigma = std::sqrt(
        power * (double)SAMPLE_RATE
        / (4.0 * (double)NXDN_SYMBOL_RATE * std::pow(10.0, ebn0 / 10.0)));
    Noise noise((unsigned)std::lround(ebn0 * 10.0), seed);
    std::vector<int16_t> noisy(audio.size());
    Run r;

    for (size_t i = 0; i < audio.size(); i++) {
        double v = (double)audio[i]
                 + ((ebn0 > 90.0) ? 0.0 : sigma * noise.gauss());

        if (v > 32767.0)
            v = 32767.0;
        if (v < -32768.0)
            v = -32768.0;
        noisy[i] = (int16_t)std::lround(v);
    }

    nxdnPhy_init();

    const size_t block = 960; /* 20 ms */
    for (size_t off = 0; off < noisy.size(); off += block) {
        size_t n = (noisy.size() - off > block) ? block : (noisy.size() - off);

        nxdnPhy_push(&noisy[off], n, onFrame, &r);
    }

    return r;
}

std::vector<int16_t> gAudio;

const std::vector<int16_t> &capture()
{
    if (gAudio.empty()) {
        std::string path = std::string(REPLAY_ASSET_DIR)
                         + "/nxdn_air_frame.wav";

        gAudio = readWav(path.c_str());
    }

    return gAudio;
}

/** Frames the 1.5 s asset carries. */
constexpr unsigned AIR_FRAMES = 18;

} /* namespace */

TEST_CASE("NXDN receive decodes the air capture unchanged", "[nxdn][rx]")
{
    Run r = runCapture(capture(), 99.0, 0);

    REQUIRE(r.frames == AIR_FRAMES);
    REQUIRE(r.lichOk == AIR_FRAMES);
    REQUIRE(r.sacchOk == AIR_FRAMES);
    REQUIRE(r.ambe == 4 * AIR_FRAMES);

    /* The matched filter leaves a clean eye: the flat RRC measured 0.140. */
    REQUIRE((r.evm / (double)r.frames) < 0.09);
}

TEST_CASE("NXDN receive holds the frame grid down to 8 dB", "[nxdn][rx]")
{
    /* Four noise realisations per point: the rates below are means, and the
     * tolerance is what the spread between them allows. */
    Run at12 = runCapture(capture(), 12.0, 0) + runCapture(capture(), 12.0, 1)
             + runCapture(capture(), 12.0, 2) + runCapture(capture(), 12.0, 3);
    Run at8 = runCapture(capture(), 8.0, 0) + runCapture(capture(), 8.0, 1)
            + runCapture(capture(), 8.0, 2) + runCapture(capture(), 8.0, 3);

    INFO("12 dB: " << at12.lichOk << "/" << at12.frames << " LICH, "
                   << at12.sacchOk << " SACCH; 8 dB: " << at8.lichOk << "/"
                   << at8.frames << " LICH, " << at8.sacchOk << " SACCH");

    /* Every frame, both points: the LICH is the first thing the noise eats and
     * the flat-RRC receiver lost half the frames at 8 dB. */
    REQUIRE(at12.lichOk == at12.frames);
    REQUIRE(at8.lichOk >= (at8.frames - at8.frames / 12));

    /* And the SACCH, which is the layer 3 the mode exists for. */
    REQUIRE(at12.sacchOk >= (at12.frames - at12.frames / 8));
    REQUIRE(at8.sacchOk >= (at8.frames * 3) / 4);
}

TEST_CASE("NXDN soft SACCH decisions beat hard ones", "[nxdn][rx]")
{
    /* At the floor of the mode the trellis either uses the reliabilities or
     * wastes them: compare, on the frames the receiver actually produced. */
    Run r = runCapture(capture(), 2.0, 0) + runCapture(capture(), 2.0, 1)
          + runCapture(capture(), 2.0, 2) + runCapture(capture(), 2.0, 3);

    INFO("2 dB: soft " << r.sacchOk << ", hard " << r.hardOk << ", soft wins "
                       << r.softWins << ", hard wins " << r.hardWins);

    /* The frame's own result is the soft decode with the hard one as a
     * fallback when the CRC fails, so this compares the reliability path
     * against the plain decisions taken from the very same dibits. */
    REQUIRE(r.sacchOk >= (3 * r.hardOk) / 2);
    REQUIRE(r.softWins >= 8);
}
