/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file nxdn_tx.cpp
 * @brief NXDN transmit, checked against the receiver of this tree.
 *
 *  1. The coding halves: LICH parity, the SACCH block (CRC-6, punctured
 *     convolutional code, interleaver) and the VCH packing, each one closed
 *     through its own decoder in NxdnFec.c.
 *  2. The whole chain: frames built by NxdnTx.c, shaped by the 4FSK modulator
 *     and pushed into nxdnPhy_push(), must come back **bit for bit** -- the
 *     182 payload dibits, the four AMBE+2 frames and the layer 3 message of
 *     the SACCH -- over three call configurations and two transmit levels.
 *  3. The scrambler: with it off, the symbols on the air are the same payload
 *     without the mask and the receiver's LICH has to fail, which is what
 *     makes the mask visible in the test at all.
 *  4. The RAN: the filter the mode applies lives in NxdnDefs.c so that the
 *     whole policy -- before the first SACCH, on a match, on a mismatch, on a
 *     lost block and for the "any" setting -- is a table here, and the RAN the
 *     receiver reports from a synthetic stream is the one the transmitter put
 *     in the SACCH, which is what the filter reads.
 *  5. The state the mode publishes: a synthetic stream through the real
 *     demodulator and the real NxdnStatus module, then the struct the main
 *     screen reads -- sync, RAN, counters, "source>destination" and the
 *     message type -- for a call, a release and a disconnect, and what
 *     nxdnStatus_reset() and nxdnStatus_clear() leave behind.
 *  6. Symbol timing: the same call, built once as one continuous stream and
 *     fed through a resampler, has to decode bit for bit from an arbitrary
 *     start phase, while the two clocks drift by up to 500 ppm, and again
 *     after a lost block resets the demodulator mid-call.  The loopback tests
 *     start on a symbol boundary with the modulator's own clock, so none of
 *     them can see the fractional timing search or the tracker.
 *
 * The levels are the two the C62 can be set to: 3189 is the modulator gain
 * for +-1050 Hz of outer deviation (a 12.5 kHz channel, what the NXDN mode
 * uses) and 1063 the one for +-350 Hz (a 6.25 kHz channel).  They are the
 * same number the DMR chain uses for +-1944 Hz, scaled: see NXDN_TX_LEVEL in
 * OpMode_NXDN.cpp and docs/TX_NXDN.md.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "protocols/Fsk4Tx.h"
#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnFec.h"
#include "protocols/NXDN/NxdnPhy.h"
#include "protocols/NXDN/NxdnStatus.h"
#include "protocols/NXDN/NxdnTx.h"
}

