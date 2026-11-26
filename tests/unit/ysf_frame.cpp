/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file ysf_frame.cpp
 * @brief The System Fusion frame layer: what it publishes, and what it refuses.
 *
 * tests/host/ysf_tx closes the modulation loop (FICH, data channel, voice and
 * the C4FM waveform through the firmware's own receiver) and compares every
 * field of a call that is on the air.  That is the happy path, and it is the
 * one a clean bench signal always takes.  What it never does is hand the frame
 * layer a frame it must *not* believe:
 *
 *  - a FICH whose Golay words or CRC do not check (a burst lost to noise keeps
 *    the same 100 ms grid, so this is the common case on a busy channel);
 *  - a data channel block whose CRC-16 fails, while the unprotected voice of
 *    the same frame is still perfectly readable;
 *  - a voice mode this build cannot decode (full-rate voice), which must be
 *    counted as unsupported instead of being handed to the vocoder as rubbish;
 *  - a V/D mode 2 slot whose fixed bit says the slot is corrupt.
 *
 * The frames for those cases cannot come from YsfTx (it transmits one kind of
 * call), so this file carries the encoding half of the frame layer: the same
 * CRC-16, Golay(24,12), rate-1/2 K=5 code, interleavers and PN(9,5) whitening
 * that YsfTx.c runs.  Two cases keep that builder honest -- one compares it
 * with the transmitter's own header frame, one decodes it back -- because a
 * builder that is wrong would make every refusal below pass for the wrong
 * reason.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/C4FM/C4fmMod.h"
#include "protocols/YSF/YsfFrame.h"
#include "protocols/YSF/YsfPhy.h"
#include "protocols/YSF/YsfTx.h"
}

namespace {

/** Dibits of the frame sync: 0x7FFD... in the receiver's terms. */
const uint8_t syncDibits[YSF_SYNC_SYMBOLS] = {3, 1, 1, 0, 1, 3, 0, 1, 3, 0,
                                              2, 1, 1, 2, 0, 3, 1, 0, 3, 1};

/** One frame as the demodulator hands it over: 480 dibits, sync first. */
struct Frame {
    uint8_t d[YSF_FRAME_DIBITS];

    Frame() { memset(d, 0, sizeof(d)); }

    /** What the frame layer takes. */
    struct ysfFrame phy() const
    {
        struct ysfFrame f;
        memset(&f, 0, sizeof(f));
        memcpy(f.dibits, d, sizeof(d));
        return f;
    }
};

/* ------------------------------------------------------------ encoding --- */

/** CRC-16 of System Fusion: 0x1021, zero register, inverted result. */
uint16_t crc16(const uint8_t *bits, int len)
{
    uint32_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc = ((crc << 1) | (bits[i] & 1u)) & 0x1FFFFu;
        if (crc & 0x10000u)
            crc = (crc & 0xFFFFu) ^ 0x1021u;
    }
    return (uint16_t)((crc ^ 0xFFFFu) & 0xFFFFu);
}

/** Systematic extended Golay(24,12). */
uint32_t golay24(uint32_t m)
{
    uint32_t cw = (m & 0xFFFu) << 11;
    int ones = 0;

    for (int i = 22; i >= 11; i--)
        if (cw & (1u << i))
            cw ^= 0xC75u << (i - 11);

    const uint32_t word = ((m & 0xFFFu) << 11) | (cw & 0x7FFu);
    for (uint32_t v = word; v != 0; v >>= 1)
        ones += (int)(v & 1u);

    return (word << 1) | (uint32_t)(ones & 1);
}

/** Rate 1/2, K = 5, generators 0o31 / 0o27. */
void convEncode(const uint8_t *in, int steps, uint8_t *out)
{
    static const uint8_t parity32[32] = {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1,
                                         0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
                                         0, 1, 1, 0, 1, 0, 0, 1};
    int reg = 0;

    for (int i = 0; i < steps; i++) {
        reg = ((reg << 1) | (in[i] & 1)) & 0x1F;
        out[i] = (uint8_t)((parity32[reg & 0x19] << 1) | parity32[reg & 0x17]);
    }
}

