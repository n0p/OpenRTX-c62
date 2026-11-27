/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file ysf_phy.cpp
 * @brief The System Fusion demodulator: what it locks on, and what it refuses.
 *
 * tests/host/ysf_host runs the demodulator over an off-air capture and
 * tests/host/ysf_tx over a transmission of the firmware's own transmitter.
 * Both start from a frame that is there.  What neither of them does is ask the
 * demodulator to find a frame that is not: a 20-symbol sync correlation that
 * fires on anything is a receiver that hands the frame layer a new "frame"
 * every 100 ms of noise, and the frame layer's counters then say the channel
 * is busy when it is empty.
 *
 * The waveform here is built with the same C4FM shaper the transmitter uses
 * (raised cosine, ten samples per symbol), so the only thing being measured is
 * the synchroniser: the frame is placed anywhere in the sample stream and the
 * demodulator has to find it there and only there.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/C4FM/C4fmMod.h"
#include "protocols/YSF/YsfFrame.h"
#include "protocols/YSF/YsfPhy.h"
#include "protocols/YSF/YsfTx.h"
}

namespace {

/** Outer C4FM level of the transmitter's own harness (docs/TX_YSF.md). */
constexpr float GAIN = 2730.0f;

/** Dibits of the frame sync, from the air interface. */
const uint8_t syncDibits[YSF_SYNC_SYMBOLS] = {3, 1, 1, 0, 1, 3, 0, 1, 3, 0,
                                              2, 1, 1, 2, 0, 3, 1, 0, 3, 1};

/** The header frame the transmitter builds for a call. */
struct Frame {
    uint8_t d[YSF_FRAME_DIBITS];

    Frame() { memset(d, 0, sizeof(d)); }
};

/** A header frame, straight out of YsfTx, dibits and all. */
Frame txHeaderFrame()
{
    struct ysfTxConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.source, "EA5TEST");
    strcpy(cfg.dest, "CQCQCQ");
    strcpy(cfg.uplink, "EA5RPT");
    strcpy(cfg.downlink, "EA5RPT");
    cfg.dgid = 42;
    cfg.text = nullptr;

    ysfTx_begin(&cfg);

    /* One frame's worth of symbols: the modulator builds the next frame only
     * when the samples of this one have all been taken. */
    std::vector<int16_t> pcm((size_t)YSF_FRAME_SYMBOLS * C4FM_MOD_SPS);
    ysfTx_samples(pcm.data(), (int)pcm.size(), GAIN);

    Frame f;
    memcpy(f.d, ysfTx_frameDibits(), sizeof(f.d));
    ysfTx_end();
    return f;
}

/** C4FM waveform of a dibit stream, with silence around it. */
std::vector<int16_t> modulate(const uint8_t *dibits, int count, int lead,
                              int tail)
{
    c4fmMod_t mod;
    c4fmMod_init(&mod, GAIN, false, C4FM_PULSE_RC);

    std::vector<int16_t> out((size_t)(lead + tail), 0);
    out.resize((size_t)(lead + count * C4FM_MOD_SPS + tail), 0);

    /* Silence in, silence out: the shaper's history starts empty */
    c4fmMod_dibits(&mod, nullptr, 1, &out[0]);
    c4fmMod_dibits(&mod, dibits, count, &out[(size_t)lead]);
    c4fmMod_dibits(&mod, nullptr, 1, &out[(size_t)lead + (size_t)count *
                                          C4FM_MOD_SPS]);
    return out;
}

/** What the demodulator reported. */
struct Seen {
    int frames = 0;
    int syncErrors = 0;
    int voiceSlots = 0;
    bool fichOk = false;
    char source[YSF_CALLSIGN_LEN + 1] = {0};
};

void onFrame(const struct ysfFrame *f, void *user)
{
    auto *s = static_cast<Seen *>(user);
    s->frames++;
    s->syncErrors = f->syncErrors;

    const struct ysfDecoded *d = ysfFrame_decode(f);
    if (d == nullptr)
        return;

    s->fichOk = d->fichOk;
    s->voiceSlots = d->voiceCount;
    snprintf(s->source, sizeof(s->source), "%s", d->call.source);
}

/** Push a whole buffer in blocks, the way the audio stream hands it over. */
Seen run(const std::vector<int16_t> &pcm, size_t block)
{
    Seen s;
    ysfPhy_init();

    for (size_t off = 0; off < pcm.size(); off += block) {
        size_t n = pcm.size() - off;
        if (n > block)
            n = block;
        ysfPhy_push(&pcm[off], n, onFrame, &s);
    }
    return s;
}

} // namespace

