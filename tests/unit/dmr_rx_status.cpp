/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The DMR receiver, fed synthetic baseband: a stand-in for its receiver.
 *
 * docs/TEST_PLAN.md names the hole: no digital mode had anything that fed its
 * receive path with samples and checked the state it publishes.  This is the
 * DMR one.  It builds a repeater's 30 ms slots with the firmware's own
 * modulator (protocols/DMR/dmr_tx.c, the same one tests/host/dmr_tx_host
 * drives), hands them to the demodulator in the block size OpMode_DMR::feed()
 * uses, and asserts on what dmr_getStatus() says afterwards - the struct the
 * main screen paints:
 *
 *   sync, colour code, timeslot (which is the CACH TACT's answer), the call's
 *   source and destination and whether it is a group call, and the link
 *   quality figure.
 *
 * And on the refusals, which are the half that never shows on a test bench:
 * nothing is published from silence or noise, a link control that fails its
 * checksum announces no source or destination, and a colour code that only
 * decoded with an error correction does not overwrite the one on the air
 * (the "colour code dancing while the audio is fine" defect).
 *
 * What a repeater does that the mode's transmitter does not is only the
 * wrapping: the 24-bit CACH before each burst and the BS-sourced sync word in
 * the middle of it.  Both are built here, from the standard's constants, on
 * top of the package's own burst builders; the payload (BPTC, RS(12,9), the
 * embedded LC, the AMBE+2 interleave) is the transmitter's.
 *
 * Not covered, and why: the mode's own update() (it needs the Zephyr kernel,
 * the audio streams and the radio), the vocoder's output samples (unit/imbe_*
 * and the dmr_rx host harness cover them), the 7 KiB voice stack (a compile
 * time assertion in DmrSpeech.c) and the BK4819's filters (a register the
 * host has no driver for: see tests/unit/bk4819_bypass.cpp for what is left).
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrStatus.h"
#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_fec.h"
#include "protocols/DMR/dmr_glue.h"
#include "protocols/DMR/dmr_proto.h"
#include "protocols/DMR/dmr_sms.h"
#include "protocols/DMR/dmr_tx.h"
#include "protocols/DMR/dmr_voice.h"
}

