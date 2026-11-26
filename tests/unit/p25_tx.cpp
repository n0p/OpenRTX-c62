/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p25_tx.cpp
 * @brief The P25 transmit chain, closed through the mode's own demodulator.
 *
 * tests/host/p25_tx does the same thing with a BER sweep and a voice source;
 * this case keeps the part that has to hold in the suite on every build: a
 * call the transmitter keys is a call a P25 receiver can read.  What it pins is
 * what the transmitter decides on its own:
 *
 *  - a call without a voice source is HDU + TSBK + TDU.  This build has no
 *    IMBE encoder, and the honest transmission is one that announces itself,
 *    carries its signalling and stops -- not one that puts noise in a voice
 *    frame and lets the receiver play it (docs/TX_P25.md);
 *  - the NAC is the twelve bits of the channel.  Anything wider would run into
 *    the DUID beside it in the NID and make every frame of the call unreadable,
 *    so a configuration with a wider NAC has to be masked, not truncated by
 *    accident;
 *  - the idle TSBK is a null-information block with a valid CRC-16, which is
 *    what the receiver's acceptance rule (Viterbi and CRC) checks.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/C4FM/C4fmMod.h"
#include "protocols/P25/P25Defs.h"
#include "protocols/P25/P25Phy.h"
#include "protocols/P25/P25Tx.h"
}

namespace {

/** Symbol level to deviation, as the mode keys the exciter. */
constexpr float GAIN = 5460.0f;

/** One frame of the call, as it arrived. */
struct Arrived {
    int duid;
    int nac;
    int span;
    int syncErrors;
    bool hdu;
    int hduAlgid;
    int hduGroup;
    bool tsbkValid;
    uint8_t tsbk[P25_TSBK_BYTES];
    int voiceCount;
};

struct Seen {
    std::vector<Arrived> frames;
};

void onFrame(const struct p25Frame *f, void *user)
{
    auto *s = static_cast<Seen *>(user);

    Arrived a;
    memset(&a, 0, sizeof(a));
    a.duid = (int)f->duid;
    a.nac = f->nac;
    a.span = f->spanSymbols;
    a.syncErrors = f->syncErrors;
    a.hdu = f->hdu;
    a.hduAlgid = f->hduAlgid;
    a.hduGroup = f->hduGroup;
    a.tsbkValid = f->tsbkValid;
    memcpy(a.tsbk, f->tsbk, sizeof(a.tsbk));
    a.voiceCount = f->voiceCount;

    s->frames.push_back(a);
}

/**
 * Key a call of @p signalling frames (no voice source) and demodulate it.
 *
 * @param nac: the NAC the transmitter is asked for.
 * @param signalling: TSBK frames between the header and the terminator.
 */
Seen transmit(uint16_t nac, int signalling)
{
    struct p25TxConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.nac = nac;
    cfg.group = 7;
    cfg.source = 0x123456;
    cfg.service = 0x00;

    p25Tx_setVoiceSource(nullptr, nullptr);
    p25Tx_begin(&cfg);

    const int frames = signalling + 2;   /* header and terminator */
    const size_t lead = 2400;
    const size_t data = (size_t)frames * P25_FRAME_SYMBOLS * C4FM_MOD_SPS;
    const size_t lastFrame = (size_t)P25_FRAME_SYMBOLS * C4FM_MOD_SPS;
    const size_t tail = 2 * lastFrame;

    std::vector<int16_t> pcm(lead + data + tail, 0);

    /* Everything but the last frame, then the request to end: the terminator
     * is built when its first symbol is asked for. */
    p25Tx_samples(&pcm[lead], (int)(data - lastFrame), GAIN);
    p25Tx_end();
    p25Tx_samples(&pcm[lead + data - lastFrame], (int)lastFrame, GAIN);

    Seen s;
    p25Phy_init();
    for (size_t off = 0; off < pcm.size(); off += 960) {
        const size_t n = (pcm.size() - off < 960) ? pcm.size() - off : 960;
        p25Phy_push(&pcm[off], n, onFrame, &s);
    }

    REQUIRE(p25Tx_frames() == (uint32_t)frames);

    return s;
}

} // namespace

TEST_CASE("a keyed P25 call is a header, its signalling and a terminator",
          "[p25][tx]")
{
    const int signalling = 6;
    const Seen s = transmit(0x293, signalling);

    /* Every frame of the call, in the order the transmitter sent them */
    REQUIRE(s.frames.size() >= (size_t)(signalling + 2));

    const Arrived &hdu = s.frames.front();
    REQUIRE(hdu.duid == (int)P25_DUID_HDU);
    REQUIRE(hdu.nac == 0x293);
    REQUIRE(hdu.hdu);
    REQUIRE(hdu.hduAlgid == 0x80);   /* clear, as a call with no voice is */
    REQUIRE(hdu.hduGroup == 7);
    REQUIRE(hdu.syncErrors == 0);

    /* The signalling blocks: a null-information TSBK with a valid CRC-16 */
    static const uint8_t nullBlock[P25_TSBK_BYTES] = {0, 0, 0, 0, 0, 0,
                                                      0, 0, 0, 0, 0xFF, 0xFF};
    int tsbks = 0;
    for (const Arrived &a : s.frames) {
        if (a.duid != (int)P25_DUID_TSBK)
            continue;

        tsbks++;
        INFO("TSBK " << tsbks);
        REQUIRE(a.nac == 0x293);
        REQUIRE(a.span == p25_frame_symbols(P25_DUID_TSBK));
        REQUIRE(a.tsbkValid);
        REQUIRE(memcmp(a.tsbk, nullBlock, sizeof(nullBlock)) == 0);
    }
    REQUIRE(tsbks >= signalling);

    /* And the terminator closes it */
    REQUIRE(s.frames.back().duid == (int)P25_DUID_TDU);
}

TEST_CASE("a call with no voice source carries no voice", "[p25][tx]")
{
    /*
     * The important half of the case above: this build has no IMBE encoder, so
     * the transmitter must not send LDU1/LDU2 at all.  A voice frame built from
     * silence or from random parameters would be a call the receiver plays as
     * noise, and the operator would have no way to tell the radio was lying.
     */
    const Seen s = transmit(0x293, 4);

    for (const Arrived &a : s.frames) {
        INFO("duid " << a.duid);
        REQUIRE(a.duid != (int)P25_DUID_LDU1);
        REQUIRE(a.duid != (int)P25_DUID_LDU2);
        REQUIRE(a.voiceCount == 0);
    }
}

TEST_CASE("the NAC of the transmission is the twelve bits of the channel",
          "[p25][tx]")
{
    /*
     * The NID carries the NAC in twelve bits next to the four of the DUID.  A
     * configuration with more than twelve bits set has to reach the air as the
     * low twelve, or the extra bits land on the DUID and every frame of the
     * call is decoded as another data unit -- or, with the BCH computing its
     * parity over them, not decoded at all.
     */
    const Seen s = transmit(0x1293, 4);

    REQUIRE_FALSE(s.frames.empty());
    for (const Arrived &a : s.frames) {
        INFO("duid " << a.duid << " nac " << a.nac);
        REQUIRE(a.nac == 0x293);
        REQUIRE((a.duid == (int)P25_DUID_HDU || a.duid == (int)P25_DUID_TSBK ||
                 a.duid == (int)P25_DUID_TDU));
    }
}