TEST_CASE("the demodulator finds a frame wherever it is in the stream",
          "[ysf][phy]")
{
    /*
     * The frame is placed with three different amounts of silence in front of
     * it and the stream is cut into blocks of three sizes, because the audio
     * driver's block boundary has nothing to do with the symbol clock: the
     * synchroniser has to find the sync in each of them and nowhere else.
     */
    const Frame f = txHeaderFrame();

    for (int lead : {100, 1337, 4801}) {
        for (size_t block : {160u, 960u, 2400u}) {
            const std::vector<int16_t> pcm = modulate(f.d, YSF_FRAME_DIBITS,
                                                      lead, 2400);
            const Seen s = run(pcm, block);

            INFO("lead " << lead << " block " << block);
            REQUIRE(s.frames == 1);
            REQUIRE(s.fichOk);
            REQUIRE(strcmp(s.source, "EA5TEST") == 0);
            REQUIRE(s.voiceSlots == 0);   /* a header carries no voice */
            REQUIRE(s.syncErrors <= 1);
        }
    }
}

TEST_CASE("the demodulator does not invent frames out of noise", "[ysf][phy]")
{
    /*
     * A second of noise at the level of a quiet channel.  The sync is 20
     * symbols correlated on their signs: a hunt that does not gate on how many
     * of them match finds a "frame" several times a second, and every one of
     * them costs the frame layer a bad-FICH count and the mode a voice frame
     * it never received.
     */
    std::vector<int16_t> noise(48000);
    uint32_t state = 0x2468ACE0u;
    for (size_t i = 0; i < noise.size(); i++) {
        state = state * 1664525u + 1013904223u;
        noise[i] = (int16_t)(((state >> 16) & 0x7FFF) - 0x4000);
    }

    const Seen s = run(noise, 2400);
    REQUIRE(s.frames == 0);
    REQUIRE_FALSE(ysfPhy_synced());
}

TEST_CASE("a frame with the wrong sync is not a frame", "[ysf][phy]")
{
    /*
     * The payload of this frame is a perfectly good header -- FICH, callsigns
     * and all -- and the 20 sync symbols are replaced by a DC level.  Nothing
     * about the frame is wrong except that it never announced itself, so the
     * demodulator must not hand it to the frame layer.
     */
    Frame f = txHeaderFrame();
    for (int i = 0; i < YSF_SYNC_SYMBOLS; i++)
        f.d[i] = 0;

    const std::vector<int16_t> pcm = modulate(f.d, YSF_FRAME_DIBITS, 500, 2400);
    const Seen s = run(pcm, 2400);

    REQUIRE(s.frames == 0);
    REQUIRE_FALSE(ysfPhy_synced());
}

TEST_CASE("a frame that stops in the middle is not reported", "[ysf][phy]")
{
    /*
     * The carrier drops after 300 of the frame's 480 symbols.  A frame is a
     * whole 100 ms of dibits and the callback hands them all on; reporting one
     * from half a burst would give the frame layer a FICH it never received.
     */
    const Frame f = txHeaderFrame();

    for (int symbols : {120, 300, 400}) {
        const std::vector<int16_t> pcm = modulate(f.d, symbols, 500, 2400);
        const Seen s = run(pcm, 2400);
        INFO("symbols " << symbols);
        REQUIRE(s.frames == 0);
    }
}

TEST_CASE("a frame after a lost one is still found", "[ysf][phy]")
{
    /*
     * Two frames back to back with the first one's sync destroyed: the second
     * has to be found on its own.  The demodulator's timing coasting (three
     * frames on the last good offset) must not turn one lost frame into a
     * transmission that is gone.
     */
    Frame first = txHeaderFrame();
    Frame second = txHeaderFrame();
    for (int i = 0; i < YSF_SYNC_SYMBOLS; i++)
        first.d[i] = 0;

    std::vector<uint8_t> stream;
    stream.insert(stream.end(), first.d, first.d + YSF_FRAME_DIBITS);
    stream.insert(stream.end(), second.d, second.d + YSF_FRAME_DIBITS);

    const std::vector<int16_t> pcm =
        modulate(stream.data(), YSF_FRAME_DIBITS * 2, 500, 2400);
    const Seen s = run(pcm, 2400);

    REQUIRE(s.frames == 1);
    REQUIRE(s.fichOk);
    REQUIRE(strcmp(s.source, "EA5TEST") == 0);
}

namespace {

/** The transmitter's next frame as the frame layer takes it. */
struct ysfFrame nextFrame(const uint8_t *dibits)
{
    struct ysfFrame f;