namespace {

/** Outer deviation +-1050 Hz, the 12.5 kHz channel of the mode. */
constexpr float LEVEL_12K5 = 3189.0f;
/** Outer deviation +-350 Hz, the 6.25 kHz channel. */
constexpr float LEVEL_6K25 = 1063.0f;

struct Decoded {
    std::vector<nxdnFrame> frames;
};

void onFrame(const struct nxdnFrame *f, void *user)
{
    static_cast<Decoded *>(user)->frames.push_back(*f);
}

/** Deterministic, speech-like enough AMBE payload for frame n, VCH v. */
void payloadFrame(int n, int v, uint8_t out[NXDN_AMBE_BYTES])
{
    for (int b = 0; b < NXDN_AMBE_BYTES; b++)
        out[b] = (uint8_t)((n * 73 + v * 29 + b * 11 + 0x35) & 0xFF);
}

/**
 * Run a whole call through the modulator and the receiver.
 *
 * @param src, dst unit IDs.
 * @param ran RAN.
 * @param group group or individual call.
 * @param level modulator gain.
 * @param scrambler 4.6 data scrambler on or off.
 * @param nFrames voice frames to send.
 * @param air optional: the symbols that went on the air, per frame.
 * @return what the receiver decoded.
 */
Decoded loopback(uint16_t src, uint16_t dst, uint8_t ran, bool group,
                 float level, bool scrambler, int nFrames,
                 std::vector<std::vector<uint8_t>> *air)
{
    nxdn_tx_t tx;

    nxdnTx_setup(&tx, src, dst, ran, group);
    tx.scrambler = scrambler;

    fsk4_mod_t mod;
    fsk4Mod_init(&mod, NXDN_TX_SPS, level, false, true);

    nxdnPhy_init();

    Decoded out;
    std::vector<int16_t> audio(NXDN_TX_FRAME_SAMPLES);
    int8_t levels[NXDN_FRAME_SYMBOLS];

    /* The preamble the mode sends first, so the receiver's filter settles */
    nxdnTx_preamble(levels);
    fsk4Mod_levels(&mod, levels, NXDN_FRAME_SYMBOLS, audio.data());
    nxdnPhy_push(audio.data(), NXDN_TX_FRAME_SAMPLES, onFrame, &out);

    for (int n = 0; n < nFrames; n++) {
        uint8_t ambe[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES];

        for (int v = 0; v < NXDN_VCH_PER_FRAME; v++)
            payloadFrame(n, v, ambe[v]);

        nxdnTx_frame(&tx, ambe, levels);

        if (air != nullptr) {
            std::vector<uint8_t> d(NXDN_FRAME_SYMBOLS - NXDN_FSW_SYMBOLS);

            for (int i = 0; i < (int)d.size(); i++) {
                int8_t l = levels[NXDN_FSW_SYMBOLS + i];

                d[i] = (uint8_t)(((l > 0 ? 1 : 0) << 1)
                                 | (std::abs((int)l) == 3 ? 1 : 0));
            }
            air->push_back(d);
        }

        fsk4Mod_levels(&mod, levels, NXDN_FRAME_SYMBOLS, audio.data());
        nxdnPhy_push(audio.data(), NXDN_TX_FRAME_SAMPLES, onFrame, &out);
    }

    return out;
}

/** A frame's primary sync position mapped to the frame of the call it is. */
int frameIndex(const nxdnFrame &f)
{
    /* ring samples: 10 per symbol, 192 symbols per frame, preamble first */
    long inSample = 20L * f.symbol + 40L - NXDN_TX_FRAME_SAMPLES;
    int idx = (int)(inSample / NXDN_TX_FRAME_SAMPLES);

    return (idx < 0) ? -1 : idx;
}

/* --------------------------------------------------------- timing tested --- */

/**
 * A call built as one continuous baseband stream.
 *
 * loopback() feeds the receiver the samples the modulator wrote, frame after
 * frame, starting on a symbol boundary: the receiver's timing is right for
 * free.  This one keeps the stream so a test can resample it, which is what a
 * real capture is: the transmitter's clock is not the receiver's, the audio
 * path does not start on the first sample of a burst, and neither end of a
 * link is a whole symbol out of step.
 */
struct AirStream {
    std::vector<int16_t> samples; ///< 20 samples per symbol, preamble first
    int frames = 0;               ///< Voice frames it carries
    uint8_t ran = 63;             ///< RAN of the call
};

AirStream buildStream(uint16_t src, uint16_t dst, uint8_t ran, int frames)
{
    nxdn_tx_t tx;

    nxdnTx_setup(&tx, src, dst, ran, true);

    fsk4_mod_t mod;
    fsk4Mod_init(&mod, NXDN_TX_SPS, LEVEL_12K5, false, true);

    AirStream out;

    out.ran = ran;
    std::vector<int16_t> block(NXDN_TX_FRAME_SAMPLES);
    int8_t levels[NXDN_FRAME_SYMBOLS];

    nxdnTx_preamble(levels);
    fsk4Mod_levels(&mod, levels, NXDN_FRAME_SYMBOLS, block.data());
    out.samples.insert(out.samples.end(), block.begin(), block.end());

    for (int n = 0; n < frames; n++) {
        uint8_t ambe[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES];

        for (int v = 0; v < NXDN_VCH_PER_FRAME; v++)
            payloadFrame(n, v, ambe[v]);

        nxdnTx_frame(&tx, ambe, levels);
        fsk4Mod_levels(&mod, levels, NXDN_FRAME_SYMBOLS, block.data());
        out.samples.insert(out.samples.end(), block.begin(), block.end());
        out.frames++;
    }

    return out;
}

/** Linear interpolation of the stream at a fractional sample position. */
int16_t sampleAt(const std::vector<int16_t> &s, double pos)
{
    if (pos <= 0.0)
        return s.front();

    const size_t i = (size_t)pos;

    if (i + 1 >= s.size())
        return s.back();

    const double f = pos - (double)i;

    return (int16_t)std::lround((1.0 - f) * (double)s[i] + f * (double)s[i + 1]);
}

/**
 * Feed a stream to the receiver through a resampler.
 *
 * @param s    the samples the modulator wrote.
 * @param start position of the first sample read, in samples: a fractional
 *             value is a receiver that starts between two of them.
 * @param step  stream samples consumed per sample fed to the receiver: 1.0 is
 *             the transmitter's clock, 1.0002 is a receiver 200 ppm slow.
 * @param dropAt  when >= 0, @p dropCount samples are cut out of the stream at
 *             this position and the demodulator is reset there, which is what
 *             the mode does when the audio stream loses a block.
 * @param resetOnDrop reset the demodulator at the cut, as OpMode_NXDN does.
 * @return what the receiver decoded.
 */
Decoded runStream(const std::vector<int16_t> &s, double start, double step,
                  long dropAt, long dropCount, bool resetOnDrop,
                  double *evmOut)
{
    nxdnPhy_init();

    Decoded out;
    std::vector<int16_t> block(960);
    double pos = start;
    size_t cut = (dropAt >= 0) ? (size_t)dropAt : 0;
    bool dropped = (dropAt < 0);

    while (true) {
        size_t k = 0;

        for (; k < block.size(); k++) {
            size_t at = (size_t)pos;

            if (!dropped && (at >= cut)) {
                /* The stream jumps here: continue after the lost samples */
                dropped = true;
                pos += (double)dropCount;
                if (resetOnDrop)
                    nxdnPhy_reset();
                at = (size_t)pos;
            }
            if (at + 1 >= s.size())
                break;
            block[k] = sampleAt(s, pos);
            pos += step;
        }
        if (k == 0)
            break;

        nxdnPhy_push(block.data(), k, onFrame, &out);
    }

    if (evmOut != nullptr) {
        double sum = 0.0;

        for (const nxdnFrame &f : out.frames)
            sum += f.evm;
        *evmOut = out.frames.empty() ? 0.0 : sum / (double)out.frames.size();
    }

    return out;
}

/**
 * Check that the frames a run produced are the frames the stream carried, in
 * order, bit for bit, and that the SACCH of the superframes still spells the
 * call.
 */
void checkStream(const AirStream &stream, const Decoded &d, uint16_t src,
                 uint16_t dst, int minFrames)
{
    uint8_t l3[72] = { 0xFF };
    int idx = -1;
    int checked = 0;

    REQUIRE((int)d.frames.size() >= minFrames);

    for (const nxdnFrame &f : d.frames) {
        int n = -1;

        /* Which frame of the call is it? The AMBE payload says */
        for (int c = idx + 1; c < stream.frames; c++) {
            uint8_t want[NXDN_AMBE_BYTES];

            payloadFrame(c, 0, want);
            if (std::memcmp(f.frame[0], want, NXDN_AMBE_BYTES) == 0) {
                n = c;
                break;
            }
        }

        REQUIRE(f.lichOk);
        REQUIRE(f.sacchOk);
        REQUIRE(f.frameCount == NXDN_VCH_PER_FRAME);
        REQUIRE((f.sacchSr & 0x3Fu) == (uint8_t)(stream.ran & 0x3Fu));
        REQUIRE(n >= 0);
        idx = n;

        /* The AMBE frames of the ones the payload matched, bit for bit */
        for (int v = 0; v < NXDN_VCH_PER_FRAME; v++) {
            uint8_t want[NXDN_AMBE_BYTES];

            payloadFrame(n, v, want);
            REQUIRE(std::memcmp(f.frame[v], want, NXDN_AMBE_BYTES) == 0);
        }

        /* The SACCH segments reassemble the VCALL of the call */
        int part = 3 - ((f.sacchSr >> 6) & 3);

        std::memcpy(&l3[part * 18], f.sacchBits, 18);
        checked++;
    }

    REQUIRE(checked >= NXDN_SUPERFRAME);

    struct nxdnSacchMessage msg;

    nxdnFecSacchParse(l3, &msg);
    REQUIRE(msg.type == NXDN_MSG_VCALL);
    REQUIRE(msg.source == src);
    REQUIRE(msg.dest == dst);
}

/* --------------------------------------------------------- coding halves --- */

} /* namespace */

