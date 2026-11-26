/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dstar_phy.cpp
 * @brief A whole D-STAR DV transmission through the demodulator, and the three
 * ways it must not become one.
 *
 * tests/host/dstar runs the same transmitter-receiver loop with a BER sweep and
 * real captures; what it does not do is ask the receiver to refuse anything.
 * The demodulator's state machine is the one place where a wrong answer is
 * expensive: it stays in RX_HEADER collecting 660 soft bits, then in RX_DATA
 * handing 96-bit frames to the status, and a header it accepts by mistake
 * invents a call.  So the cases here are:
 *
 *  - a transmission that is there: the header, the 96-bit frames, the resync
 *    word every 21st frame and the end pattern, with the status published the
 *    way OpMode_DSTAR publishes it;
 *  - a transmission cut inside its header: no call at all;
 *  - a transmission cut inside its frames: the call it announced, and only the
 *    frames that arrived;
 *  - noise: nothing.
 *
 * The voice is the D-STAR null AMBE frame the transmitter sends when it has no
 * encoder, and the vocoder front end is checked on it and on its flipped bits:
 * those are the two error counts AMBE 3600x2400's repeat and mute decisions are
 * made of (82 5).  There is no 2400 decoder in this build, which is why the
 * mode plays silence; dstarVocoder_available() says so.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/DSTAR/DStarDefs.h"
#include "protocols/DSTAR/DStarPhy.h"
#include "protocols/DSTAR/DStarStatus.h"
#include "protocols/DSTAR/DStarTx.h"
#include "protocols/DSTAR/DStarVocoder.h"
}

namespace {

/** Amplitude of the modulating waveform: the mode's own constant. */
constexpr int AMPLITUDE = 24000;

/** What one run of the demodulator produced, status included. */
struct Seen {
    int headers = 0;
    int frames = 0;
    int resyncs = 0;
    int eot = 0;
    int voiceMismatches = 0;
    int voiceCompared = 0;
    struct dstarHeader header;
    uint32_t expected = 0;
};

void onHeader(const struct dstarHeader *hdr, int syncErrors, void *user)
{
    auto *s = static_cast<Seen *>(user);
    s->headers++;
    s->header = *hdr;
    dstarStatus_header(hdr, syncErrors, 1000);
}

void onFrame(const struct dstarPhyFrame *f, void *user)
{
    auto *s = static_cast<Seen *>(user);
    s->frames++;
    if (f->sync)
        s->resyncs++;

    /* The transmitter sends the null AMBE frame in every slot, so every
     * received voice bit can be checked against a known value.  Frames past the
     * transmission are not part of it. */
    if (f->index < s->expected) {
        for (int i = 0; i < DSTAR_VOICE_BYTES; i++) {
            uint8_t x = (uint8_t)(f->voice[i] ^ dstarNullVoice[i]);
            while (x != 0) {
                x &= (uint8_t)(x - 1);
                s->voiceMismatches++;
            }
        }
        s->voiceCompared++;
    }

    dstarStatus_frame(f, 1000 + f->index * 20);
}

void onEot(void *user)
{
    static_cast<Seen *>(user)->eot++;
}

/** A DV transmission of the firmware's own transmitter, with a message. */
std::vector<int16_t> makeTransmission(int voiceFrames, const char *text)
{
    struct dstarTxConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.hdr.flags[0] = DSTAR_FLAG1_REPEATER;
    strcpy(cfg.hdr.rpt2, "ED1ZAR G");
    strcpy(cfg.hdr.rpt1, "ED1ZAR B");
    strcpy(cfg.hdr.ur, "CQCQCQ");
    strcpy(cfg.hdr.my, "EA5TEST");
    strcpy(cfg.hdr.sfx, "C62");
    cfg.text = text;
    cfg.repeatHeader = true;
    cfg.useNullVoice = true;

    dstarTx_begin(&cfg);

    /* Bit sync, frame sync, coded header and the frames, then the end pattern
     * and 50 ms of receiver noise floor so the last timing block completes. */
    const int64_t bits = DSTAR_BITSYNC_BITS + DSTAR_FRAME_SYNC_BITS +
                         DSTAR_CODED_BITS + (int64_t)voiceFrames * DSTAR_FRAME_BITS;
    const int frames = (int)((bits * 48000) / DSTAR_BAUD) + 64;
    const int end = (48 * 48000) / DSTAR_BAUD + 4800;

    std::vector<int16_t> pcm((size_t)(frames + end), 0);
    dstarTx_samples(pcm.data(), frames, AMPLITUDE);
    dstarTx_end();
    dstarTx_samples(&pcm[(size_t)frames], end, AMPLITUDE);

    return pcm;
}

/** Push samples through the demodulator in 160-sample blocks, as the mode does. */
Seen run(const std::vector<int16_t> &pcm, uint32_t expectedFrames)
{
    Seen s;
    s.expected = expectedFrames;

    dstarStatus_clear();
    dstarPhy_reset();

    for (size_t off = 0; off < pcm.size(); off += 160) {
        const size_t n = (pcm.size() - off < 160) ? pcm.size() - off : 160;
        dstarPhy_push(&pcm[off], n, onHeader, onFrame, onEot, &s);
    }

    return s;
}

} // namespace