namespace {

/* One 30 ms slot at 48 kHz, ten samples per symbol (OpMode_DMR) */
constexpr int kHalf = 1440;
constexpr int kSps = DMR_TX_SPS;
constexpr int kSlotSyms = kHalf / kSps; /* 144 */
constexpr int kCachSyms = 12;           /* 2.5 ms of CACH before the burst */
constexpr int kBurstSyms = DMR_TX_BURST_DIBITS; /* 132 */
constexpr int kSyncAt = 54; /* first sync dibit inside the burst */

/* Sync words of a repeater (ETSI TS 102 361-1 9.1.1), as dmr_demod.c has them */
constexpr uint64_t kSyncBsVoice = 0x755FD7DF75F7ull;
constexpr uint64_t kSyncBsData = 0xDFF57D75DF5Dull;

/*
 * Where the seven TACT bits sit in the 24 of the CACH (dmr_proto.c,
 * k_tact_pos): one Hamming(7,4) codeword, interleaved.  The rest of the CACH
 * carries the colour code and spare bits, which this receiver does not read
 * (it takes the colour code from the slot type and the embedded LC).
 */
constexpr int kTactPos[7] = {0, 4, 8, 12, 14, 18, 22};

/* The call every test transmits unless it says otherwise */
constexpr uint32_t kSrc = 1234567u;
constexpr uint32_t kTg = 9u;
constexpr uint8_t kCc = 1u;

/* The mode's transmit level, so the decoder sees the deviation it expects */
constexpr float kTxLevel = 23000.0f * 0.3165f * 1944.0f / 2400.0f;

/* The package stamps its entries with the firmware's tick */
long long tickMs;

struct Stream
{
    std::vector<int16_t> pcm;  /* the baseband, as the radio hands it over */
    std::vector<int> tsOfSlot; /* 1 or 2: which slot that 30 ms carried */
    std::vector<const char *> whatOfSlot; /* what was in it, for the failures */
};

struct Gen
{
    dmr_tx_t tx {};
    dmr_mod_t mod {};
    Stream s;
};

void putSync(uint8_t burst[kBurstSyms], uint64_t sync)
{
    for (int i = 0; i < 24; i++)
        burst[kSyncAt + i] = static_cast<uint8_t>((sync >> (46 - 2 * i)) & 3u);
}

/*
 * The CACH of a repeater burst: its TACT says which timeslot this burst is
 * (TC) and whether it comes from a repeater or a mobile (AT = 0 here).
 */
void putCach(uint8_t cach[kCachSyms], int ts, int accessType)
{
    uint8_t bits[24] = {};
    /* AT (1) | TC (1) | LCSS (2), the order dmr_proto.c decodes it in */
    const uint32_t word = (static_cast<uint32_t>(accessType) << 3)
                        | (static_cast<uint32_t>(ts == 2 ? 1 : 0) << 2);
    const uint32_t cw = dmr_hamming_7_4_encode(word);

    for (int i = 0; i < 7; i++)
        bits[kTactPos[i]] = static_cast<uint8_t>((cw >> (6 - i)) & 1u);
    for (int i = 0; i < kCachSyms; i++)
        cach[i] = static_cast<uint8_t>((bits[2 * i] << 1) | bits[2 * i + 1]);
}

/*
 * Append one 30 ms slot: the CACH, then the burst, or silence when there is
 * none.  A repeater sends the CACH before every burst, including the ones of
 * the other timeslot.
 */
void appendSlot(Gen &g, const uint8_t burst[kBurstSyms], bool carry, int ts,
                const char *what)
{
    uint8_t syms[kSlotSyms];
    int16_t pcm[kHalf];

    if (carry) {
        putCach(syms, ts, 0);
        memcpy(&syms[kCachSyms], burst, kBurstSyms);
    } else {
        memset(syms, 0, sizeof(syms));
    }
    dmr_mod_dibits(&g.mod, syms, kSlotSyms, pcm);
    g.s.pcm.insert(g.s.pcm.end(), pcm, pcm + kHalf);
    g.s.tsOfSlot.push_back(ts);
    g.s.whatOfSlot.push_back(what);
}

/* Three AMBE+2 frames of 72 bits, deterministic: the payload does not matter */
void ambeFrames(unsigned n, uint8_t ambe[3][9])
{
    for (int f = 0; f < 3; f++)
        for (int i = 0; i < 9; i++)
            ambe[f][i] = static_cast<uint8_t>((n * 37 + f * 11 + i * 7) & 0xFF);
}

/* An idle burst of a repeater: a slot type and no payload worth reading */
void idleBurst(uint8_t burst[kBurstSyms], uint8_t cc)
{
    const uint8_t bytes[12] = {};

    dmr_tx_block(cc, DMR_DT_IDLE, bytes, burst);
    putSync(burst, kSyncBsData);
}

/* What the call being transmitted is, and what is wrong with it on purpose */
struct CallOpts
{
    int callTs = 1;      /* timeslot the repeater repeats the call in */
    int frames = 40;     /* 60 ms TDMA frames */
    bool terminator = false;
    bool voice = true;   /* voice superframes after the two headers */
    /* The header's Reed-Solomon parity is wrong: the link control in it is
     * not announced, though the rest of the burst (slot type, sync) is intact */
    bool headerBroken = false;
    /* Frame whose burst C carries a colour code that only decodes with one
     * bit of error correction (-1: none) */
    int miscorrectFrame = -1;
    /* ... and the same with a clean field (a different colour code on the
     * air, correctly received) */
    int differentCcFrame = -1;
    /* Break the embedded LC word of one voice burst every N frames, from the
     * eighth on: the quality figure has to notice (0: none) */
    int breakEmbEvery = 0;
};

/*
 * A 16-bit embedded LC word the code cannot correct, found by asking its own
 * decoder: the test says "this word does not decode" without depending on how
 * many bit errors that takes.
 */
uint16_t embPoison()
{
    static uint16_t poison = 0;

    if (poison == 0) {
        for (uint32_t m = 1; m < 0x10000; m++) {
            uint32_t cw = m;

            if (dmr_qr_16_7_correct(&cw) < 0) {
                poison = static_cast<uint16_t>(m);
                break;
            }
        }
    }
    return poison;
}

/* Write a 16-bit word into the two halves of the burst's EMB field */
void putEmb(uint8_t burst[kBurstSyms], uint16_t word)
{
    for (int i = 0; i < 16; i++) {
        const int b = (i < 8) ? 108 + i : 148 + (i - 8);
        const uint8_t mask = static_cast<uint8_t>((b & 1) ? 0x1u : 0x2u);
        const int d = b / 2;

        if ((word >> (15 - i)) & 1u)
            burst[d] |= mask;
        else
            burst[d] = static_cast<uint8_t>(burst[d] & ~mask);
    }
}

/*
 * A voice LC header burst, built the way dmr_tx.c's dmr_tx_header() builds it:
 * the link control with its RS(12,9) parity, BPTC(196,96), the slot type and
 * the repeater's sync.  Built here because the transmitter has no way to emit
 * a deliberately broken one, and this test needs the payload wrong after its
 * checksum was made.
 */
void headerBurst(const dmr_tx_t &tx, bool breakLc, uint8_t burst[kBurstSyms])
{
    uint8_t bytes[12] = {};
    uint8_t info[96], coded[196], bits[264] = {}, st[20];

    for (int i = 0; i < 72; i++)
        bytes[i / 8] |= static_cast<uint8_t>(tx.lc[i] << (7 - i % 8));
    dmr_rs_12_9_parity(bytes, DMR_RS_MASK_VOICE_HEADER, &bytes[9]);

    if (breakLc) {
        /* Two octets of the link control wrong after its parity was made:
         * one the RS would correct, two it detects and the LC is dropped */
        bytes[3] ^= 0x01u; /* destination, low octet */
        bytes[8] ^= 0x80u; /* source, low octet */
    }
    for (int i = 0; i < 96; i++)
        info[i] = static_cast<uint8_t>((bytes[i / 8] >> (7 - i % 8)) & 1u);
    dmr_bptc196_encode(info, coded);

    memcpy(bits, coded, 98);
    memcpy(&bits[166], &coded[98], 98);
    dmr_u32_to_bits(dmr_golay_20_8_encode((static_cast<uint32_t>(tx.cc) << 4)
                                         | DMR_DT_VOICE_LC),
                    20, st);
    memcpy(&bits[98], st, 10);
    memcpy(&bits[156], &st[10], 10);
    for (int i = 0; i < kBurstSyms; i++)
        burst[i] = static_cast<uint8_t>((bits[2 * i] << 1) | bits[2 * i + 1]);
    putSync(burst, kSyncBsData);
}

/*
 * A whole call on a repeater: a voice LC header twice, then voice superframes
 * (A..F, A carrying the sync, B..E the embedded LC), and a terminator if
 * asked.  The other timeslot carries idle bursts, as a busy repeater's does.
 */
void buildCall(Gen &g, const CallOpts &o)
{
    uint8_t burst[kBurstSyms];
    unsigned voice = 0;

    dmr_tx_setup(&g.tx, kSrc, kTg, true, kCc);
    dmr_mod_init(&g.mod, kTxLevel, true);

    for (int f = 0; f < o.frames; f++) {
        for (int ts = 1; ts <= 2; ts++) {
            if (ts != o.callTs) {
                idleBurst(burst, kCc);
                appendSlot(g, burst, true, ts, "idle");
                continue;
            }
            if (f < 2) {
                headerBurst(g.tx, o.headerBroken, burst);
                appendSlot(g, burst, true, ts,
                           o.headerBroken ? "header, checksum broken"
                                          : "header");
                continue;
            }
            if (o.terminator && f == o.frames - 1) {
                dmr_tx_terminator(&g.tx, burst);
                putSync(burst, kSyncBsData);
                appendSlot(g, burst, true, ts, "terminator");
                continue;
            }
            if (!o.voice) {
                /* The headers and nothing else: no embedded LC to fall back on */
                idleBurst(burst, kCc);
                appendSlot(g, burst, true, ts, "idle");
                continue;
            }

            uint8_t ambe[3][9];

            ambeFrames(voice++, ambe);
            /* Burst A of the superframe carries the sync; B..F carry the
             * embedded LC in its place, so the sync must go in only there */
            const bool burstA = (g.tx.voiceIdx == 0);
            const bool otherCc =
                (f == o.miscorrectFrame || f == o.differentCcFrame);

            /*
             * A burst of another colour code: the colour code of a voice
             * burst lives in the embedded LC word, which dmr_tx_voice() fills
             * from the call's, so the call's is moved for this one burst.
             */
            const uint8_t cc = g.tx.cc;

            if (otherCc)
                g.tx.cc = static_cast<uint8_t>(kCc + 4);
            dmr_tx_voice(&g.tx, ambe, burst);
            g.tx.cc = cc;
            if (burstA)
                putSync(burst, kSyncBsVoice);
            /* One bit of the embedded LC word wrong: the FEC corrects it, so
             * the word still decodes - onto another colour code.  Only on a
             * burst that carries an EMB: burst A has the sync there. */
            if (f == o.miscorrectFrame && !burstA)
                burst[kSyncAt] ^= 1u;
            /* The embedded LC word replaced by one its code cannot correct:
             * the word does not decode at all */
            if (!burstA && o.breakEmbEvery > 0 && f >= 8
                && (f % o.breakEmbEvery) == 0)
                putEmb(burst, embPoison());
            appendSlot(g, burst, true, ts, "voice");
        }
    }
}

/* What the mode would publish after each 30 ms block of samples */
struct Obs
{
    size_t block;
    struct dmrStatus st;
    bool locked;
};

/* The vocoder's output, as the speech thread would hand it to the codec */
unsigned long long pcmSamples;

void sink(const float *pcm, int n)
{
    (void)pcm;
    pcmSamples += static_cast<unsigned long long>(n);
}

/*
 * Feed the baseband the way OpMode_DMR::feed() does: 1440 samples at a time,
 * converted to float in chunks of 160, then dmr_demod_feed_disc(), the frames
 * the speech thread would decode (DmrSpeech.c does this in its own thread),
 * and the snapshot refresh the UI reads.
 */
std::vector<Obs> feed(const Stream &s)
{
    std::vector<Obs> obs;
    float chunk[160];

    for (size_t off = 0; off + kHalf <= s.pcm.size(); off += kHalf) {
        for (int i = 0; i < kHalf; i += 160) {
            const int n = (kHalf - i < 160) ? kHalf - i : 160;

            for (int k = 0; k < n; k++)
                chunk[k] = static_cast<float>(s.pcm[off + i + k]);
            dmr_demod_feed_disc(chunk, n);
        }
        tickMs += 30; /* one 30 ms block of stream time */
        while (dmr_voice_decode_next())
            ;
        dmrStatus_update();

        Obs o { off / kHalf, {}, false };
        dmr_demod_status_t ds;

        dmr_getStatus(&o.st);
        dmr_demod_get_status(&ds);
        o.locked = ds.locked;
        obs.push_back(o);
    }
    return obs;
}

/* Fresh receiver, as enable() + a new channel leaves it */
void resetReceiver()
{
    tickMs = 0;
    pcmSamples = 0;
    dmr_demod_init();
    dmr_demod_reset();
    dmr_proto_reset();
    dmr_voice_reset();
    dmr_voice_set_sink(sink);
    dmrStatus_clear();
}

/* The snapshots of the last `n` blocks: what the screen shows mid-call */
std::vector<Obs> tail(const std::vector<Obs> &obs, size_t n)
{
    if (obs.size() <= n)
        return obs;
    return std::vector<Obs>(obs.end() - static_cast<long>(n), obs.end());
}

} // namespace