TEST_CASE("NXDN LICH encoding closes through its decoder", "[nxdn][tx]")
{
    for (uint8_t control = 0; control < 128; control++) {
        uint8_t dibits[NXDN_LICH_DIBITS];
        struct nxdnLich lich;

        nxdnFecLichEncode(control, dibits);
        REQUIRE(nxdnFecLich(dibits, &lich));
        REQUIRE(lich.raw == (control & 0x7F));
        REQUIRE(lich.rfType == ((control >> 5) & 3));
        REQUIRE(lich.usc == ((control >> 3) & 3));
        REQUIRE(lich.steal == ((control >> 1) & 3));
        REQUIRE(lich.outbound == ((control & 1) != 0));
    }
}

TEST_CASE("NXDN SACCH encoding closes through its decoder", "[nxdn][tx]")
{
    for (unsigned seed = 0; seed < 64; seed++) {
        uint8_t info[26];
        uint8_t bits[NXDN_SACCH_BITS];
        uint8_t back[26];

        for (int i = 0; i < 26; i++)
            info[i] = (uint8_t)(((seed * 2654435761u) >> (i & 15)) & 1u);

        nxdnFecSacchEncode(info, bits);
        REQUIRE(nxdnFecSacch(bits, back));
        REQUIRE(std::memcmp(info, back, 26) == 0);
    }
}