/** Block interleaver, written row by row and read column by column. */
void interleave(const uint8_t *in, int rows, int cols, uint8_t *out)
{
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
            out[i + j * rows] = in[j + i * cols];
}

/** PN(9,5) whitening, which is its own inverse. */
void whiten(uint8_t *bits, int n)
{
    uint16_t reg = 0x1C9;

    for (int i = 0; i < n; i++) {
        const uint8_t bit = (uint8_t)(reg & 1u);
        bits[i] ^= bit;
        const uint8_t feedback = (uint8_t)(((reg >> 4) ^ reg) & 1u);
        reg = (uint16_t)((reg >> 1) | ((uint16_t)feedback << 8));
    }
}

void put(uint8_t *bits, int offset, int length, uint32_t value)
{
    for (int i = 0; i < length; i++)
        bits[offset + i] = (uint8_t)((value >> (length - 1 - i)) & 1u);
}

void putCallsign(uint8_t *bits, int offset, const char *call)
{
    /* Stop at the terminator: the transmitter reads a memset-then-strcpy
     * config field, this builder takes a string literal, and reading past the
     * NUL is how a frame ends up carrying the next literal in .rodata. */
    int n = 0;
    while ((n < YSF_CALLSIGN_LEN) && (call[n] != '\0'))
        n++;

    for (int i = 0; i < YSF_CALLSIGN_LEN; i++) {
        const char c = (i < n) ? call[i] : ' ';
        put(bits, offset + 8 * i, 8, (uint8_t)c);
    }
}

/** Rows and columns of the two data channel block sizes. */
struct DchShape {
    int steps;
    int rows;
    int cols;
};

constexpr DchShape DCH_160 = {180, 20, 9};
constexpr DchShape DCH_80 = {100, 20, 5};

/**
 * Build one data channel block.
 *
 * @param data: the information bits (160 or 80).
 * @param shape: block size.
 * @param dibits: receives the interleaved dibits.
 */
void buildDch(const uint8_t *data, const DchShape &shape, uint8_t *dibits)
{
    uint8_t info[200];
    uint8_t air[200];
    const int dataBits = shape.steps - 4 - 16;

    memset(info, 0, sizeof(info));
    memcpy(info, data, (size_t)dataBits);
    whiten(info, dataBits);
    put(info, dataBits, 16, crc16(info, dataBits + 16));
    memset(&info[dataBits + 16], 0, 4); /* tail */

    convEncode(info, shape.steps, air);
    interleave(air, shape.rows, shape.cols, dibits);
}

/**
 * Build the coded FICH of a frame.
 *
 * @param dgid: 0..99 for a DG-ID, -1 for none (the squelch flag set).
 */
void buildFich(Frame &f, uint8_t fi, uint8_t bn, uint8_t fn, uint8_t ft,
               uint8_t dt, int dgid)
{
    uint8_t info[48];
    uint8_t coded[96];
    uint8_t payload[100];
    uint8_t air[100];

    memset(info, 0, sizeof(info));
    put(info, 0, 2, fi);
    put(info, 2, 2, 2);   /* callsigns assigned */
    put(info, 4, 2, 0);   /* call mode: group */
    put(info, 6, 2, bn);
    put(info, 8, 2, 0);   /* block type */
    put(info, 10, 3, fn);
    put(info, 13, 3, ft);
    put(info, 18, 3, 1);  /* message / repeater */
    put(info, 21, 1, 1);  /* voice present */
    put(info, 22, 2, dt);
    put(info, 24, 1, (dgid >= 0) ? 0 : 1); /* inverted: clear means DG-ID */
    put(info, 25, 7, (dgid >= 0) ? (uint32_t)dgid : 0u);
    put(info, 32, 16, crc16(info, 48));

    for (int g = 0; g < 4; g++) {
        uint32_t m = 0;
        for (int i = 0; i < 12; i++)
            m = (m << 1) | (info[g * 12 + i] & 1u);

        const uint32_t w = golay24(m);
        for (int i = 0; i < 24; i++)
            coded[g * 24 + i] = (uint8_t)((w >> (23 - i)) & 1u);
    }

    memcpy(payload, coded, 96);
    memset(&payload[96], 0, 4);
    convEncode(payload, 100, air);
    interleave(air, 20, 5, &f.d[YSF_SYNC_SYMBOLS]);
}

