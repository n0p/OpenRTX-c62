/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dpmr_tx.cpp
 * @brief dPMR transmit, checked against the receiver of this tree.
 *
 *  1. The coding halves -- CCH, HI, END and the TCH packing -- each closed
 *     through its own decoder in DpmrDefs.c, with the fields coming back.
 *  2. The whole chain: header frame, superframes and END frame built by
 *     DpmrTx.c, shaped by the 4FSK modulator and pushed into dpmrPhy_push(),
 *     must come back **bit for bit**: the header's addresses and call
 *     information, the CCH's frame number and address slice of every payload
 *     frame, the four AMBE+2 frames of its TCH, the channel code, and the END
 *     frame.
 *  3. Two call configurations and two transmit levels (12.5 kHz and 6.25 kHz
 *     deviation).
 *  4. Symbol timing: the same call as one continuous stream through a
 *     resampler -- an arbitrary start phase inside a symbol, +-100 ppm of
 *     clock drift bit for bit, and past that the framer still holding the CCH
 *     of every frame.  loopback() starts on a symbol boundary with the
 *     modulator's clock, so it cannot see the timing sweep of DpmrPhy.c.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/DPMR/DpmrDefs.h"
#include "protocols/DPMR/DpmrPhy.h"
#include "protocols/DPMR/DpmrTx.h"
#include "protocols/Fsk4Tx.h"
}

namespace {

/** Outer deviation +-1050 Hz, what dPMR asks for on a 12.5 kHz channel. */
constexpr float LEVEL_12K5 = 3189.0f;
/** Outer deviation +-350 Hz, a 6.25 kHz channel (h = 0.29). */
constexpr float LEVEL_6K25 = 1063.0f;

constexpr int FRAMES = 8;

struct Decoded {
    std::vector<dpmrFrame> frames;
};

void onFrame(const struct dpmrFrame *f, void *user)
{
    static_cast<Decoded *>(user)->frames.push_back(*f);
}

uint8_t payloadByte(int n, int v, int b)
{
    return (uint8_t)((n * 73 + v * 29 + b * 11 + 0x35) & 0xFF);
}

void payloadFrame(int n, int v, uint8_t out[DPMR_AMBE_BYTES])
{
    for (int b = 0; b < DPMR_AMBE_BYTES; b++)
        out[b] = payloadByte(n, v, b);
}

struct Call {
    const char *name;
    uint32_t own;
    uint32_t called;
    uint16_t channelCode;
    uint8_t mode;
    uint8_t ht;
    uint16_t ci;
};

Decoded loopback(const Call &call, float level, int preambleFrames,
                 std::vector<std::vector<uint8_t>> *air)
{
    dpmr_tx_t tx;

    dpmrTx_setup(&tx, call.own, call.called, call.channelCode);
    dpmrTx_setCallType(&tx, call.mode, 0, DPMR_FMT_PEER);

    fsk4_mod_t mod;
    fsk4Mod_init(&mod, DPMR_TX_SPS, level, false, false);

    dpmrPhy_init();

    Decoded out;
    std::vector<int16_t> audio(DPMR_TX_FRAME_SAMPLES);
    int8_t levels[DPMR_FRAME_SYMBOLS];

    for (int p = 0; p < preambleFrames; p++) {
        dpmrTx_preamble(levels);
        fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, audio.data());
        dpmrPhy_push(audio.data(), DPMR_TX_FRAME_SAMPLES, onFrame, &out);
    }

    dpmrTx_header(&tx, call.ht, call.ci, levels);
    fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, audio.data());
    dpmrPhy_push(audio.data(), DPMR_TX_FRAME_SAMPLES, onFrame, &out);

    for (int n = 0; n < FRAMES; n++) {
        uint8_t ambe[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES];

        for (int v = 0; v < DPMR_AMBE_FRAMES; v++)
            payloadFrame(n, v, ambe[v]);

        dpmrTx_frame(&tx, ambe, levels);

        if (air != nullptr) {
            std::vector<uint8_t> d(DPMR_FRAME_SYMBOLS);

            for (int i = 0; i < DPMR_FRAME_SYMBOLS; i++) {
                int8_t l = levels[i];

                d[i] = (uint8_t)(((l < 0) ? 2 : 0)
                                 | ((std::abs((int)l) == 3) ? 1 : 0));
            }
            air->push_back(d);
        }

        fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, audio.data());
        dpmrPhy_push(audio.data(), DPMR_TX_FRAME_SAMPLES, onFrame, &out);
    }

    dpmrTx_end(&tx, -1, levels);
    fsk4Mod_levels(&mod, levels, DPMR_TX_END_SYMBOLS, audio.data());
    dpmrPhy_push(audio.data(), DPMR_TX_END_SAMPLES, onFrame, &out);

    /*
     * The receiver decodes a frame only when a whole 192-symbol frame of
     * history is in its ring, and an END frame is 48 symbols long: the tail
     * carries the rest, as the PA ramp-down of a real transmission does.
     */
    std::vector<int16_t> tail(DPMR_TX_FRAME_SAMPLES, 0);

    fsk4Mod_levels(&mod, nullptr, DPMR_FRAME_SYMBOLS, tail.data());
    dpmrPhy_push(tail.data(), DPMR_TX_FRAME_SAMPLES, onFrame, &out);

    return out;
}

} /* namespace */

