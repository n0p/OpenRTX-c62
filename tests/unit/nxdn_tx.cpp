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
 *
 * The levels are the two the C62 can be set to: 3189 is the modulator gain
 * for +-1050 Hz of outer deviation (a 12.5 kHz channel, what the NXDN mode
 * uses) and 1063 the one for +-350 Hz (a 6.25 kHz channel).  They are the
 * same number the DMR chain uses for +-1944 Hz, scaled: see NXDN_TX_LEVEL in
 * OpMode_NXDN.cpp and docs/TX_NXDN.md.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/Fsk4Tx.h"
#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnFec.h"
#include "protocols/NXDN/NxdnPhy.h"
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