/*
 * sysTime, for the package's own deadlines (dmr_track, the voice timeouts).
 * The harness runs two seconds of audio in a few milliseconds, so the clock
 * is the *stream's* time, not the wall's.
 */
extern "C" long long getTick(void)
{
    return tickMs;
}

/*
 * The call as a repeater repeats it: colour code 1, TS1, source 1234567 to
 * talkgroup 9, and a clean slot.
 */
TEST_CASE("A DMR repeater call publishes CC, timeslot, call and quality",
          "[dmr][rx]")
{
    resetReceiver();

    Gen g;
    CallOpts o;

    o.frames = 40;
    buildCall(g, o);

    const std::vector<Obs> obs = feed(g.s);
    const std::vector<Obs> st = tail(obs, 40); /* 1.2 s of steady state */

    int voiceSlots = 0, idleSlots = 0;

    for (const Obs &o : st) {
        REQUIRE(o.locked);

        /* Something is on the air, and the colour code does not dance */
        REQUIRE(o.st.sync);
        REQUIRE(o.st.colourCode == kCc);

        if (o.st.timeslot == 1) {
            voiceSlots++;
            /* The call's link control: who is calling whom */
            REQUIRE(o.st.voice);
            REQUIRE(o.st.idsValid);
            REQUIRE(o.st.group);
            REQUIRE(o.st.src == kSrc);
            REQUIRE(o.st.dst == kTg);
        } else if (o.st.timeslot == 2) {
            idleSlots++;
            /*
             * The repeater's other slot is data, not this call's voice, and
             * it carries no link control of its own: the call announced on
             * TS1 is not read from TS2's idle bursts.
             */
            REQUIRE_FALSE(o.st.voice);
            REQUIRE_FALSE(o.st.idsValid);
        } else {
            FAIL("timeslot unknown after four superframes on a repeater");
        }
    }
    /* Both slots were seen, so the CACH phase was established and followed */
    REQUIRE(voiceSlots >= 15);
    REQUIRE(idleSlots >= 15);

    /* Link quality: every embedded LC of the call decoded */
    REQUIRE(st.back().st.evm == 1000);
    /* Bursts, voice bursts and the AMBE+2 frames handed to the vocoder */
    REQUIRE(st.back().st.bursts > 0);
    REQUIRE(st.back().st.voiceBursts > 0);
    /* AMBE+2 frames that reached the vocoder, and the audio it produced */
    REQUIRE(st.back().st.frames > 0);
    REQUIRE(pcmSamples > 0);

    dmr_demod_status_t ds;

    dmr_demod_get_status(&ds);
    REQUIRE(ds.bs);
    REQUIRE_FALSE(ds.ms);
    REQUIRE(ds.level > 1000.0f); /* an outer symbol, in discriminator units */
}