TEST_CASE("dPMR CCH encoding closes through its decoder", "[dpmr][tx]")
{
    for (uint8_t fn = 0; fn < 4; fn++) {
        for (unsigned seed = 0; seed < 16; seed++) {
            uint8_t dibits[DPMR_CCH_SYMBOLS];
            struct dpmrCch cch;
            const uint16_t id12 = (uint16_t)(0x100 * fn + 7 * seed);
            const uint8_t mode = (uint8_t)(seed % 6);
            const uint32_t sld = (0x12345u * seed) & 0x3FFFFu;

            dpmrFec_cchEncode(fn, id12, mode, 0, DPMR_FMT_PEER, sld, dibits);

            REQUIRE(dpmrFec_cchDecode(dibits, &cch));
            REQUIRE(cch.crcOk);
            REQUIRE(cch.fixes == 0);
            REQUIRE(cch.fn == fn);
            REQUIRE(cch.id12 == id12);
            REQUIRE(cch.mode == mode);
            REQUIRE(cch.version == 0);
            REQUIRE(cch.format == DPMR_FMT_PEER);
            REQUIRE(cch.sld == sld);
        }
    }
}

TEST_CASE("dPMR header information closes through its decoder", "[dpmr][tx]")
{
    for (unsigned seed = 0; seed < 16; seed++) {
        uint8_t dibits[DPMR_HI_SYMBOLS];
        struct dpmrHeader hdr;
        const uint32_t called = 0x010203u * (seed + 1) & 0xFFFFFFu;
        const uint32_t own = 0xFEDCBAu ^ (seed * 0x1111u);
        const uint16_t ci = (uint16_t)(0x7FFu & (seed * 337));

        dpmrFec_hiEncode(DPMR_HT_START, called, own, DPMR_MODE_VOICE, 0,
                         DPMR_FMT_PEER, ci, dibits);

        REQUIRE(dpmrFec_hiDecode(dibits, &hdr));
        REQUIRE(hdr.crcOk);
        REQUIRE(hdr.fixes == 0);
        REQUIRE(hdr.ht == DPMR_HT_START);
        REQUIRE(hdr.calledId == called);
        REQUIRE(hdr.ownId == own);
        REQUIRE(hdr.mode == DPMR_MODE_VOICE);
        REQUIRE(hdr.version == 0);
        REQUIRE(hdr.format == DPMR_FMT_PEER);
        REQUIRE(hdr.ci == ci);
    }
}

TEST_CASE("dPMR END encoding closes through its decoder", "[dpmr][tx]")
{
    for (unsigned seed = 0; seed < 8; seed++) {
        uint8_t dibits[DPMR_END_FIELD_SYMBOLS];
        struct dpmrEnd end;
        const uint8_t status = (uint8_t)(seed & 0x1F);

        dpmrFec_endEncode(1, 0, (uint8_t)seed, status, dibits);

        REQUIRE(dpmrFec_endDecode(dibits, &end));
        REQUIRE(end.crcOk[0]);
        REQUIRE(end.crcOk[1]);
        REQUIRE(end.same);
        REQUIRE(end.et == 1);
        REQUIRE(end.wait == seed);
        REQUIRE(end.status == status);
        REQUIRE(end.reserved == 0);
    }
}

TEST_CASE("dPMR TCH packing closes through its unpacking", "[dpmr][tx]")
{
    for (unsigned seed = 0; seed < 64; seed++) {
        uint8_t frame[DPMR_AMBE_BYTES];
        uint8_t dibits[36];
        uint8_t back[DPMR_AMBE_BYTES];

        for (int b = 0; b < DPMR_AMBE_BYTES; b++)
            frame[b] = (uint8_t)((seed * 37 + b * 53) & 0xFF);

        dpmrFec_voiceFrameEncode(frame, dibits);
        dpmrFec_voiceFrame(dibits, back);
        REQUIRE(std::memcmp(frame, back, DPMR_AMBE_BYTES) == 0);
    }
}