    memset(&f, 0, sizeof(f));
    memcpy(f.dibits, dibits, YSF_FRAME_DIBITS);
    return f;
}

} // namespace

TEST_CASE("a frame whose FICH did not verify still hands over its voice",
          "[ysf][frame]")
{
    /*
     * The FICH is a convolutional code plus Golay(24,12) plus a CRC-16, so a
     * frame whose FICH fails is a frame with real errors -- but its payload can
     * still be intact, and it is 100 ms of speech.  Dropping the whole frame
     * leaves a hole in the speech timeline that is audible as a clip; the
     * layout of the previous frame of the same transmission is the best guess
     * for it, and the frame must not update what the transmission says.
     *
     * The frames are the transmitter's own: the header (fi 0, DT 0) followed by
     * the first voice frame of the call (fi 1, DT 0).
     */
    struct ysfTxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.source, "EA5TEST");
    strcpy(cfg.dest, "CQCQCQ");
    strcpy(cfg.uplink, "EA5RPT");
    strcpy(cfg.downlink, "EA5RPT");
    cfg.text = nullptr;

    ysfTx_begin(&cfg);

    std::vector<int16_t> pcm((size_t)YSF_FRAME_SYMBOLS * C4FM_MOD_SPS);
    ysfTx_samples(pcm.data(), (int)pcm.size(), GAIN);
    const struct ysfFrame header = nextFrame(ysfTx_frameDibits());

    ysfTx_samples(pcm.data(), (int)pcm.size(), GAIN);
    struct ysfFrame voice = nextFrame(ysfTx_frameDibits());
    ysfTx_end();

    ysfFrame_reset();
    const struct ysfDecoded *h = ysfFrame_decode(&header);

    REQUIRE(h != nullptr);
    REQUIRE(h->fichOk);
    REQUIRE(h->call.dt == 0);
    REQUIRE(ysfFrame_call()->fi == 0);

    /* The same frame with its FICH beyond repair: 200 bits after the sync,
     * flipped so that neither the Viterbi metric nor Golay can absorb it */
    for (int i = 0; i < 100; i += 3)
        voice.dibits[YSF_SYNC_SYMBOLS + i] ^= 3;

    const struct ysfDecoded *v = ysfFrame_decode(&voice);

    REQUIRE(v != nullptr);
    REQUIRE_FALSE(v->fichOk);
    REQUIRE(v->voiceCount == YSF_VOICE_SLOTS);
    REQUIRE(ysfFrame_call()->badFich == 1);

    /* It did not touch what the transmission says */
    REQUIRE(ysfFrame_call()->fi == 0);
    REQUIRE(ysfFrame_call()->frames == 1);
}

TEST_CASE("after a terminator a bad FICH does not invent voice", "[ysf][frame]")
{
    /*
     * The fallback must not run past the end of a call: a terminator ends the
     * voice layout, so a frame after it whose FICH fails is silence, not a data
     * channel decoded as speech.
     */
    struct ysfTxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.source, "EA5TEST");
    strcpy(cfg.dest, "CQCQCQ");
    cfg.text = nullptr;

    ysfTx_begin(&cfg);

    std::vector<int16_t> pcm((size_t)YSF_FRAME_SYMBOLS * C4FM_MOD_SPS);

    ysfFrame_reset();

    /* The header and one voice frame, so the call is on a voice layout */
    ysfTx_samples(pcm.data(), (int)pcm.size(), GAIN);
    const struct ysfFrame header = nextFrame(ysfTx_frameDibits());
    ysfTx_samples(pcm.data(), (int)pcm.size(), GAIN);
    const struct ysfFrame voice = nextFrame(ysfTx_frameDibits());

    REQUIRE(ysfFrame_decode(&header)->fichOk);
    REQUIRE(ysfFrame_decode(&voice)->voiceCount == YSF_VOICE_SLOTS);

    /* The end of the transmission: the terminator is the next frame out */
    ysfTx_end();
    ysfTx_samples(pcm.data(), (int)pcm.size(), GAIN);
    struct ysfFrame term = nextFrame(ysfTx_frameDibits());
    ysfTx_end();

    const struct ysfDecoded *t = ysfFrame_decode(&term);

    REQUIRE(t->fichOk);
    REQUIRE(t->call.fi == 2);

    for (int i = 0; i < 100; i += 3)
        term.dibits[YSF_SYNC_SYMBOLS + i] ^= 3;

    const struct ysfDecoded *v = ysfFrame_decode(&term);

    REQUIRE_FALSE(v->fichOk);
    REQUIRE(v->voiceCount == 0);
}