TEST_CASE("a DV transmission publishes its header, its frames and its resyncs",
          "[dstar][phy]")
{
    const int voiceFrames = 40;
    const std::vector<int16_t> pcm = makeTransmission(voiceFrames,
                                                      "VOZ D-STAR C62");
    Seen s = run(pcm, (uint32_t)voiceFrames);

    REQUIRE(s.headers == 1);
    REQUIRE(strcmp(s.header.my, "EA5TEST") == 0);
    REQUIRE(strcmp(s.header.sfx, "C62") == 0);
    REQUIRE(strcmp(s.header.ur, "CQCQCQ") == 0);
    REQUIRE(strcmp(s.header.rpt1, "ED1ZAR B") == 0);
    REQUIRE(strcmp(s.header.rpt2, "ED1ZAR G") == 0);
    REQUIRE(s.header.flags[0] == DSTAR_FLAG1_REPEATER);
    REQUIRE(s.header.crcOk);
    REQUIRE(s.header.viterbi == 0);

    /* Every frame of the transmission, and the few the demodulator slices out
     * of the end pattern and the silence behind it before it decides the call
     * is over (the transmitter stops mid-frame and the end pattern is 48 bits
     * long, so up to a frame and a bit of it are on the air).  Four is the
     * measured count; what matters here is that no frame of the transmission
     * is missing. */
    INFO("frames " << s.frames << " resyncs " << s.resyncs);
    REQUIRE(s.frames >= voiceFrames);
    REQUIRE(s.frames <= voiceFrames + 4);

    /* The resync word rides in frame 0 and every 21st after it */
    const int expectedResyncs =
        (voiceFrames + DSTAR_SUPERFRAME - 1) / DSTAR_SUPERFRAME;
    REQUIRE(s.resyncs == expectedResyncs);

    /*
     * The transmission ends where its end pattern is, not four seconds later on
     * the timeout: DStarTx sends the 48 bits of STD-7 4.1.2 h after
     * dstarTx_end(), and the demodulator's bit history holds 48 bits so it can
     * match them.  Before that it held 32, the comparison could never be true,
     * and every call ended by timeout with the tail of it read as all-zero
     * voice.
     */
    REQUIRE(s.eot == 1);

    /* What the screen reads, published the way the mode publishes it */
    struct dstarStatus st;
    dstar_getStatus(&st);
    REQUIRE(st.headers == 1);
    REQUIRE(st.frames >= (uint32_t)voiceFrames);
    REQUIRE(strcmp(st.my, "EA5TEST") == 0);
    REQUIRE(strcmp(st.ur, "CQCQCQ") == 0);
    REQUIRE(strcmp(st.rpt1, "ED1ZAR B") == 0);
    REQUIRE(strcmp(st.text, "VOZ D-STAR C62") == 0);

    const struct dstarHeardStation *heard = dstarHeard(0);
    REQUIRE(heard != nullptr);
    REQUIRE(strcmp(heard->call, "EA5TEST") == 0);
}