TEST_CASE("NXDN VCH packing closes through its unpacking", "[nxdn][tx]")
{
    for (unsigned seed = 0; seed < 64; seed++) {
        uint8_t frame[NXDN_AMBE_BYTES];
        uint8_t dibits[NXDN_VCH_DIBITS];
        uint8_t back[NXDN_AMBE_BYTES];

        for (int b = 0; b < NXDN_AMBE_BYTES; b++)
            frame[b] = (uint8_t)((seed * 37 + b * 53) & 0xFF);
        nxdnFecVchFromAmbe(frame, dibits);
        nxdnFecVchToAmbe(dibits, back);
        REQUIRE(std::memcmp(frame, back, NXDN_AMBE_BYTES) == 0);
    }
}

/* ------------------------------------------------------------- loopback --- */

namespace {

struct CallConfig {
    const char *name;
    uint16_t src;
    uint16_t dst;
    uint8_t ran;
    bool group;
};

const CallConfig calls[] = {
    { "group, RAN 17", 0x0123, 0x4567, 17, true },
    { "individual, RAN 0", 0xBEEF, 0x0001, 0, false },
    { "group, RAN 63", 0x0001, 0xFFF0, 63, true },
};

constexpr int FRAMES = 9;

} /* namespace */

TEST_CASE("NXDN TX loopback is bit exact, at both transmit levels",
          "[nxdn][tx]")
{
    for (const CallConfig &cfg : calls) {
        for (float level : { LEVEL_12K5, LEVEL_6K25 }) {
            INFO("call: " << cfg.name << ", level " << level);

            std::vector<std::vector<uint8_t>> air;
            Decoded d = loopback(cfg.src, cfg.dst, cfg.ran, cfg.group, level,
                                 true, FRAMES, &air);

            /* The preamble costs one frame of acquisition at most */
            REQUIRE(d.frames.size() >= (size_t)(FRAMES - 1));

            uint8_t l3[72] = { 0xFF };
            bool haveMsg = false;
            int checked = 0;

            for (const nxdnFrame &f : d.frames) {
                int idx = frameIndex(f);

                REQUIRE(f.lichOk);
                REQUIRE(f.lich.rfType == NXDN_RF_RDCH);
                REQUIRE(f.lich.steal == NXDN_STEAL_NONE);
                REQUIRE(f.voiceCount == NXDN_VCH_PER_FRAME);
                REQUIRE(f.frameCount == NXDN_VCH_PER_FRAME);
                REQUIRE(f.sacchOk);
                REQUIRE((f.sacchSr & 0x3Fu) == (uint8_t)(cfg.ran & 0x3F));

                if ((idx < 0) || (idx >= FRAMES))
                    continue;

                /* The 182 payload dibits, as the receiver sliced them */
                for (int i = 0; i < 182; i++) {
                    uint8_t expect = air[idx][i];
                    uint8_t mask = (uint8_t)((nxdnScramble[i >> 3]
                                              >> (i & 7)) & 1u);

                    if (mask)
                        expect ^= 0x02;
                    REQUIRE(f.dibits[i] == expect);
                }

                /* One AMBE+2 frame per VCH, in order */
                for (int v = 0; v < NXDN_VCH_PER_FRAME; v++) {
                    uint8_t want[NXDN_AMBE_BYTES];

                    payloadFrame(idx, v, want);
                    REQUIRE(std::memcmp(f.frame[v], want, NXDN_AMBE_BYTES)
                            == 0);
                }

                /* The SACCH segments of the superframe reassemble the VCALL */
                int part = 3 - ((f.sacchSr >> 6) & 3);

                std::memcpy(&l3[part * 18], f.sacchBits, 18);
                checked++;
                if (checked >= NXDN_SUPERFRAME)
                    haveMsg = true;
            }

            REQUIRE(haveMsg);

            struct nxdnSacchMessage msg;

            nxdnFecSacchParse(l3, &msg);
            REQUIRE(msg.type == NXDN_MSG_VCALL);
            REQUIRE(msg.source == cfg.src);
            REQUIRE(msg.dest == cfg.dst);
        }
    }
}