/*
 * The same call in the other timeslot: the CACH's TACT, not the order of the
 * bursts, is what says which slot is being heard.
 */
TEST_CASE("A call in timeslot 2 is reported as timeslot 2", "[dmr][rx]")
{
    resetReceiver();

    Gen g;
    CallOpts o;

    o.callTs = 2;
    o.frames = 40;
    buildCall(g, o);

    const std::vector<Obs> st = tail(feed(g.s), 40);
    int ts2 = 0, ts1 = 0;

    for (const Obs &o : st) {
        if (o.st.timeslot == 2) {
            ts2++;
            REQUIRE(o.st.voice);
            REQUIRE(o.st.idsValid);
            REQUIRE(o.st.src == kSrc);
            REQUIRE(o.st.dst == kTg);
        } else if (o.st.timeslot == 1) {
            ts1++;
            REQUIRE_FALSE(o.st.voice);
        }
    }
    REQUIRE(ts2 >= 15);
    REQUIRE(ts1 >= 15);
}

/* Nothing on the air is nothing on the screen: no sync, no colour code, no call */
TEST_CASE("Silence and noise lock nothing and publish nothing", "[dmr][rx]")
{
    resetReceiver();

    Stream s;
    uint32_t lfsr = 0x1234567u;

    for (int i = 0; i < kHalf * 150; i++) {
        int16_t v = 0;

        if (i >= kHalf * 50) { /* then a noise floor 20 dB over the slicer's */
            lfsr = lfsr * 1103515245u + 12345u;
            v = static_cast<int16_t>(((lfsr >> 16) & 0x7FFFu) - 0x4000);
        }
        s.pcm.push_back(v);
        if (i % kHalf == 0) {
            s.tsOfSlot.push_back(0);
            s.whatOfSlot.push_back("silence");
        }
    }

    const std::vector<Obs> obs = feed(s);

    for (const Obs &o : obs) {
        REQUIRE_FALSE(o.locked);
        REQUIRE_FALSE(o.st.sync);
        REQUIRE(o.st.colourCode == -1);
        REQUIRE(o.st.timeslot == 0);
        REQUIRE_FALSE(o.st.idsValid);
        REQUIRE(o.st.src == 0);
        REQUIRE(o.st.dst == 0);
    }
}