TEST_CASE("the voice of the transmission arrives bit for bit", "[dstar][phy]")
{
    /* The 72-bit voice field of every frame is carried unprotected (its own
     * Golay words are inside it), so with the transmitter and the demodulator
     * agreeing there is no margin to hide an error in: the comparison is
     * zero-tolerance. */
    const int voiceFrames = 20;
    const std::vector<int16_t> pcm = makeTransmission(voiceFrames, nullptr);
    Seen s = run(pcm, (uint32_t)voiceFrames);

    INFO("voice frames compared " << s.voiceCompared << " mismatches "
                                  << s.voiceMismatches);
    REQUIRE(s.voiceCompared >= voiceFrames - 1);
    REQUIRE(s.voiceMismatches == 0);
}

TEST_CASE("a transmission cut inside its header is not a call", "[dstar][phy]")
{
    /*
     * The carrier drops 300 of the 660 coded header bits in.  The demodulator
     * has collected a frame sync and part of a header, and the rest of its
     * buffer fills with silence: it must go back to hunting, call no header,
     * publish no call and hand no frame to the status.  Accepting one gives the
     * operator a station that does not exist.
     */
    std::vector<int16_t> pcm = makeTransmission(40, nullptr);

    /* 64 + 15 + 300 bits at 10 samples per bit, plus the shaper's tail */
    const size_t cut = (size_t)((DSTAR_BITSYNC_BITS + DSTAR_FRAME_SYNC_BITS + 300) *
                                48000 / DSTAR_BAUD) + 100;
    pcm.resize(cut);
    pcm.resize(cut + 4800, 0);

    Seen s = run(pcm, 0);

    REQUIRE(s.headers == 0);
    REQUIRE(s.frames == 0);
    REQUIRE_FALSE(dstarPhy_synced());

    struct dstarStatus st;
    dstar_getStatus(&st);
    REQUIRE(st.headers == 0);
    REQUIRE(st.frames == 0);
    REQUIRE(st.my[0] == '\0');
    REQUIRE(dstarHeard(0) == nullptr);
}

TEST_CASE("a transmission cut inside its frames keeps what arrived",
          "[dstar][phy]")
{
    /*
     * Header and five frames in, the carrier goes away.  The call is real --
     * the header was accepted -- and the frames that arrived are the frames that
     * are published; the ones that did not are simply not there.  A receiver
     * that drops the whole call because the terminator never came loses the
     * beginning of the transmission, which is the part with the callsign.
     */
    const int voiceFrames = 40;
    std::vector<int16_t> pcm = makeTransmission(voiceFrames, nullptr);

    const int64_t bits = DSTAR_BITSYNC_BITS + DSTAR_FRAME_SYNC_BITS +
                         DSTAR_CODED_BITS + 5 * (int64_t)DSTAR_FRAME_BITS;
    const size_t cut = (size_t)((bits * 48000) / DSTAR_BAUD) + 64;
    pcm.resize(cut);
    pcm.resize(cut + 4800, 0);

    Seen s = run(pcm, (uint32_t)voiceFrames);

    REQUIRE(s.headers == 1);
    REQUIRE(strcmp(s.header.my, "EA5TEST") == 0);
    REQUIRE(s.eot == 0);                    /* the transmission never ended */
    /* Frames keep coming out of the noise floor for a while (the demodulator
     * stays in RX_DATA until the end pattern or two superframes without a
     * resync), so the assertion is on the part that is real: the five frames
     * that arrived, and the call they belong to. */
    INFO("frames " << s.frames << " mismatches " << s.voiceMismatches);
    REQUIRE(s.frames >= 4);

    struct dstarStatus st;
    dstar_getStatus(&st);
    REQUIRE(st.headers == 1);
    REQUIRE(strcmp(st.my, "EA5TEST") == 0);
}

