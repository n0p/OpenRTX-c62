/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p25_phy.cpp
 * @brief Frame-cut path of the P25 Phase 1 demodulator, on a known frame.
 *
 * The bits of a real LDU1 (tests/unit/p25_frame_bits.h) are turned into a C4FM
 * baseband here: symbol levels from the dibits, root-raised-cosine alpha 0.2 at
 * ten samples per symbol, an arbitrary sub-sample phase, a residual carrier as
 * DC and the amplitude of a real discriminator.  The signal is then fed to
 * p25Phy_push() in 20 ms blocks, exactly as the radio feeds it.
 *
 * What it pins down: the demodulator has to find the frame sync whatever the
 * sampling phase is.  It used to search only the sample instants of its 24 kHz
 * ring, which left the eye anywhere inside a symbol; at 0.4-0.45 symbol off, the
 * matched filter attenuates the outer levels enough that the aperture gate threw
 * the true sync away and the frame was lost - four sampling phases in five.  The
 * sweep over one symbol period is that regression.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "p25_frame_bits.h"
#include "p25_tsdu_bits.h"
#include "protocols/P25/P25Defs.h"
#include "protocols/P25/P25Phy.h"

namespace {

/** The fixture: one bit per entry, MSB first out of the packed bytes. */
struct Bits {
    uint8_t b[P25_FRAME_BITS];
    Bits()
    {
        for (int i = 0; i < P25_FRAME_BITS; i++)
            b[i] = (p25_test_frame_bytes[i >> 3] >> (7 - (i & 7))) & 1;
    }
};

constexpr double FS = 48000.0;
constexpr int SPS = 10;               ///< 48 kHz / 4800 symbol/s
constexpr double ALPHA = 0.2;
constexpr int SPAN = 10;              ///< RRC length in symbols each side

/** Outer level of a real discriminator capture, in the ring's scale. */
constexpr double OUTER = 6000.0;
/** Residual carrier offset, as a fraction of the outer level. */
constexpr double DC = 0.15;

/** Root-raised-cosine impulse response, unit energy. */
std::vector<double> rrcTaps()
{
    std::vector<double> h;
    for (int n = -SPAN * SPS; n <= SPAN * SPS; n++)
    {
        double t = (double)n / SPS;
        double v;
        if (std::fabs(t) < 1e-9)
            v = 1.0 - ALPHA + 4.0 * ALPHA / M_PI;
        else
        {
            double a = M_PI * t * (1.0 - ALPHA);
            double b = 4.0 * ALPHA * t * std::cos(M_PI * t * (1.0 + ALPHA));
            double c = M_PI * t * (1.0 - (4.0 * ALPHA * t) * (4.0 * ALPHA * t));
            v = (std::sin(a) + b) / c;
        }
        h.push_back(v);
    }
    /* Unit energy, so the levels below survive the filter */
    double e = 0.0;
    for (double v : h)
        e += v * v;
    e = std::sqrt(e);
    for (double &v : h)
        v /= e;
    return h;
}

/** C4FM level of a dibit: 01 = +1800 Hz, 00 = +600, 10 = -600, 11 = -1800. */
double level(uint8_t d)
{
    switch (d & 3)
    {
        case 1: return 1.0;
        case 0: return 1.0 / 3.0;
        case 2: return -1.0 / 3.0;
        default: return -1.0;
    }
}

/**
 * C4FM baseband of a bit stream.
 *
 * @param bits: frame bits, two per symbol.
 * @param nsym: symbols in the frame (864 for an LDU, 360 for a TSDU).
 * @param symPhase: sub-sample offset of the first symbol, in samples at 48 kHz.
 * @param invert: true to flip the discriminator polarity.
 * @param lead: silence before the frame, in samples.
 */
std::vector<int16_t> modulate(const uint8_t *bits, int nsym, double symPhase,
                              bool invert, int lead)
{
    const std::vector<double> h = rrcTaps();
    const int n = lead + nsym * SPS + SPAN * SPS + 1;

    std::vector<double> base((size_t)n, 0.0);
    const double pol = invert ? -1.0 : 1.0;
    const double amp = OUTER * pol;

    for (int k = 0; k < nsym; k++)
    {
        uint8_t d = (uint8_t)((bits[2 * k] << 1) | bits[2 * k + 1]);
        double t0 = lead + symPhase + (double)k * SPS;
        for (int j = 0; j < (int)h.size(); j++)
        {
            double t = t0 + (double)(j - SPAN * SPS);
            long idx = lround(t);
            if (idx >= 0 && idx < (long)base.size())
                base[(size_t)idx] += amp * level(d) * h[(size_t)j];
        }
    }

    std::vector<int16_t> pcm((size_t)n);
    for (int i = 0; i < n; i++)
    {
        double v = base[(size_t)i] + DC * OUTER * pol;
        if (v > 32767.0)
            v = 32767.0;
        if (v < -32768.0)
            v = -32768.0;
        pcm[(size_t)i] = (int16_t)lround(v);
    }
    return pcm;
}

/** What the demodulator reported, so the assertions read like the radio. */
struct Result {
    int frames = 0;
    int nac = -1;
    int duid = -1;
    int span = 0;
    bool lcValid = false;
    int talkgroup = -1;
    int source = -1;
    int lcRsErrors = -1;
    uint8_t lc[9] = {0};
    int voiceCount = 0;
    int voiceBad = 0;
    int imbeErrors = 0;
    uint8_t imbe[P25_IMBE_PER_LDU][P25_IMBE_BYTES] = {{0}};
    int syncErrors = 0;
    bool inverted = false;
    bool tsbkValid = false;
    uint8_t tsbk[P25_TSBK_BYTES] = {0};
};

void count(const struct p25Frame *f, void *user)
{
    Result *r = static_cast<Result *>(user);
    r->frames++;
    r->nac = f->nac;
    r->duid = (int)f->duid;
    r->span = f->spanSymbols;
    r->lcValid = f->lcValid;
    r->talkgroup = f->talkgroup;
    r->source = (int)f->source;
    r->lcRsErrors = f->lcRsErrors;
    r->voiceCount = f->voiceCount;
    r->voiceBad = f->voiceBad;
    r->imbeErrors = f->imbeErrors;
    r->syncErrors = f->syncErrors;
    r->inverted = f->inverted;
    r->tsbkValid = f->tsbkValid;
    memcpy(r->lc, f->lc, sizeof(r->lc));
    memcpy(r->imbe, f->voice, sizeof(r->imbe));
    memcpy(r->tsbk, f->tsbk, sizeof(r->tsbk));
}

/** Push a whole buffer through the demodulator in 20 ms blocks. */
Result demodulate(const std::vector<int16_t> &pcm)
{
    Result r;
    p25Phy_init();

    const size_t block = 960;
    for (size_t off = 0; off < pcm.size(); off += block)
    {
        size_t n = pcm.size() - off;
        if (n > block)
            n = block;
        p25Phy_push(&pcm[off], n, count, &r);
    }

    /* The end of a buffer is not the end of a stream: give the last symbols
     * room to arrive, like the radio always does. */
    std::vector<int16_t> tail(2 * (size_t)P25_FRAME_SYMBOLS * SPS, 0);
    p25Phy_push(tail.data(), tail.size(), count, &r);
    return r;
}

/**
 * What a train of identical frames, back to back, produced.
 *
 * This is the steady state of a live channel: the frame grid has to hold on
 * its own, frame after frame, and the search has to stop looking.
 */
struct Train {
    int frames = 0;
    int tsbkBad = 0;        ///< TSBK frames whose CRC-16 did not check out
    unsigned tries = 0;     ///< Sync candidates the train itself cost
    unsigned tailTries = 0; ///< Candidates burnt scanning the silence after it
    unsigned gridMisses = 0;///< Times the frame-length prediction did not hold
    Result last;            ///< What the last frame carried
};

void countTrain(const struct p25Frame *f, void *user)
{
    Train *t = static_cast<Train *>(user);
    t->frames++;
    if (f->duid == P25_DUID_TSBK && !f->tsbkValid)
        t->tsbkBad++;
    t->last = Result();
    count(f, &t->last);
}

Train demodulateTrain(const uint8_t *bits, int nsym, int nframes, double phase)
{
    /* The frames repeated into one bit stream and modulated as a single
     * contiguous waveform: tiling a modulated frame would repeat its
     * root-raised-cosine tail as a gap between frames, which is not what a
     * transmitter sends and not what the demodulator has to hold a grid on. */
    std::vector<uint8_t> rep((size_t)nsym * 2u * (size_t)nframes);
    for (int f = 0; f < nframes; f++)
        memcpy(&rep[(size_t)f * (size_t)nsym * 2u], bits,
               (size_t)nsym * 2u);

    std::vector<int16_t> train = modulate(rep.data(), nsym * nframes, phase,
                                          false, 1000);

    Train t;
    p25Phy_init();

    const size_t block = 960;
    for (size_t off = 0; off < train.size(); off += block)
    {
        size_t n = train.size() - off;
        if (n > block)
            n = block;
        p25Phy_push(&train[off], n, countTrain, &t);
    }

    t.tries = p25Phy_syncTries();

    /* The end of a capture is not the end of a transmission: the demodulator
     * keeps hunting the silence after the last frame, and that hunt is not
     * what the frame timing costs. */
    std::vector<int16_t> tail(2 * (size_t)P25_FRAME_SYMBOLS * SPS, 0);
    p25Phy_push(tail.data(), tail.size(), countTrain, &t);

    t.tailTries = p25Phy_syncTries() - t.tries;
    t.gridMisses = p25Phy_gridMisses();
    return t;
}

/** Require the frame to be the one the bits carry, bit for bit. */
void requireFrame(const Result &r)
{
    REQUIRE(r.frames == 1);
    REQUIRE(r.nac == 0x293);
    REQUIRE(r.duid == (int)P25_DUID_LDU1);
    REQUIRE(r.span == P25_FRAME_SYMBOLS);
    REQUIRE(r.lcValid);
    REQUIRE(r.talkgroup == 1);
    REQUIRE(r.source == 1);
    REQUIRE(r.lcRsErrors == 0);      /* no symbol corrected in the LCW */
    REQUIRE(r.voiceCount == P25_IMBE_PER_LDU);
    REQUIRE(r.voiceBad == 0);
    REQUIRE(r.imbeErrors == 0);      /* no bit corrected in the nine codewords */
    REQUIRE(r.syncErrors == 0);
}

/**
 * The link control and the nine IMBE parameter sets of that frame, as the
 * reference receiver of the repo (work/p25-rx, validated against dsd-neo and
 * op25) decodes the same off-air LDU1 from P25-C4FM-VC_AF.wav.
 *
 * Zero tolerance: with no bit and no symbol corrected anywhere in the chain,
 * these 108 bytes are either the frame that went out or a different one, and
 * the cut of the frame has to be right to the symbol for them to match.
 */
const uint8_t referenceLc[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                                 0x00, 0x00, 0x01 };
const uint8_t referenceImbe[P25_IMBE_PER_LDU][P25_IMBE_BYTES] = {
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x44 },
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x45 },
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x44 },
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x45 },
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x44 },
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x45 },
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x44 },
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x45 },
    { 0x04, 0x0c, 0xfd, 0x7b, 0xfb, 0x7d, 0xf2, 0x7b, 0x3d, 0x9e, 0x44 },
};