/*
 * A link control that fails its checksum is not a call: the burst is there
 * (its slot type decoded), but who is calling whom is not published.  The
 * same stream with the header intact does announce it, so the test is about
 * the checksum and not about the stream.
 */
TEST_CASE("A DMR link control that fails its checksum announces no call",
          "[dmr][rx]")
{
    for (int broken = 1; broken >= 0; broken--) {
        resetReceiver();

        Gen g;
        CallOpts o;

        o.frames = 20;
        o.voice = false;         /* no embedded LC to fall back on */
        o.headerBroken = (broken != 0);
        buildCall(g, o);

        const std::vector<Obs> st = tail(feed(g.s), 20);
        int callSlots = 0, announced = 0;

        for (const Obs &obs : st) {
            /* Whatever the header carried, the burst itself was heard */
            REQUIRE(obs.st.sync);
            REQUIRE(obs.st.colourCode == kCc);
            if (obs.st.timeslot != 1)
                continue;
            callSlots++;
            if (obs.st.idsValid) {
                announced++;
                REQUIRE(obs.st.src == kSrc);
                REQUIRE(obs.st.dst == kTg);
                REQUIRE(obs.st.group);
            } else {
                /* Nothing decoded: neither half of an identifier is made up */
                REQUIRE(obs.st.src == 0);
                REQUIRE(obs.st.dst == 0);
            }
        }
        REQUIRE(callSlots >= 8);

        if (broken)
            REQUIRE(announced == 0);
        else
            REQUIRE(announced > 0);
    }
}