TEST_CASE("noise is not a transmission", "[dstar][phy]")
{
    std::vector<int16_t> noise(48000);
    uint32_t state = 0xC0FFEE01u;
    for (size_t i = 0; i < noise.size(); i++) {
        state = state * 1664525u + 1013904223u;
        noise[i] = (int16_t)(((state >> 16) & 0x7FFF) - 0x4000);
    }

    Seen s = run(noise, 0);

    REQUIRE(s.headers == 0);
    REQUIRE(s.frames == 0);
    REQUIRE(s.eot == 0);
    REQUIRE_FALSE(dstarPhy_synced());

    struct dstarStatus st;
    dstar_getStatus(&st);
    REQUIRE(st.headers == 0);
    REQUIRE(st.frames == 0);
    REQUIRE_FALSE(st.sync);
}

TEST_CASE("the AMBE front end reports the errors of each FEC word",
          "[dstar][vocoder]")
{
    /*
     * AMBE 3600x2400's repeat and mute decisions are driven by the two error
     * counts separately (mbelib's process path repeats a frame whose
     * c0 + protected errors exceed three), so the front end has to attribute
     * every channel bit to the word it came from: 24 bits of the c0 Golay word,
     * 12 of the c1 word (through the PN sequence), and the 36 unprotected bits
     * of c2/c3 that nothing can check.
     *
     * The null frame is the one a silent D-STAR radio sends, and it is a valid
     * codeword: no errors.  Flipping one air bit at a time is the whole
     * attribution map in 72 steps.
     */
    uint8_t params[DSTAR_AMBE_BITS];
    int c0 = -1;
    int prot = -1;

    REQUIRE(dstarVocoder_frame(dstarNullVoice, params, &c0, &prot) == 0);
    REQUIRE(c0 == 0);
    REQUIRE(prot == 0);

    int c0Bits = 0;
    int protBits = 0;
    int unprotected = 0;

    for (int bit = 0; bit < 72; bit++) {
        uint8_t air[DSTAR_VOICE_BYTES];
        memcpy(air, dstarNullVoice, sizeof(air));
        air[bit >> 3] ^= (uint8_t)(1u << (bit & 7));

        uint8_t p[DSTAR_AMBE_BITS];
        int e0 = 0;
        int ep = 0;
        INFO("air bit " << bit);
        REQUIRE(dstarVocoder_frame(air, p, &e0, &ep) == 0);
        REQUIRE((e0 + ep) <= 1);          /* one bit, at most one word */

        if (e0 != 0)
            c0Bits++;
        else if (ep != 0)
            protBits++;
        else
            unprotected++;
    }

    /* 24 + 12 protected, 36 not: the geometry of the D-STAR channel frame */
    REQUIRE(c0Bits == 24);
    REQUIRE(protBits == 12);
    REQUIRE(unprotected == 36);

    /* The aggregate call the PHY-side users make reports the sum */
    uint8_t air[DSTAR_VOICE_BYTES];
    memcpy(air, dstarNullVoice, sizeof(air));
    for (int i = 0; i < 6; i++)
        air[i] ^= 0x55;                   /* both protected words, badly */
    int errors = -1;
    REQUIRE(dstarVocoder_params(air, params, &errors) == 0);
    INFO("errors " << errors);
    REQUIRE(errors > 3);                  /* past mbelib's repeat threshold */
}

TEST_CASE("no 2400 decoder is linked, and the mode says so", "[dstar][vocoder]")
{
    /*
     * The D-STAR speech path asks this before it promises audio: without a
     * decoder the mode plays silence and the screen still shows the call, which
     * is the honest state of this build (docs/RX_DSTAR.md).  A platform that
     * links one overrides both weak symbols, and this case is what tells the
     * two builds apart.
     */
    int16_t pcm[DSTAR_PCM_SAMPLES];

    if (dstarVocoder_available()) {
        REQUIRE(dstarVocoder_decode(dstarNullVoice, pcm) == 0);
    } else {
        REQUIRE(dstarVocoder_decode(dstarNullVoice, pcm) == -1);
    }
}