void requireReferenceBits(const Result &r)
{
    CHECK(memcmp(r.lc, referenceLc, sizeof(referenceLc)) == 0);
    for (int n = 0; n < P25_IMBE_PER_LDU; n++)
    {
        INFO("IMBE frame " << n);
        CHECK(memcmp(r.imbe[n], referenceImbe[n], P25_IMBE_BYTES) == 0);
    }
}

} // namespace

TEST_CASE("P25 frame cut at every sampling phase", "[p25][phy]")
{
    /*
     * One symbol period of sub-sample phase, in 1/40 symbol steps.  The five
     * sampling instants of the 24 kHz ring are all covered, and the offsets in
     * between are the ones the demodulator used to lose.
     */
    const Bits frame;
    for (int i = 0; i < 40; i++)
    {
        double phase = (double)i * SPS / 40.0;
        std::vector<int16_t> pcm = modulate(frame.b, P25_FRAME_SYMBOLS, phase,
                                            false, 1000);
        Result r = demodulate(pcm);
        INFO("symbol phase " << phase << " samples");
        requireFrame(r);
        requireReferenceBits(r);
    }
}

TEST_CASE("P25 frame cut with an inverted discriminator", "[p25][phy]")
{
    const Bits frame;
    std::vector<int16_t> pcm = modulate(frame.b, P25_FRAME_SYMBOLS, 3.25, true,
                                        1000);
    Result r = demodulate(pcm);
    requireFrame(r);
    requireReferenceBits(r);
    REQUIRE(r.inverted);
}