TEST_CASE("dPMR TX loopback is bit exact, at both transmit levels",
          "[dpmr][tx]")
{
    static const Call calls[] = {
        { "voice, 12.5 kHz level", 0x000135, 0x00393D, 0x0DD, DPMR_MODE_VOICE,
          DPMR_HT_START, 0x123 },
        { "voice, other ids", 0xABCDEF, 0x123456, 0x145, DPMR_MODE_VOICE,
          DPMR_HT_START, 0x001 },
    };

    for (const Call &call : calls) {
        for (float level : { LEVEL_12K5, LEVEL_6K25 }) {
            INFO("call: " << call.name << ", level " << level);

            std::vector<std::vector<uint8_t>> air;
            Decoded d = loopback(call, level, 1, &air);

            bool headerOk = false;
            int voiceFrames = 0;

            for (const dpmrFrame &f : d.frames) {
                if (f.headerOk) {
                    REQUIRE(f.header.ht == call.ht);
                    REQUIRE(f.header.calledId == call.called);
                    REQUIRE(f.header.ownId == call.own);
                    REQUIRE(f.header.mode == call.mode);
                    REQUIRE(f.header.ci == call.ci);
                    headerOk = true;
                    continue;
                }
                if (!f.cchOk || !f.voice)
                    continue;

                REQUIRE(f.cch.crcOk);
                REQUIRE(f.cch.mode == call.mode);
                REQUIRE(f.cch.format == DPMR_FMT_PEER);

                /* Which transmitted frame is it? The AMBE payload says */
                int idx = -1;
                int bestScore = 0;

                for (int n = 0; n < FRAMES; n++) {
                    int score = 0;

                    for (int v = 0; v < DPMR_AMBE_FRAMES; v++)
                        for (int b = 0; b < DPMR_AMBE_BYTES; b++)
                            if (f.ambe[v][b]
                                == payloadByte(n, v, b))
                                score++;
                    if (score > bestScore) {
                        bestScore = score;
                        idx = n;
                    }
                }
                REQUIRE(idx >= 0);
                REQUIRE(bestScore == 4 * DPMR_AMBE_BYTES);

                /* Four AMBE+2 frames, bit for bit */
                for (int v = 0; v < DPMR_AMBE_FRAMES; v++)
                    for (int b = 0; b < DPMR_AMBE_BYTES; b++)
                        REQUIRE(f.ambe[v][b] == payloadByte(idx, v, b));

                /* The frame number and its address slice of the CCH */
                REQUIRE(f.cch.fn == (idx & 3));
                switch (idx & 3) {
                case 0: REQUIRE(f.cch.id12 == ((call.called >> 12) & 0xFFF));
                    break;
                case 1: REQUIRE(f.cch.id12 == (call.called & 0xFFF)); break;
                case 2: REQUIRE(f.cch.id12 == ((call.own >> 12) & 0xFFF)); break;
                default: REQUIRE(f.cch.id12 == (call.own & 0xFFF)); break;
                }

                /* Frames 2 and 4 open with the channel code */
                if ((idx & 1) != 0)
                    REQUIRE(f.channelCode == call.channelCode);

                voiceFrames++;
            }

            REQUIRE(headerOk);
            REQUIRE(voiceFrames >= FRAMES - 1);

            /* The END frame closes the transmission */
            bool endOk = false;

            for (const dpmrFrame &f : d.frames)
                if (f.endOk)
                    endOk = true;
            REQUIRE(endOk);
        }
    }
}

/* ------------------------------------------------------------------ timing --- */