/** Callsigns and nodes of a call, the fields every frame of it repeats. */
struct Call {
    const char *dest = "CQCQCQ";
    const char *source = "EA5TEST";
    const char *uplink = "EA5RPT";
    const char *downlink = "EA5RPT";
    int dgid = 42;
};

/** The two data channel blocks of a header or terminator. */
void callsignPayload(const Call &c, int block, uint8_t *dibits)
{
    uint8_t data[160];
    memset(data, 0, sizeof(data));

    if (block == 0) {
        putCallsign(data, 0, c.dest);
        putCallsign(data, 80, c.source);
    } else {
        putCallsign(data, 0, c.downlink);
        putCallsign(data, 80, c.uplink);
    }

    buildDch(data, DCH_160, dibits);
}

/** A header (fi = 0) or terminator (fi = 2) frame. */
Frame callsignFrame(uint8_t fi, const Call &c)
{
    Frame f;
    memcpy(f.d, syncDibits, sizeof(syncDibits));
    buildFich(f, fi, 0, 0, 0, 0, c.dgid);

    uint8_t block[2][180];
    for (int b = 0; b < 2; b++)
        callsignPayload(c, b, block[b]);

    uint8_t *payload = &f.d[YSF_SYNC_SYMBOLS + 100];
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 5; i++)
            memcpy(&payload[(i * 2 + b) * 36], &block[b][i * 36], 36);

    return f;
}

/**
 * One V/D mode 1 voice frame: five CSD blocks and five 72-bit voice slots.
 *
 * @param voice: five AMBE+2 channel frames, nine bytes each.
 */
Frame voiceFrameMode1(uint8_t fn, uint8_t ft, const uint8_t voice[YSF_VOICE_SLOTS][9],
                      const Call &c, uint8_t bn)
{
    Frame f;
    memcpy(f.d, syncDibits, sizeof(syncDibits));
    buildFich(f, 1, bn, fn, ft, 0, c.dgid);

    uint8_t csd[160];
    uint8_t block[180];
    memset(csd, 0, sizeof(csd));
    if (bn == 0) {
        putCallsign(csd, 0, c.dest);
        putCallsign(csd, 80, c.source);
    } else {
        putCallsign(csd, 0, c.uplink);
        putCallsign(csd, 80, c.downlink);
    }
    buildDch(csd, DCH_160, block);

    uint8_t *payload = &f.d[YSF_SYNC_SYMBOLS + 100];
    for (int i = 0; i < YSF_VOICE_SLOTS; i++) {
        memcpy(&payload[i * 72], &block[i * 36], 36);

        for (int b = 0; b < 36; b++) {
            const int b0 = 2 * b;
            const uint8_t bit0 =
                (uint8_t)((voice[i][b0 >> 3] >> (7 - (b0 & 7))) & 1u);
            const uint8_t bit1 =
                (uint8_t)((voice[i][(b0 + 1) >> 3] >> (7 - ((b0 + 1) & 7))) & 1u);
            payload[i * 72 + 36 + b] = (uint8_t)((bit0 << 1) | bit1);
        }
    }

    return f;
}

/**
 * One V/D mode 2 voice frame: five data channel blocks of 80 bits and five
 * 52-dibit voice slots carrying 49 AMBE parameter bits each.
 *
 * @param params: five 49-bit parameter sets.
 * @param fixedBroken: set the fixed bit of every slot, i.e. mark it corrupt.
 */