TEST_CASE("P25 voice frames keep the frame grid", "[p25][phy]")
{
    /*
     * Eight LDUs back to back, an LDU being 864 symbols: after the first one
     * the search must predict every frame 864 symbols on instead of walking
     * the ring, and every frame must still come out with no corrected bit.
     */
    const Bits frame;
    const int n = 8;
    Train t = demodulateTrain(frame.b, P25_FRAME_SYMBOLS, n, 2.5);

    REQUIRE(t.frames == n);
    REQUIRE(t.last.nac == 0x293);
    REQUIRE(t.last.duid == (int)P25_DUID_LDU1);
    REQUIRE(t.last.span == P25_FRAME_SYMBOLS);
    REQUIRE(t.last.imbeErrors == 0);
    REQUIRE(t.last.voiceBad == 0);
    /* At most the frame after the last one is missing; every frame that is
     * there has to be found on the grid, not by walking the ring. */
    REQUIRE(t.gridMisses <= 1);
    /* One candidate per symbol walked up to the first sync, then one per
     * frame: 132 measured, against 5142 for a demodulator that re-acquires
     * every frame because it predicts the next sync 864 symbols on. */
    INFO("tries " << t.tries << " tail " << t.tailTries);
    REQUIRE(t.tries <= 200);
}

TEST_CASE("P25 control-channel frame is cut to its own length", "[p25][phy]")
{
    /*
     * A real TSDU: 720 bits, 360 symbols, not the 864 of a voice frame.  The
     * cut has to be the frame's own length - at every sampling phase - or the
     * levels are fitted over the frames that follow it and the next sync is
     * predicted 504 symbols too far.
     */
    static uint8_t bits[P25_TEST_TSDU_BITS];
    for (int i = 0; i < P25_TEST_TSDU_BITS; i++)
        bits[i] = (uint8_t)((p25_test_tsdu_bytes[i >> 3] >> (7 - (i & 7))) & 1);

    for (int i = 0; i < 40; i += 4)
    {
        double phase = (double)i * SPS / 40.0;
        std::vector<int16_t> pcm = modulate(bits, P25_TEST_TSDU_SYMBOLS, phase,
                                            false, 1000);
        Result r = demodulate(pcm);
        INFO("symbol phase " << phase << " samples");
        REQUIRE(r.frames == 1);
        REQUIRE(r.nac == P25_TEST_TSDU_NAC);
        REQUIRE(r.duid == (int)P25_DUID_TSBK);
        REQUIRE(r.span == P25_TEST_TSDU_SYMBOLS);
        REQUIRE(r.tsbkValid);
        REQUIRE(r.syncErrors == 0);
    }
}