TEST_CASE("NXDN without the scrambler the air symbols are the raw payload",
          "[nxdn][tx]")
{
    std::vector<std::vector<uint8_t>> air;
    Decoded d = loopback(0x0ABC, 0x0123, 7, true, LEVEL_12K5, false, 5, &air);

    REQUIRE(!d.frames.empty());

    /*
     * The receiver always undoes the 4.6 mask, so with the transmitter not
     * applying it the LICH cannot pass and the payload it reports is the air
     * stream with the mask applied: comparing there proves the transmitter's
     * scrambler is exactly the mask the receiver removes.
     */
    int compared = 0;

    for (const nxdnFrame &f : d.frames) {
        int idx = frameIndex(f);

        /* A LICH can still close by chance (half of the mask lands on its
         * eight bits), a 60-bit SACCH block cannot */
        REQUIRE(!f.sacchOk);
        if ((idx < 0) || (idx >= (int)air.size()))
            continue;

        for (int i = 0; i < 182; i++) {
            uint8_t mask = (uint8_t)((nxdnScramble[i >> 3] >> (i & 7)) & 1u);

            REQUIRE((f.dibits[i] ^ (mask ? 0x02 : 0x00)) == air[idx][i]);
        }
        compared++;
    }

    REQUIRE(compared >= 3);
}

TEST_CASE("NXDN frame layout: FSW, LICH and the SACCH segment order",
          "[nxdn][tx]")
{
    nxdn_tx_t tx;
    int8_t levels[NXDN_FRAME_SYMBOLS];
    uint8_t zero[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES] = { { 0 } };

    nxdnTx_setup(&tx, 0x0100, 0x0200, 5, true);

    for (int f = 0; f < NXDN_SUPERFRAME; f++) {
        nxdnTx_frame(&tx, zero, levels);

        /* The frame sync is the table, symbol for symbol */
        REQUIRE(std::memcmp(levels, nxdnFsw, NXDN_FSW_SYMBOLS) == 0);

        /* LICH: RDCH, SACCH of a superframe (non-superframe at the edges),
         * no stolen VCHs, inbound */
        const uint8_t usc = (f == 0) ? NXDN_USC_SACCH_NS : NXDN_USC_SACCH_SF;
        const uint8_t control = (uint8_t)((NXDN_RF_RDCH << 5) | (usc << 3)
                                          | (NXDN_STEAL_NONE << 1));

        uint8_t dibits[NXDN_LICH_DIBITS];

        for (int i = 0; i < NXDN_LICH_DIBITS; i++) {
            int8_t l = levels[NXDN_FSW_SYMBOLS + i];

            dibits[i] = (uint8_t)(((l > 0 ? 1 : 0) << 1)
                                  | (std::abs((int)l) == 3 ? 1 : 0));
        }
        nxdnFecScramble(dibits, false);

        struct nxdnLich lich;

        REQUIRE(nxdnFecLich(dibits, &lich));
        REQUIRE(lich.raw == control);
    }
}

/* ------------------------------------------------------------------ timing --- */

TEST_CASE("NXDN symbol timing is recovered from an arbitrary phase",
          "[nxdn][rx][timing]")
{
    /*
     * The receiver never sees a burst start on one of its own samples: the
     * audio path hands it a stream that began somewhere inside a symbol.  The
     * fractional timing search of NxdnPhy.c is what turns that into frames, and
     * the loopback tests cannot see it because they start on a symbol
     * boundary with the modulator's own clock.
     *
     * These positions are fractions of a symbol either side of the boundary:
     * the first sample the receiver gets is not the first sample of the
     * preamble, and the frame grid still has to come out bit for bit.
     */
    constexpr int FRAMES = 10;
    AirStream stream = buildStream(0x0123, 0x4567, 21, FRAMES);

    const double starts[] = { 0.0, 2.7, 7.35, 13.9, 19.5 };

    for (double start : starts) {
        INFO("first sample at " << start << " of a symbol");
        Decoded d = runStream(stream.samples, start, 1.0, -1, 0, false, nullptr);

        checkStream(stream, d, 0x0123, 0x4567, FRAMES - 1);
    }
}