Frame voiceFrameMode2(uint8_t fn, uint8_t ft,
                      const uint8_t params[YSF_VOICE_SLOTS][49],
                      const Call &c, bool fixedBroken)
{
    Frame f;
    memcpy(f.d, syncDibits, sizeof(syncDibits));
    buildFich(f, 1, 0, fn, ft, 2, c.dgid);

    uint8_t dch[80];
    uint8_t block[100];
    memset(dch, 0, sizeof(dch));
    putCallsign(dch, 0, c.dest);
    buildDch(dch, DCH_80, block);

    uint8_t *payload = &f.d[YSF_SYNC_SYMBOLS + 100];
    for (int i = 0; i < YSF_VOICE_SLOTS; i++) {
        memcpy(&payload[i * 72], &block[i * 20], 20);

        /* 104 data bits: 27 triples, 22 unprotected bits, the fixed zero */
        uint8_t ordered[104];
        memset(ordered, 0, sizeof(ordered));

        for (int j = 0; j < 27; j++) {
            ordered[3 * j] = params[i][j];
            ordered[3 * j + 1] = params[i][j];
            ordered[3 * j + 2] = params[i][j];
        }
        for (int j = 0; j < 22; j++)
            ordered[81 + j] = params[i][27 + j];
        ordered[103] = fixedBroken ? 1 : 0;

        whiten(ordered, 104);

        /* Air order: data index a + 26 b travels at 4 a + b */
        uint8_t air[104];
        for (int k = 0; k < 104; k++)
            air[4 * (k % 26) + (k / 26)] = ordered[k];

        for (int b = 0; b < 52; b++)
            payload[i * 72 + 20 + b] =
                (uint8_t)((air[2 * b] << 1) | air[2 * b + 1]);
    }

    return f;
}

/** A frame announcing a voice mode this build cannot decode. */
Frame voiceFrFrame(uint8_t fn, uint8_t ft, const Call &c)
{
    Frame f;
    memcpy(f.d, syncDibits, sizeof(syncDibits));
    buildFich(f, 1, 0, fn, ft, 3, c.dgid);
    return f;
}

/** What the frame layer reported for one frame. */
struct Seen {
    bool ok = false;
    struct ysfCallInfo call;
    int voiceCount = 0;
    struct ysfVoiceSlot voice[YSF_VOICE_SLOTS];
};

/** Decode one frame and copy what it said, so later frames cannot change it. */
Seen decode(const Frame &f)
{
    Seen s;
    const struct ysfDecoded *d = ysfFrame_decode(&f.phy());
    if (d == nullptr)
        return s;

    s.ok = d->fichOk;
    s.call = d->call;
    s.voiceCount = d->voiceCount;
    memcpy(s.voice, d->voice, sizeof(s.voice));
    return s;
}

