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
#include "protocols/P25/P25Fec.h"
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
 * @param gain: amplitude of the outer level, as a fraction of OUTER.
 */
std::vector<int16_t> modulate(const uint8_t *bits, int nsym, double symPhase,
                              bool invert, int lead, double gain = 1.0)
{
    const std::vector<double> h = rrcTaps();
    const int n = lead + nsym * SPS + SPAN * SPS + 1;

    std::vector<double> base((size_t)n, 0.0);
    const double pol = invert ? -1.0 : 1.0;
    const double amp = OUTER * gain * pol;

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
        double v = base[(size_t)i] + DC * OUTER * gain * pol;
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
    bool truncated = false;
    bool lcValid = false;
    int talkgroup = -1;
    int source = -1;
    int lcRsErrors = -1;
    uint8_t lc[9] = {0};
    int voiceCount = 0;
    int voiceBad = 0;
    int imbeErrors = 0;
    uint8_t imbe[P25_IMBE_PER_LDU][P25_IMBE_BYTES] = {{0}};
    bool voiceValid[P25_IMBE_PER_LDU] = {false};
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
    r->truncated = f->truncated;
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
    memcpy(r->voiceValid, f->voiceValid, sizeof(r->voiceValid));
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

/*
 * ---------------------------------------------------------------------------
 * What the demodulator must refuse
 * ---------------------------------------------------------------------------
 *
 * Everything above is a frame that is on the air and has to come out right.
 * The cases below are the other half of the contract: a frame whose identity
 * cannot be trusted must not be published at all -- no NAC, no DUID, no
 * talkgroup, no voice -- and a frame that a DUID misdescribes must not be
 * read as the frame the DUID claims.  Each of these is one refusal, because
 * that is what the mode does with a wrong one: it puts a NAC or a talkgroup on
 * the screen that nobody transmitted, and the operator keys over a call that
 * is not there.
 */

namespace {

/**
 * Re-code the NID of a frame in place: the 64 bits at p25_imbe_nid_bits are
 * the BCH(63,16) codeword of (NAC, DUID) followed by the DUID parity bit
 * (TIA-102.BAAC).  The last argument flips that parity bit, which is the
 * cheapest way to make a NID that contradicts itself.
 */
void putNid(uint8_t *bits, uint16_t nac, uint8_t duid, bool flipParity)
{
    const uint64_t cw = p25Fec_bchEncode((uint16_t)((nac << 4) | duid));

    int parity;
    switch (duid) {
        case P25_DUID_HDU:
        case P25_DUID_TDU:
        case P25_DUID_TSBK:
        case P25_DUID_PDU:
        case P25_DUID_TDU15:
            parity = 0;
            break;
        default:
            parity = 1;   /* LDU1, LDU2 */
            break;
    }

    const uint64_t nid = (cw << 1) | (uint64_t)(parity ^ (flipParity ? 1 : 0));
    for (int i = 0; i < P25_NID_BITS; i++)
        bits[p25_imbe_nid_bits[i]] = (uint8_t)((nid >> (63 - i)) & 1u);
}

/** The off-air LDU1 of the fixture, as bits, ready to be patched. */
Bits ldu1Bits()
{
    return Bits();
}

/** Flip n bits of the NID field, spread so that no two coincide. */
void corruptNid(Bits &f, int n)
{
    for (int k = 0; k < n; k++)
        f.b[p25_imbe_nid_bits[(k * 5) % P25_NID_BITS]] ^= 1u;
}

/** Every frame a train produced, in order, with what it carried. */
struct Sequence {
    std::vector<Result> frames;
    unsigned gridMisses = 0;
};

void countSequence(const struct p25Frame *f, void *user)
{
    auto *s = static_cast<Sequence *>(user);
    s->frames.push_back(Result());
    count(f, &s->frames.back());
}

/** Modulate a list of frames back to back and demodulate the train. */
Sequence demodulateSequence(const std::vector<Bits> &frames, int nsym,
                            double phase)
{
    std::vector<uint8_t> stream;
    for (const Bits &f : frames)
        stream.insert(stream.end(), f.b, f.b + (size_t)nsym * 2u);

    std::vector<int16_t> pcm = modulate(stream.data(), nsym * (int)frames.size(),
                                        phase, false, 1000);

    Sequence s;
    p25Phy_init();

    const size_t block = 960;
    for (size_t off = 0; off < pcm.size(); off += block) {
        size_t n = pcm.size() - off;
        if (n > block)
            n = block;
        p25Phy_push(&pcm[off], n, countSequence, &s);
    }

    std::vector<int16_t> tail(2 * (size_t)P25_FRAME_SYMBOLS * SPS, 0);
    p25Phy_push(tail.data(), tail.size(), countSequence, &s);
    s.gridMisses = p25Phy_gridMisses();
    return s;
}

} // namespace

TEST_CASE("P25 accepts four NID errors and refuses the fifth", "[p25][phy]")
{
    /*
     * The NID is BCH(63,16): the code corrects far more than four bits, but
     * the demodulator's policy (decodeNid) accepts a NID only up to four
     * corrected bits.  Past that the NAC and the DUID are a guess, and the
     * frame must not be published at all -- the boundary is the contract, so
     * both sides of it are checked.
     */
    for (int errors = 1; errors <= 4; errors++) {
        Bits f = ldu1Bits();
        corruptNid(f, errors);
        std::vector<int16_t> pcm =
            modulate(f.b, P25_FRAME_SYMBOLS, 2.5, false, 1000);
        Result r = demodulate(pcm);
        INFO("errors " << errors);
        REQUIRE(r.frames == 1);
        requireFrame(r);          /* still the right NAC, DUID and voice */
    }

    for (int errors = 5; errors <= 12; errors++) {
        Bits f = ldu1Bits();
        corruptNid(f, errors);
        std::vector<int16_t> pcm =
            modulate(f.b, P25_FRAME_SYMBOLS, 2.5, false, 1000);
        Result r = demodulate(pcm);
        INFO("errors " << errors);
        REQUIRE(r.frames == 0);
    }

    /* And a NID that is noise: twenty bits wrong */
    Bits f = ldu1Bits();
    corruptNid(f, 20);
    std::vector<int16_t> pcm = modulate(f.b, P25_FRAME_SYMBOLS, 2.5, false, 1000);
    Result r = demodulate(pcm);
    REQUIRE(r.frames == 0);
}

TEST_CASE("P25 refuses a NID whose DUID parity bit is wrong", "[p25][phy]")
{
    /*
     * The last bit of the NID is the parity of the DUID: even for the frames
     * that carry a header or a data unit, odd for the voice frames.  A frame
     * whose parity contradicts its DUID was not received correctly, whatever
     * the BCH says, and must be dropped.
     */
    Bits f = ldu1Bits();
    putNid(f.b, 0x293, P25_DUID_LDU1, true);
    std::vector<int16_t> pcm = modulate(f.b, P25_FRAME_SYMBOLS, 2.5, false, 1000);
    Result r = demodulate(pcm);
    REQUIRE(r.frames == 0);

    /* The same NID with the parity it should have is accepted, so the refusal
     * above is the parity bit and not the re-coding. */
    Bits g = ldu1Bits();
    putNid(g.b, 0x293, P25_DUID_LDU1, false);
    pcm = modulate(g.b, P25_FRAME_SYMBOLS, 2.5, false, 1000);
    r = demodulate(pcm);
    requireFrame(r);
    requireReferenceBits(r);
}

TEST_CASE("P25 refuses a DUID it does not know", "[p25][phy]")
{
    /* 0x1, 0x2, 0x4, 0x6, 0x8, 0x9, 0xB, 0xD and 0xE are not P25 Phase 1 data
     * units.  A frame announcing one is not a frame this mode can read, so it
     * is not reported: the NAC would be the only usable thing on the screen
     * and it will come back with the next good frame. */
    for (uint8_t duid : {0x1, 0x2, 0x4, 0x6, 0x8, 0x9, 0xB, 0xD, 0xE}) {
        Bits f = ldu1Bits();
        putNid(f.b, 0x293, duid, false);
        std::vector<int16_t> pcm =
            modulate(f.b, P25_FRAME_SYMBOLS, 2.5, false, 1000);
        Result r = demodulate(pcm);
        INFO("duid 0x" << std::hex << (int)duid);
        REQUIRE(r.frames == 0);
    }
}

TEST_CASE("P25 does not read an LDU body for the DUID it announces",
          "[p25][phy]")
{
    /*
     * The DUID says how long the frame is and what is inside it.  Announce a
     * TSBK over the body of an LDU1 and the demodulator must cut 360 symbols,
     * find no block whose CRC checks out, and publish nothing but the NAC and
     * the DUID: no link control, no talkgroup, no voice.  Reading the body as
     * the frame the DUID claims is how a control-channel frame would put a
     * talkgroup on the screen that came out of a voice frame.
     */
    Bits f = ldu1Bits();
    putNid(f.b, 0x293, P25_DUID_TSBK, false);
    std::vector<int16_t> pcm = modulate(f.b, P25_FRAME_SYMBOLS, 2.5, false, 1000);
    Result r = demodulate(pcm);

    REQUIRE(r.frames == 1);
    REQUIRE(r.nac == 0x293);
    REQUIRE(r.duid == (int)P25_DUID_TSBK);
    REQUIRE(r.span == p25_frame_symbols(P25_DUID_TSBK));
    REQUIRE_FALSE(r.tsbkValid);
    REQUIRE_FALSE(r.lcValid);
    REQUIRE(r.talkgroup == 0);       /* untouched: nothing decoded it */
    REQUIRE(r.source == 0);
    REQUIRE(r.voiceCount == 0);
    REQUIRE(r.voiceBad == 0);
}

TEST_CASE("P25 publishes the NAC that was on the air", "[p25][phy]")
{
    /*
     * Three voice frames, three NACs, one stream: the NAC of each frame must be
     * the one it was sent with.  A demodulator that keeps the first one -- or
     * the conventional 0x293 -- passes every single-NAC test ever written and
     * still shows the wrong network on a busy channel.
     */
    std::vector<Bits> frames;
    const uint16_t nacs[3] = {0x001, 0x293, 0xFFF};
    for (uint16_t nac : nacs) {
        Bits f = ldu1Bits();
        putNid(f.b, nac, P25_DUID_LDU1, false);
        frames.push_back(f);
    }

    Sequence s = demodulateSequence(frames, P25_FRAME_SYMBOLS, 2.5);
    REQUIRE(s.frames.size() == 3);
    for (size_t i = 0; i < 3; i++) {
        INFO("frame " << i);
        REQUIRE(s.frames[i].nac == nacs[i]);
        REQUIRE(s.frames[i].duid == (int)P25_DUID_LDU1);
        REQUIRE(s.frames[i].lcValid);
        REQUIRE(s.frames[i].talkgroup == 1);
        REQUIRE(s.frames[i].source == 1);
    }
}

TEST_CASE("P25 refuses the link control of a frame that was cut short",
          "[p25][phy]")
{
    /*
     * The carrier drops after 400 of the 864 symbols: sync and NID arrived,
     * the low-speed data that carries the link control did not.  The NAC and
     * the DUID are real and may be shown, but no talkgroup and no source can
     * come out of a field that never arrived - the frame is truncated and
     * nothing is decoded from its payload - and the mode shows an empty call
     * rather than a talkgroup of zeros.
     *
     * The truncation flag is also what keeps the nine IMBE words of that missing
     * payload out of the vocoder: an all-zero word is a valid codeword of both
     * the Golay and the Hamming code used inside it, so the voice of this frame
     * used to decode "clean".  See the sweep below.
     */
    Bits f = ldu1Bits();
    std::vector<int16_t> pcm = modulate(f.b, 400, 2.5, false, 1000);
    Result r = demodulate(pcm);

    REQUIRE(r.frames == 1);
    REQUIRE(r.nac == 0x293);
    REQUIRE(r.duid == (int)P25_DUID_LDU1);
    REQUIRE(r.voiceCount == 0);  /* the payload of this frame never arrived */
    REQUIRE(r.truncated);
    REQUIRE_FALSE(r.lcValid);
    REQUIRE(r.lcRsErrors < 0);
    REQUIRE(r.talkgroup == 0);   /* never decoded, so never published */
    REQUIRE(r.source == 0);
}

TEST_CASE("P25 publishes no voice from a frame the carrier cut short",
          "[p25][phy]")
{
    /*
     * The carrier drops after the NID, at every point of the frame: 60 symbols
     * (three past the NID), 200, 400, 700 and 800 of the 864.  What arrives is
     * sync, NID and then the line the discriminator holds with no carrier
     * behind it.
     *
     * That line is why this had to be found by a test and not by listening: it
     * slices to one constant level, every IMBE header word comes out all-zero,
     * and all-zero is a valid codeword of the Golay(23,12) and the Hamming(15,
     * 11) inside it.  The demodulator therefore reported nine voice frames with
     * no corrected bit - 180 ms of "clean" speech that was never transmitted -
     * and OpMode_P25 pushes exactly that to the vocoder (voiceCount > 0).
     *
     * The contract: the NAC and the DUID were on the air and are published; the
     * voice, the link control and the TSBK were not and are not.
     */
    const Bits frame;
    for (int cut : {60, 200, 400, 700, 800})
    {
        std::vector<int16_t> pcm = modulate(frame.b, cut, 2.5, false, 1000);
        Result r = demodulate(pcm);
        INFO("carrier dropped after " << cut << " of " << P25_FRAME_SYMBOLS
                                      << " symbols");

        REQUIRE(r.frames == 1);          /* sync and NID were on the air */
        REQUIRE(r.nac == 0x293);
        REQUIRE(r.duid == (int)P25_DUID_LDU1);

        /* Before the flag: this is the defect, and it comes first in the
         * report so a reverted fix reads as what it was - nine words of voice
         * that passed the FEC. */
        REQUIRE(r.voiceCount == 0);
        REQUIRE(r.voiceBad == 0);        /* nothing arrived, nothing failed */
        REQUIRE(r.imbeErrors == 0);
        for (int n = 0; n < P25_IMBE_PER_LDU; n++)
            REQUIRE_FALSE(r.voiceValid[n]);

        REQUIRE(r.truncated);
        REQUIRE_FALSE(r.lcValid);
        REQUIRE(r.lcRsErrors < 0);
        REQUIRE(r.talkgroup == 0);
        REQUIRE(r.source == 0);
        REQUIRE_FALSE(r.tsbkValid);
    }
}

TEST_CASE("P25 keeps the voice of a frame that was on the air, however weak",
          "[p25][phy]")
{
    /*
     * The gate above measures the shape of the frame's last 10 ms against the
     * outer level the frame's own sync field measured, not the absolute size of
     * the samples: a signal at a fifth of the fixture's amplitude - a weak but
     * perfectly readable frame - still spans the four levels and still crosses
     * its mean, so its nine IMBE words are published.  A gate that thresholded
     * the sample level instead would mute a distant transmitter, which is the
     * failure on the other side of this one.
     *
     * The same frame at the same phase as the first test case, so the only
     * difference is the amplitude.
     */
    const Bits frame;
    std::vector<int16_t> pcm = modulate(frame.b, P25_FRAME_SYMBOLS, 2.5, false,
                                        1000, 0.2);
    Result r = demodulate(pcm);
    REQUIRE_FALSE(r.truncated);
    requireFrame(r);
    requireReferenceBits(r);
}

TEST_CASE("P25 recovers the frame grid after a refused frame", "[p25][phy]")
{
    /*
     * A frame the demodulator throws away must cost that frame and nothing
     * else: the next sync is still where the grid says it is.  A refusal that
     * also loses the symbol clock turns one bad frame on a channel into a
     * second of silence while the receiver walks the ring again -- on a control
     * channel, where a TSDU is 360 symbols, that is every frame.
     *
     * Eight frames with the fifth refused: seven published, and the grid is
     * out by exactly one prediction (the refused frame's own), which is the
     * same count a clean train of that length costs for the frame after the
     * last one.
     */
    std::vector<Bits> frames;
    for (int i = 0; i < 8; i++) {
        Bits f = ldu1Bits();
        if (i == 4)
            corruptNid(f, 8);    /* refused, not published */
        frames.push_back(f);
    }

    Sequence s = demodulateSequence(frames, P25_FRAME_SYMBOLS, 2.5);

    REQUIRE(s.frames.size() == 7);
    for (const Result &r : s.frames) {
        REQUIRE(r.nac == 0x293);
        REQUIRE(r.duid == (int)P25_DUID_LDU1);
        REQUIRE(r.lcValid);
        REQUIRE(r.voiceBad == 0);
        REQUIRE(r.imbeErrors == 0);
    }
    /* Measured: 2.  One for the refused frame, one for the frame after the
     * last of the train; the six frames after the refusal are on the grid. */
    INFO("grid misses " << s.gridMisses);
    REQUIRE(s.gridMisses <= 2);
}