TEST_CASE("NXDN symbol timing follows a drifting clock", "[nxdn][rx][timing]")
{
    /*
     * The transmitter's 48 kHz and the receiver's are two crystals: a few tens
     * of ppm apart is normal, and the frame is 192 symbols long, so the
     * receiver has to slide its sampling instant as the drift accumulates or
     * the eye closes by the end of the call.  200 ppm over ten frames is two
     * thirds of a symbol by the last one.
     */
    constexpr int FRAMES = 10;
    AirStream stream = buildStream(0x0BEE, 0x0117, 5, FRAMES);

    struct Point { const char *name; double start; double step; };

    const Point points[] = {
        { "receiver 200 ppm fast", 3.1, 0.9998 },
        { "receiver 200 ppm slow", 11.6, 1.0002 },
        { "receiver 500 ppm slow", 0.0, 1.0005 },
    };

    for (const Point &p : points) {
        INFO(p.name);

        double evm = 0.0;
        Decoded d = runStream(stream.samples, p.start, p.step, -1, 0, false, &evm);

        checkStream(stream, d, 0x0BEE, 0x0117, FRAMES - 1);

        /* A tracker that is not following leaves the eye wide open: the mean
         * EVM of a stream read at the wrong rate is far above this. */
        REQUIRE(evm < 0.12);
    }
}

TEST_CASE("NXDN frame grid is re-acquired after a lost block",
          "[nxdn][rx][timing]")
{
    /*
     * The rtx thread resets the demodulator when the audio stream drops
     * samples -- the symbol clock jumped -- and the receiver then has to find
     * the frame grid again with no preamble in front of it: mid-call it only
     * has the frame sync words of the frames it is landing on.  What the
     * operator loses is the frames the jump cost, not the rest of the call.
     */
    constexpr int FRAMES = 14;
    AirStream stream = buildStream(0x0005, 0xFFF0, 63, FRAMES);

    /* 15 ms of stream lost inside the second frame of the call */
    const long dropAt = (long)((2 * NXDN_TX_FRAME_SAMPLES)
                               + NXDN_TX_FRAME_SAMPLES / 2);
    Decoded d = runStream(stream.samples, 0.0, 1.0, dropAt, 720, true,
                          nullptr);

    /*
     * The frames before the jump and, after one frame of re-acquisition, every
     * frame to the end: the call is followed again without the operator doing
     * anything.
     */
    checkStream(stream, d, 0x0005, 0xFFF0, FRAMES - 3);
}

/* --------------------------------------------------------------------- RAN --- */

namespace {

/** A frame with nothing in it but a SACCH result. */
nxdnFrame ranFrame(bool sacchOk, uint8_t sr)
{
    nxdnFrame f;

    std::memset(&f, 0, sizeof(f));
    f.lichOk = true;
    f.sacchOk = sacchOk;
    f.sacchSr = sr;

    return f;
}

} /* namespace */

TEST_CASE("NXDN RAN filter accepts its own and drops another", "[nxdn][ran]")
{
    /*
     * The RAN rides in the SR field of the SACCH, so the mode cannot filter on
     * it until a superframe has decoded.  The policy is one function so that it
     * can be proven here: what the receiver does before it knows the RAN, what
     * it does when it knows it, and what a lost SACCH block costs.
     */
    int lastRan = -1;
    nxdnFrame noSacch = ranFrame(false, 0);

    /* Before the first SACCH decodes, everything passes and nothing is known */
    REQUIRE(nxdnRanFrame(17, &lastRan, &noSacch));
    REQUIRE(lastRan == -1);

    /* The first block that closes sets the RAN of the call */
    nxdnFrame first = ranFrame(true, (uint8_t)((2 << 6) | 17));

    REQUIRE(nxdnRanFrame(17, &lastRan, &first));
    REQUIRE(lastRan == 17);

    /* Same RAN: kept.  Another one: dropped, and the call keeps its own RAN */
    nxdnFrame other = ranFrame(true, (uint8_t)((1 << 6) | 18));

    REQUIRE(!nxdnRanFrame(17, &lastRan, &other));
    REQUIRE(lastRan == 18);

    /* A lost SACCH block is judged by the last RAN that did decode */
    nxdnFrame lost = ranFrame(false, 0);

    REQUIRE(!nxdnRanFrame(17, &lastRan, &lost));
    REQUIRE(lastRan == 18);

    /* "Any" is every setting above 63, which is what the store holds */
    REQUIRE(nxdnRanFrame(NXDN_RAN_ANY, &lastRan, &other));
    REQUIRE(nxdnRanFrame(64, &lastRan, &other));
    REQUIRE(nxdnRanFrame(255, &lastRan, &other));

    /* The edges of the 6 bit field: 0 and 63 are RANs, not "any" */
    nxdnFrame zero = ranFrame(true, 0);
    nxdnFrame last = ranFrame(true, 63);
    int ran0 = -1;
    int ran63 = -1;

    REQUIRE(nxdnRanFrame(0, &ran0, &zero));
    REQUIRE(!nxdnRanFrame(1, &ran0, &zero));
    REQUIRE(nxdnRanFrame(63, &ran63, &last));
    REQUIRE(!nxdnRanFrame(3, &ran63, &last));

    /* And what a transmitter puts on the air for each setting */
    REQUIRE(nxdnRanTransmit(NXDN_RAN_ANY) == 63);
    REQUIRE(nxdnRanTransmit(63) == 63);
    REQUIRE(nxdnRanTransmit(17) == 17);
    REQUIRE(nxdnRanTransmit(0) == 0);
}