/** Bits of one 72-bit voice slot, MSB first. */
bool slotBitsEqual(const struct ysfVoiceSlot &slot, const uint8_t frame[9])
{
    if (slot.nbits != 72)
        return false;
    for (int b = 0; b < 72; b++) {
        const uint8_t tx = (uint8_t)((frame[b >> 3] >> (7 - (b & 7))) & 1u);
        if ((uint8_t)(slot.bits[b] & 1u) != tx)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("the frame builder of this test is the one the transmitter runs",
          "[ysf][frame]")
{
    /*
     * YsfTx builds its frames with the same coding chain, so a header frame
     * built here and the one the transmitter shapes must be the same 480
     * dibits.  Without this, a wrong builder would make every refusal below
     * pass for the wrong reason (a frame that decodes to nothing "refuses" the
     * good case too).
     */
    struct ysfTxConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.source, "EA5TEST");
    strcpy(cfg.dest, "CQCQCQ");
    strcpy(cfg.uplink, "EA5RPT");
    strcpy(cfg.downlink, "EA5RPT");
    cfg.dgid = 42;
    cfg.text = nullptr;

    ysfTx_begin(&cfg);

    /* Enough samples for one frame of 480 symbols at ten samples per symbol */
    int16_t samples[YSF_FRAME_SYMBOLS * C4FM_MOD_SPS];
    ysfTx_samples(samples, (int)(sizeof(samples) / sizeof(samples[0])), 2730.0f);

    const uint8_t *tx = ysfTx_frameDibits();
    const Frame mine = callsignFrame(0, Call());
    ysfTx_end();

    for (int i = 0; i < YSF_FRAME_DIBITS; i++) {
        INFO("dibit " << i);
        REQUIRE(tx[i] == mine.d[i]);
    }
}

TEST_CASE("a header frame publishes the call it announces", "[ysf][frame]")
{
    ysfFrame_reset();

    const Frame f = callsignFrame(0, Call());
    const Seen s = decode(f);

    REQUIRE(s.ok);
    REQUIRE(s.call.fi == 0);
    REQUIRE(s.call.dt == 0);
    REQUIRE(strcmp(s.call.dest, "CQCQCQ") == 0);
    REQUIRE(strcmp(s.call.source, "EA5TEST") == 0);
    REQUIRE(strcmp(s.call.uplink, "EA5RPT") == 0);
    REQUIRE(strcmp(s.call.downlink, "EA5RPT") == 0);
    REQUIRE(s.call.dataBlocks == 2);
    REQUIRE(s.call.frames == 1);
    REQUIRE(s.call.badFich == 0);

    /* And what the UI reads between frames is the same thing */
    const struct ysfCallInfo *live = ysfFrame_call();
    REQUIRE(live != nullptr);
    REQUIRE(strcmp(live->source, "EA5TEST") == 0);
}

TEST_CASE("a frame with a broken FICH publishes nothing new", "[ysf][frame]")
{
    /*
     * Six dibits of the FICH wrong: past the reach of the Golay and the CRC.
     * The frame must be counted as a bad FICH and must not touch what the
     * transmission says -- the previous frame's callsigns are still the call,
     * and a bad frame does not make the destination disappear.
     */
    ysfFrame_reset();

    const Frame good = callsignFrame(0, Call());
    const Seen before = decode(good);
    REQUIRE(before.ok);
    REQUIRE(strcmp(before.call.source, "EA5TEST") == 0);

    /* Twenty-five of the hundred dibits of the FICH across both bits: past
     * the Golay words, the rate-1/2 code and the 20x5 interleaver, which do
     * correct a handful of channel errors (that is the happy path). */
    Frame bad = good;
    for (int i = 0; i < 25; i++)
        bad.d[YSF_SYNC_SYMBOLS + 4 * i] ^= 0x3;

    const Seen s = decode(bad);
    REQUIRE_FALSE(s.ok);
    REQUIRE(s.call.badFich == 1);
    REQUIRE(s.call.frames == 1);            /* not counted as a frame */
    REQUIRE(strcmp(s.call.source, "EA5TEST") == 0);
    REQUIRE(strcmp(s.call.dest, "CQCQCQ") == 0);
    REQUIRE(s.call.dataBlocks == 2);        /* untouched */

    /* A good frame afterwards is still read: the refusal costs one frame */
    const Seen after = decode(good);
    REQUIRE(after.ok);
    REQUIRE(after.call.badFich == 1);
    REQUIRE(after.call.frames == 2);
}

TEST_CASE("a frame that is only a sync and noise is refused", "[ysf][frame]")
{
    ysfFrame_reset();

    Frame f;
    memcpy(f.d, syncDibits, sizeof(syncDibits));
    for (int i = YSF_SYNC_SYMBOLS; i < YSF_FRAME_DIBITS; i++)
        f.d[i] = (uint8_t)((i * 7 + 3) & 3);

    const Seen s = decode(f);
    REQUIRE_FALSE(s.ok);
    REQUIRE(s.call.badFich == 1);
    REQUIRE(s.call.frames == 0);
    REQUIRE(s.call.source[0] == '\0');
}

TEST_CASE("a broken data channel keeps the voice and the call", "[ysf][frame]")
{
    /*
     * The data channel block of a V/D mode 1 frame is protected (CRC-16 and the
     * convolutional code), the voice next to it is not, because the AMBE+2
     * frame carries its own FEC.  Break the block and the layer must drop the
     * block -- no new callsigns, dataBlocks unchanged -- while the five voice
     * slots of the same frame still go on to the vocoder untouched.
     */
    ysfFrame_reset();

    const Call call;
    uint8_t voice[YSF_VOICE_SLOTS][9];
    for (int i = 0; i < YSF_VOICE_SLOTS; i++)
        for (int b = 0; b < 9; b++)
            voice[i][b] = (uint8_t)(i * 37 + b * 11 + 5);

    const Frame good = voiceFrameMode1(0, 4, voice, call, 0);
    const Seen g = decode(good);
    REQUIRE(g.ok);
    REQUIRE(g.voiceCount == YSF_VOICE_SLOTS);
    REQUIRE(g.call.dataBlocks == 1);
    REQUIRE(strcmp(g.call.source, "EA5TEST") == 0);

    /* Every CSD dibit of the frame destroyed across both bits: the 180-dibit
     * block the CRC-16 and the convolutional code protect cannot come back
     * from that.  The voice fields next to them are untouched. */
    Frame bad = good;
    for (int i = 0; i < YSF_VOICE_SLOTS; i++)
        for (int k = 0; k < 36; k++)
            bad.d[YSF_SYNC_SYMBOLS + 100 + i * 72 + k] ^= 0x3;

    const Seen s = decode(bad);
    REQUIRE(s.ok);                          /* the FICH is intact */
    REQUIRE(s.call.dataBlocks == 1);        /* the block was refused */
    REQUIRE(s.voiceCount == YSF_VOICE_SLOTS);
    for (int i = 0; i < YSF_VOICE_SLOTS; i++) {
        INFO("slot " << i);
        REQUIRE(slotBitsEqual(s.voice[i], voice[i]));
    }
    /* The call is still the one the previous frame announced */
    REQUIRE(strcmp(s.call.source, "EA5TEST") == 0);
    REQUIRE(strcmp(s.call.dest, "CQCQCQ") == 0);

    /* And a few errors are not enough to lose the block: four dibits wrong and
     * it still decodes, which is what the code is there for. */
    Frame light = good;
    for (int i = 0; i < 4; i++)
        light.d[YSF_SYNC_SYMBOLS + 100 + 11 * i] ^= 0x1;
    const Seen l = decode(light);
    REQUIRE(l.ok);
    REQUIRE(l.call.dataBlocks == 2);
    REQUIRE(strcmp(l.call.source, "EA5TEST") == 0);
}

TEST_CASE("the voice slots are the 72 bits the transmitter fed", "[ysf][frame]")
{
    ysfFrame_reset();

    const Call call;
    uint8_t voice[YSF_VOICE_SLOTS][9];
    for (int i = 0; i < YSF_VOICE_SLOTS; i++)
        for (int b = 0; b < 9; b++)
            voice[i][b] = (uint8_t)((i * 53 + b * 29 + 0x80) & 0xFF);

    const Seen s = decode(voiceFrameMode1(1, 4, voice, call, 1));

    REQUIRE(s.ok);
    REQUIRE(s.call.fi == 1);
    REQUIRE(s.call.dt == 0);
    REQUIRE(s.call.fn == 1);
    REQUIRE(s.call.ft == 4);
    REQUIRE(s.voiceCount == YSF_VOICE_SLOTS);
    REQUIRE(s.call.voices == YSF_VOICE_SLOTS);

    for (int i = 0; i < YSF_VOICE_SLOTS; i++) {
        INFO("slot " << i);
        REQUIRE(s.voice[i].nbits == YSF_VOICE_MAX_BITS);
        REQUIRE(slotBitsEqual(s.voice[i], voice[i]));
    }

    /* bn = 1 carries the nodes of the call, and the receiver reads uplink
     * first -- the opposite order of the block in a header frame. */
    REQUIRE(strcmp(s.call.uplink, "EA5RPT") == 0);
    REQUIRE(strcmp(s.call.downlink, "EA5RPT") == 0);
}

TEST_CASE("full-rate voice is counted as unsupported, not decoded",
          "[ysf][frame]")
{
    /*
     * Data type 3 is 144 bits of full-rate IMBE per slot.  This build has no
     * IMBE path for it, so the five slots must be counted as unsupported and
     * nothing must reach the vocoder: handing AMBE+2 the wrong codec's bits
     * produces noise on the speaker for the whole transmission.
     */
    ysfFrame_reset();

    const Seen s = decode(voiceFrFrame(0, 4, Call()));

    REQUIRE(s.ok);
    REQUIRE(s.call.dt == 3);
    REQUIRE(s.voiceCount == 0);
    REQUIRE(s.call.unsupported == YSF_VOICE_SLOTS);
    REQUIRE(s.call.voices == 0);
}

TEST_CASE("a V/D mode 2 slot whose fixed bit is set is dropped", "[ysf][frame]")
{
    /*
     * V/D mode 2 sends 49 parameter bits as 27 triples and 22 single bits and
     * closes the 104-bit block with a bit that must be zero: a set fixed bit is
     * how a slot says it is corrupt.  The slot must then be dropped from
     * voiceCount, while a good one is handed on with the parameters recovered
     * by majority over the triples.
     */
    const Call call;
    uint8_t params[YSF_VOICE_SLOTS][49];
    for (int i = 0; i < YSF_VOICE_SLOTS; i++)
        for (int b = 0; b < 49; b++)
            params[i][b] = (uint8_t)(((i * 7 + b * 3) >> 1) & 1u);

    ysfFrame_reset();
    const Seen good = decode(voiceFrameMode2(0, 4, params, call, false));

    REQUIRE(good.ok);
    REQUIRE(good.call.dt == 2);
    REQUIRE(good.voiceCount == YSF_VOICE_SLOTS);
    for (int i = 0; i < YSF_VOICE_SLOTS; i++) {
        INFO("slot " << i);
        REQUIRE(good.voice[i].nbits == 49);
        for (int b = 0; b < 49; b++)
            REQUIRE((uint8_t)(good.voice[i].bits[b] & 1u) == params[i][b]);
    }

    ysfFrame_reset();
    const Seen bad = decode(voiceFrameMode2(0, 4, params, call, true));

    REQUIRE(bad.ok);
    REQUIRE(bad.call.dt == 2);
    REQUIRE(bad.voiceCount == 0);
    REQUIRE(bad.call.voices == 0);
}

TEST_CASE("the DG-ID is published only when the FICH carries one",
          "[ysf][frame]")
{
    /*
     * The DG-ID is System Fusion's talkgroup: the squelch flag in the FICH is
     * inverted, so a clear flag means "a DG-ID follows" and the seven bits are
     * a number from 0 to 99.  With the flag set there is no DG-ID, and the
     * layer must say so rather than publish whatever the seven bits hold.
     */
    ysfFrame_reset();

    Call call;
    call.dgid = 42;
    Seen s = decode(callsignFrame(0, call));
    REQUIRE(s.ok);
    REQUIRE(s.call.dgidValid);
    REQUIRE(s.call.dgid == 42);

    ysfFrame_reset();
    call.dgid = -1;      /* no DG-ID: the flag is set, the field zero */
    s = decode(callsignFrame(0, call));
    REQUIRE(s.ok);
    REQUIRE_FALSE(s.call.dgidValid);

    ysfFrame_reset();
    call.dgid = 0;       /* 0 is "all", which is a valid DG-ID */
    s = decode(callsignFrame(0, call));
    REQUIRE(s.ok);
    REQUIRE(s.call.dgidValid);
    REQUIRE(s.call.dgid == 0);
}