namespace {

/**
 * A call built as one continuous baseband stream.
 *
 * loopback() pushes the modulator's own samples, frame after frame, starting
 * on a symbol boundary: the demodulator's timing search is right for free.
 * This keeps the stream so a test can resample it, which is what an over the
 * air signal is: the transmitter's clock is not the receiver's and the audio
 * path does not start on the first sample of a burst.
 */
struct AirStream {
    std::vector<int16_t> samples; ///< 20 samples per symbol
    int frames = 0;               ///< Voice frames it carries
};

AirStream buildStream(const Call &call, int frames)
{
    dpmr_tx_t tx;

    dpmrTx_setup(&tx, call.own, call.called, call.channelCode);
    dpmrTx_setCallType(&tx, call.mode, 0, DPMR_FMT_PEER);

    fsk4_mod_t mod;
    fsk4Mod_init(&mod, DPMR_TX_SPS, LEVEL_12K5, false, false);

    AirStream out;
    std::vector<int16_t> block(DPMR_TX_FRAME_SAMPLES);
    int8_t levels[DPMR_FRAME_SYMBOLS];

    /* Two preamble frames, as the mode sends them (DPMR_TX_PREAMBLE_FRAMES) */
    for (int p = 0; p < 2; p++) {
        dpmrTx_preamble(levels);
        fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, block.data());
        out.samples.insert(out.samples.end(), block.begin(), block.end());
    }

    dpmrTx_header(&tx, call.ht, call.ci, levels);
    fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, block.data());
    out.samples.insert(out.samples.end(), block.begin(), block.end());

    for (int n = 0; n < frames; n++) {
        uint8_t ambe[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES];

        for (int v = 0; v < DPMR_AMBE_FRAMES; v++)
            payloadFrame(n, v, ambe[v]);

        dpmrTx_frame(&tx, ambe, levels);
        fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, block.data());
        out.samples.insert(out.samples.end(), block.begin(), block.end());
        out.frames++;
    }

    dpmrTx_end(&tx, -1, levels);
    fsk4Mod_levels(&mod, levels, DPMR_TX_END_SYMBOLS, block.data());
    out.samples.insert(out.samples.end(), block.begin(),
                       block.begin() + DPMR_TX_END_SAMPLES);

    /* The tail the ring needs to close the last frame, as the PA ramp is */
    std::vector<int16_t> tail(DPMR_TX_FRAME_SAMPLES, 0);

    fsk4Mod_levels(&mod, nullptr, DPMR_FRAME_SYMBOLS, tail.data());
    out.samples.insert(out.samples.end(), tail.begin(), tail.end());

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
 * Feed a stream to the demodulator through a resampler.
 *
 * @param s     the samples the modulator wrote, 20 per symbol.
 * @param start position of the first sample read: fractional is a receiver
 *              that starts between two samples.
 * @param step  stream samples consumed per sample fed to the receiver: 1.0 is
 *              the transmitter's clock, 1.0002 a receiver 200 ppm slow.
 */
Decoded runStream(const std::vector<int16_t> &s, double start, double step,
                  double *evmOut)
{
    dpmrPhy_init();

    Decoded out;
    std::vector<int16_t> block(960);
    double pos = start;

    while (true) {
        size_t k = 0;

        for (; k < block.size(); k++) {
            if (pos >= (double)(s.size() - 1))
                break;
            block[k] = sampleAt(s, pos);
            pos += step;
        }
        if (k == 0)
            break;

        dpmrPhy_push(block.data(), k, onFrame, &out);
    }

    if (evmOut != nullptr) {
        double sum = 0.0;

        for (const dpmrFrame &f : out.frames)
            sum += f.evm;
        *evmOut = out.frames.empty() ? 0.0 : sum / (double)out.frames.size();
    }

    return out;
}

/** Which voice frame of the call a decoded frame carries, -1 if none. */
int voiceIndexOf(const dpmrFrame &f, int frames)
{
    for (int n = 0; n < frames; n++) {
        bool match = true;

        for (int v = 0; v < DPMR_AMBE_FRAMES && match; v++)
            for (int b = 0; b < DPMR_AMBE_BYTES; b++)
                if (f.ambe[v][b] != payloadByte(n, v, b))
                    match = false;
        if (match)
            return n;
    }

    return -1;
}

/**
 * Check what a resampled run decoded: the header and the call it names, the
 * CCH and the address slice of every payload frame, its four AMBE+2 frames bit
 * for bit, the channel code of frames 2 and 4, and the END frame.
 */