TEST_CASE("NXDN RAN on the air is the one the filter reads", "[nxdn][ran]")
{
    /*
     * The other half of the RAN story, and the half a unit test of the policy
     * cannot see: the RAN the transmitter puts in the SACCH SR field has to be
     * the one the receiver reports, superframe after superframe, on a stream
     * with no radio in it.  The filter is then run over the frames the
     * receiver actually produced: the call is followed with its own RAN and
     * dropped with another, which is exactly what the mode does.
     */
    constexpr uint8_t RAN = 21;
    AirStream stream = buildStream(0x0123, 0x4567, RAN, 8);
    Decoded d = runStream(stream.samples, 3.3, 1.0, -1, 0, false, nullptr);

    REQUIRE(d.frames.size() >= 7);

    int accepted = 0;
    int rejected = 0;

    for (const nxdnFrame &f : d.frames) {
        int ran = -1;

        REQUIRE(f.sacchOk);
        REQUIRE(nxdnRanFrame(RAN, &ran, &f));
        REQUIRE(ran == RAN);
        accepted++;
    }
    REQUIRE(accepted == (int)d.frames.size());

    for (const nxdnFrame &f : d.frames) {
        int ran = -1;

        if (!nxdnRanFrame((uint8_t)(RAN + 1), &ran, &f))
            rejected++;
    }
    REQUIRE(rejected == accepted);
}

/* ------------------------------------------------------------------ status --- */

namespace {

/** Handler that accounts each frame the way OpMode_NXDN does. */
void onFrameAccounted(const struct nxdnFrame *f, void *user)
{
    static_cast<Decoded *>(user)->frames.push_back(*f);
    nxdnStatus_frame(f);
}

/** Native stream, no resampler: the status chain is what is under test. */
void pushStream(const std::vector<int16_t> &s, Decoded *out)
{
    nxdnPhy_init();

    const size_t block = 960;

    for (size_t off = 0; off < s.size(); off += block) {
        size_t n = (s.size() - off > block) ? block : (s.size() - off);

        nxdnPhy_push(&s[off], n, onFrameAccounted, out);
    }
}

} /* namespace */

TEST_CASE("NXDN publishes the call it decoded, and keeps it until told",
          "[nxdn][status]")
{
    /*
     * This is the receiver double the test plan says no digital mode had: a
     * stream built by the transmitter of this tree goes through the real
     * demodulator and the real status module, and what comes out is what the
     * main screen draws -- sync, RAN, "source>destination" and the message
     * type.  The mode's onFrame() is now two lines over these (the RAN filter
     * and the vocoder), so this is the whole path the UI reads, minus the
     * radio.
     */
    constexpr uint16_t SRC = 0x0123;
    constexpr uint16_t DST = 0x4567;
    constexpr uint8_t RAN = 42;

    AirStream stream = buildStream(SRC, DST, RAN, 9);

    nxdnStatus_clear();

    struct nxdnStatus st;

    nxdn_getStatus(&st);
    REQUIRE(!st.sync);
    REQUIRE(st.ran == -1);
    REQUIRE(!st.message.valid);
    REQUIRE(st.frames == 0);

    Decoded d;

    pushStream(stream.samples, &d);
    nxdn_getStatus(&st);

    REQUIRE(d.frames.size() >= 7);

    /* Sync, the frame's channel type and the RAN of the call */
    REQUIRE(st.sync);
    REQUIRE(st.ran == RAN);
    REQUIRE(st.rfType == NXDN_RF_RDCH);
    REQUIRE(st.usc == NXDN_USC_SACCH_SF);
    REQUIRE(st.steal == NXDN_STEAL_NONE);
    REQUIRE(!st.inverted);
    REQUIRE(st.evm > 0);

    /* Counters: every frame, every SACCH block, every AMBE+2 frame */
    REQUIRE(st.frames == d.frames.size());
    REQUIRE(st.lichErrors == 0);
    REQUIRE(st.sacchChecks == st.frames);
    REQUIRE(st.sacchOk == st.frames);
    REQUIRE(st.voice);
    REQUIRE(st.voiceFrames == st.frames);
    REQUIRE(st.ambeFrames == 4 * st.voiceFrames);

    /* And the call itself: source and destination the way the UI writes them */
    REQUIRE(st.message.valid);
    REQUIRE(st.message.type == NXDN_MSG_VCALL);
    REQUIRE(st.message.source == SRC);
    REQUIRE(st.message.dest == DST);
    REQUIRE(st.messages >= 1);

    /* The UI's string, from the same numbers */
    REQUIRE(std::string(nxdnFecMsgName(st.message.type)) == "VCALL");

    /*
     * A new frequency: the sync, the RAN and the half-assembled superframe go,
     * the counters stay -- that is what nxdnStatus_reset() is for.
     */
    nxdnStatus_reset();
    nxdn_getStatus(&st);

    REQUIRE(!st.sync);
    REQUIRE(st.ran == -1);
    REQUIRE(!st.message.valid);
    REQUIRE(st.frames == d.frames.size());

    /* One frame more: sync and RAN are back, the call is not: one segment of
     * a superframe is not a message */
    pushStream(std::vector<int16_t>(stream.samples.begin(),
                                    stream.samples.begin() + 4
                                        * NXDN_TX_FRAME_SAMPLES), &d);
    nxdn_getStatus(&st);

    REQUIRE(st.sync);
    REQUIRE(st.ran == RAN);
    REQUIRE(!st.message.valid);

    /* Leaving the mode: nothing is left of it, counters included */
    nxdnStatus_clear();
    nxdn_getStatus(&st);

    REQUIRE(!st.sync);
    REQUIRE(st.ran == -1);
    REQUIRE(st.frames == 0);
    REQUIRE(st.ambeFrames == 0);
    REQUIRE(!st.message.valid);
}