/*
 * The colour code comes from a field protected by a code that always lands on
 * some word, so a burst with an error in it can decode onto a *different*
 * colour code.  The status must not follow it: the colour code that only came
 * out of the correction is not published, and the screen keeps the one that
 * was heard.  A clean burst announcing another colour code is published as it
 * is - this receiver is a listener, not a filter.
 */
TEST_CASE("A miscorrected colour code does not overwrite the one on the air",
          "[dmr][rx]")
{
    int highest = -1;

    for (int corrupted = 1; corrupted >= 0; corrupted--) {
        resetReceiver();

        Gen g;
        CallOpts o;

        o.frames = 40;
        /* Frame 22 is burst C of a superframe: one that carries the EMB */
        if (corrupted)
            o.miscorrectFrame = 22;
        else
            o.differentCcFrame = 22;
        buildCall(g, o);

        int top = -1;

        for (const Obs &obs : feed(g.s))
            if (obs.st.sync && obs.st.colourCode > top)
                top = obs.st.colourCode;

        REQUIRE(top > 0);
        if (corrupted)
            REQUIRE(top == kCc);          /* the dancing colour code */
        else
            REQUIRE(top == kCc + 4);      /* what the air said, cleanly */
        highest = top;
    }
    REQUIRE(highest == kCc + 4);
}