void checkStream(const AirStream &stream, const Call &call, const Decoded &d)
{
    int header = 0;
    int voice = 0;
    int end = 0;
    int last = -1;

    for (const dpmrFrame &f : d.frames) {
        if (f.headerOk) {
            REQUIRE(f.header.ht == call.ht);
            REQUIRE(f.header.calledId == call.called);
            REQUIRE(f.header.ownId == call.own);
            REQUIRE(f.header.ci == call.ci);
            header++;
            continue;
        }

        if (f.endOk) {
            end++;
            continue;
        }

        if (!f.cchOk || !f.voice)
            continue;

        int n = voiceIndexOf(f, stream.frames);

        REQUIRE(n > last);      /* in order, none repeated */
        last = n;

        for (int v = 0; v < DPMR_AMBE_FRAMES; v++)
            for (int b = 0; b < DPMR_AMBE_BYTES; b++)
                REQUIRE(f.ambe[v][b] == payloadByte(n, v, b));

        REQUIRE(f.cch.fn == (n & 3));
        switch (n & 3) {
        case 0: REQUIRE(f.cch.id12 == ((call.called >> 12) & 0xFFF)); break;
        case 1: REQUIRE(f.cch.id12 == (call.called & 0xFFF)); break;
        case 2: REQUIRE(f.cch.id12 == ((call.own >> 12) & 0xFFF)); break;
        default: REQUIRE(f.cch.id12 == (call.own & 0xFFF)); break;
        }

        if ((n & 1) != 0)
            REQUIRE(f.channelCode == call.channelCode);

        voice++;
    }

    REQUIRE(header == 1);
    REQUIRE(end >= 1);
    REQUIRE(voice >= stream.frames - 1);
}

const Call timingCall = { "timed call", 0x000135, 0x00393D, 0x0DD,
                          DPMR_MODE_VOICE, DPMR_HT_START, 0x123 };

} /* namespace */

TEST_CASE("dPMR symbol timing is recovered from an arbitrary phase",
          "[dpmr][rx][timing]")
{
    constexpr int FRAMES = 8;
    AirStream stream = buildStream(timingCall, FRAMES);

    const double starts[] = { 0.0, 3.4, 8.75, 14.1, 19.6 };

    for (double start : starts) {
        INFO("first sample at " << start << " of a symbol");
        Decoded d = runStream(stream.samples, start, 1.0, nullptr);

        checkStream(stream, timingCall, d);
    }
}

TEST_CASE("dPMR symbol timing follows a clock within 100 ppm",
          "[dpmr][rx][timing]")
{
    /*
     * The receiver's 48 kHz and the transmitter's are two crystals.  Inside
     * 100 ppm -- two orders of magnitude worse than the TCXO of either end --
     * the timing sweep of DpmrPhy.c has to hold the frame bit for bit to the
     * end of the call; the drift by the last frame is a few samples.
     */
    constexpr int FRAMES = 8;
    AirStream stream = buildStream(timingCall, FRAMES);

    struct Point { const char *name; double start; double step; };

    const Point points[] = {
        { "receiver 50 ppm fast", 4.2, 0.99995 },
        { "receiver 50 ppm slow", 12.7, 1.00005 },
        { "receiver 100 ppm slow", 0.0, 1.0001 },
    };

    for (const Point &p : points) {
        INFO(p.name);

        double evm = 0.0;
        Decoded d = runStream(stream.samples, p.start, p.step, &evm);

        checkStream(stream, timingCall, d);
        REQUIRE(evm < 0.25);
    }
}

TEST_CASE("dPMR keeps the frame grid far outside the clock tolerance",
          "[dpmr][rx][timing]")
{
    /*
     * Past 100 ppm the TCH starts to lose the odd byte -- it carries no coding
     * of its own, so what the demodulator slices is what the vocoder gets --
     * but the framer must not let go: the CCH of every payload frame still has
     * to close its CRC, and the header and the END frame still have to decode.
     * Losing the grid mid-call would silence the rest of the transmission.
     */
    constexpr int FRAMES = 8;
    AirStream stream = buildStream(timingCall, FRAMES);

    struct Point { const char *name; double start; double step; };

    const Point points[] = {
        { "receiver 200 ppm fast", 4.2, 0.9998 },
        { "receiver 200 ppm slow", 12.7, 1.0002 },
        { "receiver 500 ppm slow", 0.0, 1.0005 },
        { "receiver 500 ppm fast", 9.3, 0.9995 },
    };

    for (const Point &p : points) {
        INFO(p.name);

        Decoded d = runStream(stream.samples, p.start, p.step, nullptr);

        int payload = 0;
        int header = 0;
        int end = 0;

        for (const dpmrFrame &f : d.frames) {
            if (f.headerOk) {
                REQUIRE(f.header.calledId == timingCall.called);
                REQUIRE(f.header.ownId == timingCall.own);
                header++;
            }
            if (f.endOk)
                end++;
            if (f.cchOk && !f.headerOk && !f.endOk)
                payload++;
        }

        REQUIRE(header == 1);
        REQUIRE(payload >= FRAMES - 2);
        REQUIRE(end >= 1);
    }
}