TEST_CASE("P25 control-channel train keeps the grid", "[p25][phy]")
{
    /*
     * A control channel sends TSDUs every 360 symbols.  Cutting each one to
     * 864 symbols predicted the next sync 504 symbols late, so every single
     * frame used to be re-acquired by scanning the ring: this is the
     * regression for that.
     */
    static uint8_t bits[P25_TEST_TSDU_BITS];
    for (int i = 0; i < P25_TEST_TSDU_BITS; i++)
        bits[i] = (uint8_t)((p25_test_tsdu_bytes[i >> 3] >> (7 - (i & 7))) & 1);

    const int n = 12;
    Train t = demodulateTrain(bits, P25_TEST_TSDU_SYMBOLS, n, 2.5);

    REQUIRE(t.frames == n);
    REQUIRE(t.tsbkBad == 0);
    REQUIRE(t.last.nac == P25_TEST_TSDU_NAC);
    REQUIRE(t.last.span == P25_TEST_TSDU_SYMBOLS);
    /* Cutting 864 symbols gave one miss per frame; now only the frame after
     * the last one is missing. */
    REQUIRE(t.gridMisses <= 1);
    /* 126 measured; 4650 when the cut was the 864 symbols of a voice frame. */
    INFO("tries " << t.tries << " tail " << t.tailTries);
    REQUIRE(t.tries <= 200);
}

TEST_CASE("P25 demodulator does not invent frames", "[p25][phy]")
{
    /* Noise at the level of a quiet channel: no sync, so no frame. */
    std::vector<int16_t> noise(48000, 0);
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < noise.size(); i++)
    {
        state = state * 1664525u + 1013904223u;
        noise[i] = (int16_t)(((state >> 16) & 0x7FFF) - 0x4000);
    }

    Result r = demodulate(noise);
    REQUIRE(r.frames == 0);
}