/*
 * Packet data: a text message (the same builder the SMS tests use) put on the
 * air as a repeater's bursts, with one byte of its last block wrong.  The
 * status carries the two figures the stats page shows - the data blocks that
 * had a CRC and those that passed it - so a failed CRC-32 is visible there,
 * and a passed one cannot be counted as failed.
 */
TEST_CASE("A DMR packet whose CRC fails is not counted as a good one",
          "[dmr][rx]")
{
    for (int broken = 1; broken >= 0; broken--) {
        resetReceiver();

        constexpr int kMaxBursts = 32;
        uint8_t dt[kMaxBursts];
        uint8_t bytes[kMaxBursts][12];
        const int n = dmrSms_build(kTg, true, DMR_SMS_ETSI_IP, "hola", kSrc,
                                   dt, bytes, kMaxBursts);

        REQUIRE(n > 2);
        if (broken) {
            /* A payload byte of the last block: BPTC still decodes it, so
             * what fails is the packet's CRC-32 and nothing else */
            bytes[n - 1][6] ^= 0x5Au;
        }

        Gen g;
        uint8_t burst[kBurstSyms];

        dmr_mod_init(&g.mod, kTxLevel, true);
        for (int i = 0; i < n; i++) {
            for (int ts = 1; ts <= 2; ts++) {
                if (ts != 1) {
                    idleBurst(burst, kCc);
                    appendSlot(g, burst, true, ts, "idle");
                    continue;
                }
                dmr_tx_block(kCc, dt[i], bytes[i], burst);
                putSync(burst, kSyncBsData);
                appendSlot(g, burst, true, ts, "data");
            }
        }

        const struct dmrStatus st = feed(g.s).back().st;

        REQUIRE(st.crcChecks > 0);
        if (broken)
            REQUIRE(st.crcOk == 0);
        else
            REQUIRE(st.crcOk == st.crcChecks);
    }
}

/*
 * The quality figure is the share of the embedded link controls that decoded,
 * per mille.  A clean call is 1000, and a call whose embedded words are broken
 * has to be below it: a figure that always reads 1000 is not a measurement.
 * What must not happen either is the broken words taking the call with them -
 * the CC stays, the call stays, the audio stays.
 */
TEST_CASE("DMR link quality falls when the embedded link controls fail",
          "[dmr][rx]")
{
    unsigned evmClean = 0, evmBroken = 0;

    for (int broken = 0; broken <= 1; broken++) {
        resetReceiver();

        Gen g;
        CallOpts o;

        o.frames = 40;
        if (broken)
            o.breakEmbEvery = 5;
        buildCall(g, o);

        const std::vector<Obs> st = tail(feed(g.s), 36);
        const unsigned evm = st.back().st.evm;

        for (const Obs &obs : st) {
            /* Neither the call nor its colour code goes with the bad words */
            REQUIRE(obs.st.sync);
            REQUIRE(obs.st.colourCode == kCc);
            if (obs.st.timeslot != 1)
                continue;
            REQUIRE(obs.st.idsValid);
            REQUIRE(obs.st.src == kSrc);
            REQUIRE(obs.st.dst == kTg);
        }
        if (broken) {
            evmBroken = evm;
            REQUIRE(evm > 0);
            REQUIRE(evm < 1000);
        } else {
            evmClean = evm;
            REQUIRE(evm == 1000);
        }
    }
    REQUIRE(evmClean > evmBroken);
}