TEST_CASE("NXDN publishes a release and a disconnect, not just a call",
          "[nxdn][status]")
{
    /*
     * The three message types the mode names -- VCALL, TX_REL and DISC -- come
     * out of the same SACCH assembly, and the UI keys its note row off them.
     * A message of any other type still reaches the status (its four blocks
     * passed their CRCs) but carries no call fields, which is what keeps the
     * screen from inventing a source and a destination.
     */
    struct msgType { uint8_t type; const char *name; };

    const msgType types[] = {
        { NXDN_MSG_VCALL, "VCALL" },
        { NXDN_MSG_TX_REL, "TX_REL" },
        { NXDN_MSG_DISC, "DISC" },
    };

    for (const msgType &t : types) {
        INFO(t.name);

        AirStream stream = buildStream(0x0BEE, 0x0117, 9, 9);
        Decoded d;

        nxdnStatus_clear();
        pushStream(stream.samples, &d);

        /* Replace the VCALL of the stream with this message and replay it */
        nxdn_tx_t tx;
        uint8_t octets[9];

        nxdnTx_setup(&tx, 0x0BEE, 0x0117, 9, true);
        std::memset(octets, 0, sizeof(octets));
        octets[0] = t.type;
        octets[2] = 0x00;
        octets[3] = 0x0B;           /* source 0x0BEE */
        octets[4] = 0xEE;
        octets[5] = 0x01;           /* destination 0x0117 */
        octets[6] = 0x17;
        nxdnTx_setMessage(&tx, octets);

        fsk4_mod_t mod;
        fsk4Mod_init(&mod, NXDN_TX_SPS, LEVEL_12K5, false, true);

        Decoded d2;
        std::vector<int16_t> audio(NXDN_TX_FRAME_SAMPLES);
        int8_t levels[NXDN_FRAME_SYMBOLS];

        nxdnStatus_clear();
        nxdnPhy_init();

        nxdnTx_preamble(levels);
        fsk4Mod_levels(&mod, levels, NXDN_FRAME_SYMBOLS, audio.data());
        nxdnPhy_push(audio.data(), NXDN_TX_FRAME_SAMPLES, onFrameAccounted,
                     &d2);

        for (int n = 0; n < 9; n++) {
            uint8_t ambe[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES];

            for (int v = 0; v < NXDN_VCH_PER_FRAME; v++)
                payloadFrame(n, v, ambe[v]);
            nxdnTx_frame(&tx, ambe, levels);
            fsk4Mod_levels(&mod, levels, NXDN_FRAME_SYMBOLS, audio.data());
            nxdnPhy_push(audio.data(), NXDN_TX_FRAME_SAMPLES,
                         onFrameAccounted, &d2);
        }

        struct nxdnStatus st;

        nxdn_getStatus(&st);
        REQUIRE(st.message.valid);
        REQUIRE(st.message.type == t.type);
        REQUIRE(st.message.source == 0x0BEE);
        REQUIRE(st.message.dest == 0x0117);
        REQUIRE(std::string(nxdnFecMsgName(st.message.type)) == t.name);
    }
}
