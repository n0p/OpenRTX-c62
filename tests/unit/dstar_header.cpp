/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dstar_header.cpp
 * @brief The D-STAR radio header: its FEC, its refusals, and what the mode
 * publishes once one is accepted.
 *
 * A D-STAR transmission is named by its header and by nothing else: the
 * callsign that goes on the screen, the destination, the two repeaters, the
 * suffix and the flags all come from those 41 bytes, and everything after them
 * is 96-bit frames that carry no identity at all.  So the interesting cases are
 * not "a header decodes" (tests/host/dstar does that on a loopback and on real
 * captures) but:
 *
 *  - a header that arrives wrong must be refused, because a mis-corrected
 *    header is a call attributed to the wrong station;
 *  - a header the Viterbi cannot repair must not be published either, and the
 *    demodulator must go back to hunting instead of feeding 96-bit frames to a
 *    call that does not exist.
 *
 * The published state is checked here too: dstarStatus_header() is what the
 * screen reads, and its fields are the header's own.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "protocols/DSTAR/DStarDefs.h"
#include "protocols/DSTAR/DStarFec.h"
#include "protocols/DSTAR/DStarPhy.h"
#include "protocols/DSTAR/DStarStatus.h"
}

namespace {

/** The call the D-STAR fixtures use: two repeaters, a general call. */
struct dstarHeader testHeader()
{
    struct dstarHeader h;
    memset(&h, 0, sizeof(h));

    h.flags[0] = DSTAR_FLAG1_REPEATER;   /* voice, via a repeater */
    h.flags[1] = 0x00;
    h.flags[2] = 0x00;
    strcpy(h.rpt2, "ED1ZAR G");
    strcpy(h.rpt1, "ED1ZAR B");
    strcpy(h.ur, "CQCQCQ");
    strcpy(h.my, "EA5TEST");
    strcpy(h.sfx, "C62");

    return h;
}

/** A received frame as the demodulator hands it over. */
struct dstarPhyFrame frame(uint32_t index, bool sync, const uint8_t slow[3])
{
    struct dstarPhyFrame f;
    memset(&f, 0, sizeof(f));
    f.index = index;
    f.sync = sync;
    if (slow != nullptr)
        memcpy(f.slow, slow, DSTAR_SLOW_BYTES);
    return f;
}

} // namespace

TEST_CASE("the header's P_FCS is CRC-16/X.25", "[dstar][header]")
{
    /* The standard check value of the polynomial D-STAR uses (STD-7 4.1.1 k:
     * reflected 0x8408, init 0xFFFF, final inversion). */
    const char *check = "123456789";
    REQUIRE(dstarCrc16((const uint8_t *)check, 9) == 0x906E);
}

TEST_CASE("a packed header checks out and comes back field by field",
          "[dstar][header]")
{
    const struct dstarHeader h = testHeader();

    uint8_t raw[DSTAR_HEADER_BYTES];
    dstarHeaderPack(&h, raw);

    REQUIRE(dstarHeaderCheck(raw));

    struct dstarHeader back;
    dstarHeaderUnpack(raw, &back);

    REQUIRE(back.flags[0] == DSTAR_FLAG1_REPEATER);
    REQUIRE(back.flags[1] == 0x00);
    REQUIRE(back.flags[2] == 0x00);
    REQUIRE(strcmp(back.rpt2, "ED1ZAR G") == 0);
    REQUIRE(strcmp(back.rpt1, "ED1ZAR B") == 0);
    REQUIRE(strcmp(back.ur, "CQCQCQ") == 0);
    REQUIRE(strcmp(back.my, "EA5TEST") == 0);
    REQUIRE(strcmp(back.sfx, "C62") == 0);
}

TEST_CASE("a header is upper-cased and padded to its field width",
          "[dstar][header]")
{
    /* What a radio sends: the fields are fixed width and the case is the one
     * the air interface uses, so a channel or CPS that hands over lower case
     * or a short callsign still produces the bytes a receiver expects. */
    struct dstarHeader h;
    memset(&h, 0, sizeof(h));
    strcpy(h.my, "ea5test");
    strcpy(h.sfx, "d");
    strcpy(h.ur, "cqcqcq");

    uint8_t raw[DSTAR_HEADER_BYTES];
    dstarHeaderPack(&h, raw);

    REQUIRE(memcmp(&raw[DSTAR_HDR_MY], "EA5TEST ", 8) == 0);
    REQUIRE(memcmp(&raw[DSTAR_HDR_SFX], "D   ", 4) == 0);
    REQUIRE(memcmp(&raw[DSTAR_HDR_UR], "CQCQCQ  ", 8) == 0);

    struct dstarHeader back;
    dstarHeaderUnpack(raw, &back);
    REQUIRE(strcmp(back.my, "EA5TEST") == 0);
    REQUIRE(strcmp(back.sfx, "D") == 0);
}

TEST_CASE("any single corrupted byte fails the P_FCS", "[dstar][header]")
{
    /* Every one of the 41 bytes, at a bit that is not in the CRC's own low
     * nibble: a header whose CRC still checks after a channel error is a call
     * with the wrong callsign on the screen. */
    const struct dstarHeader h = testHeader();
    uint8_t raw[DSTAR_HEADER_BYTES];
    dstarHeaderPack(&h, raw);

    for (int b = 0; b < DSTAR_HEADER_BYTES; b++) {
        uint8_t rx[DSTAR_HEADER_BYTES];
        memcpy(rx, raw, sizeof(rx));
        rx[b] ^= 0x10;
        INFO("byte " << b);
        REQUIRE_FALSE(dstarHeaderCheck(rx));
    }
}

TEST_CASE("the coded header survives its coding gain and is refused past it",
          "[dstar][header]")
{
    /*
     * 660 on-air bits carry the 328 information bits: rate 1/2 K=3, a 24-column
     * interleave and the PN scrambler.  Forty channel bit errors are inside
     * what the Viterbi repairs (it reports them), a hundred and twenty are not,
     * and the refusal is the CRC's -- the struct is filled either way, so the
     * caller has to look at the return value.
     */
    const struct dstarHeader h = testHeader();
    uint8_t raw[DSTAR_HEADER_BYTES];
    dstarHeaderPack(&h, raw);

    uint8_t bits[DSTAR_CODED_BITS];
    dstarHeaderEncode(raw, bits);

    int8_t soft[DSTAR_CODED_BITS];
    for (int i = 0; i < DSTAR_CODED_BITS; i++)
        soft[i] = (int8_t)(bits[i] ? 96 : -96);

    struct dstarHeader back;
    REQUIRE(dstarHeaderDecode(soft, &back));
    REQUIRE(strcmp(back.my, "EA5TEST") == 0);
    REQUIRE(strcmp(back.ur, "CQCQCQ") == 0);
    REQUIRE(strcmp(back.rpt1, "ED1ZAR B") == 0);
    REQUIRE(back.viterbi == 0);

    /* Forty channel bits inverted, spread by the interleaver */
    int8_t noisy[DSTAR_CODED_BITS];
    memcpy(noisy, soft, sizeof(noisy));
    for (int k = 0; k < 40; k++)
        noisy[(k * 37) % DSTAR_CODED_BITS] = (int8_t)-noisy[(k * 37) % DSTAR_CODED_BITS];

    REQUIRE(dstarHeaderDecode(noisy, &back));
    REQUIRE(strcmp(back.my, "EA5TEST") == 0);
    REQUIRE(strcmp(back.rpt2, "ED1ZAR G") == 0);
    REQUIRE(back.viterbi == 40);   /* every one of them, and it says so */

    /* A hundred and twenty: past the code, refused by the P_FCS */
    int8_t broken[DSTAR_CODED_BITS];
    memcpy(broken, soft, sizeof(broken));
    for (int k = 0; k < 120; k++)
        broken[(k * 37) % DSTAR_CODED_BITS] = (int8_t)-broken[(k * 37) % DSTAR_CODED_BITS];

    REQUIRE_FALSE(dstarHeaderDecode(broken, &back));
}

TEST_CASE("a header that was cut short cannot be decoded", "[dstar][header]")
{
    /*
     * The demodulator collects 660 soft bits before it tries the header.  If
     * the carrier dropped half way (or the timing block lost the signal) the
     * rest of the buffer is silence, i.e. soft zeros: the Viterbi has nothing
     * to weigh, and the only correct answer is "no header".  A receiver that
     * accepts one produces a call with a callsign made of question marks.
     */
    const struct dstarHeader h = testHeader();
    uint8_t raw[DSTAR_HEADER_BYTES];
    dstarHeaderPack(&h, raw);

    uint8_t bits[DSTAR_CODED_BITS];
    dstarHeaderEncode(raw, bits);

    for (int keep : {200, 300, 400, 500}) {
        int8_t soft[DSTAR_CODED_BITS];
        for (int i = 0; i < DSTAR_CODED_BITS; i++)
            soft[i] = (i < keep) ? (int8_t)(bits[i] ? 96 : -96) : (int8_t)0;

        struct dstarHeader back;
        INFO("kept " << keep << " of " << DSTAR_CODED_BITS);
        REQUIRE_FALSE(dstarHeaderDecode(soft, &back));
    }
}

TEST_CASE("a cleared D-STAR status has no call to show", "[dstar][status]")
{
    dstarStatus_clear();

    struct dstarStatus st;
    dstar_getStatus(&st);

    REQUIRE_FALSE(st.sync);
    REQUIRE_FALSE(st.voice);
    REQUIRE(st.my[0] == '\0');
    REQUIRE(st.ur[0] == '\0');
    REQUIRE(st.rpt1[0] == '\0');
    REQUIRE(st.rpt2[0] == '\0');
    REQUIRE(st.headers == 0);
    REQUIRE(st.frames == 0);
    REQUIRE(st.text[0] == '\0');
    REQUIRE(st.lat == 0);
    REQUIRE(st.gpsValid == false);

    REQUIRE(dstarHeard(0) == nullptr);
}

TEST_CASE("an accepted header publishes MY, UR, both repeaters and the flags",
          "[dstar][status]")
{
    dstarStatus_clear();

    struct dstarHeader h = testHeader();
    h.viterbi = 3;
    dstarStatus_header(&h, 1, 1000);

    struct dstarStatus st;
    dstar_getStatus(&st);

    REQUIRE(st.sync);
    REQUIRE(st.headers == 1);
    REQUIRE(strcmp(st.my, "EA5TEST") == 0);
    REQUIRE(strcmp(st.sfx, "C62") == 0);
    REQUIRE(strcmp(st.ur, "CQCQCQ") == 0);
    REQUIRE(strcmp(st.rpt1, "ED1ZAR B") == 0);
    REQUIRE(strcmp(st.rpt2, "ED1ZAR G") == 0);
    REQUIRE(st.flags[0] == DSTAR_FLAG1_REPEATER);
    REQUIRE(st.flags[1] == 0);
    REQUIRE(st.flags[2] == 0);
    REQUIRE(st.viterbi == 3);

    /* And the station is in the heard list the screen shows */
    const struct dstarHeardStation *heard = dstarHeard(0);
    REQUIRE(heard != nullptr);
    REQUIRE(strcmp(heard->call, "EA5TEST") == 0);
    REQUIRE(strcmp(heard->sfx, "C62") == 0);
    REQUIRE(strcmp(heard->rpt1, "ED1ZAR B") == 0);
    REQUIRE(strcmp(heard->ur, "CQCQCQ") == 0);
    REQUIRE(dstarHeard(1) == nullptr);
}

TEST_CASE("a second station becomes the newest call and keeps the first heard",
          "[dstar][status]")
{
    dstarStatus_clear();

    struct dstarHeader first = testHeader();
    dstarStatus_header(&first, 0, 1000);

    struct dstarHeader second = testHeader();
    strcpy(second.my, "EA3ABC");
    strcpy(second.sfx, "");
    strcpy(second.ur, "EA5TEST");
    dstarStatus_header(&second, 0, 2000);

    struct dstarStatus st;
    dstar_getStatus(&st);
    REQUIRE(st.headers == 2);
    REQUIRE(strcmp(st.my, "EA3ABC") == 0);
    REQUIRE(strcmp(st.ur, "EA5TEST") == 0);
    REQUIRE(st.sfx[0] == '\0');

    const struct dstarHeardStation *newest = dstarHeard(0);
    const struct dstarHeardStation *older = dstarHeard(1);
    REQUIRE(newest != nullptr);
    REQUIRE(older != nullptr);
    REQUIRE(strcmp(newest->call, "EA3ABC") == 0);
    REQUIRE(strcmp(older->call, "EA5TEST") == 0);
    REQUIRE(dstarHeard(2) == nullptr);
}

TEST_CASE("frames count voice, and a resync word counts as a resync",
          "[dstar][status]")
{
    /*
     * A frame with the resync word in its slow-data field carries voice and no
     * slow data, which the status has to account for separately: the resync
     * counter is what tells a receiver that its frame boundary is holding (the
     * host voice harness fails the call when it stops finding them).
     */
    dstarStatus_clear();

    const uint8_t slow[3] = {0x66, 0x66, 0x66};

    for (uint32_t i = 0; i < 3; i++) {
        const struct dstarPhyFrame f = frame(i, false, slow);
        dstarStatus_frame(&f, 5000);
    }
    const struct dstarPhyFrame r = frame(3, true, nullptr);
    dstarStatus_frame(&r, 5000);

    struct dstarStatus st;
    dstar_getStatus(&st);
    REQUIRE(st.frames == 4);
    REQUIRE(st.resyncs == 1);
    REQUIRE(st.voice);
    REQUIRE(st.sync);

    /* Voice falls off after a second without a frame, sync after two */
    dstarStatus_age(5000 + 900);
    dstar_getStatus(&st);
    REQUIRE(st.voice);
    REQUIRE(st.sync);

    dstarStatus_age(5000 + 1100);
    dstar_getStatus(&st);
    REQUIRE_FALSE(st.voice);
    REQUIRE(st.sync);

    dstarStatus_age(5000 + 2100);
    dstar_getStatus(&st);
    REQUIRE_FALSE(st.voice);
    REQUIRE_FALSE(st.sync);
}

TEST_CASE("the slow-data message becomes the status line", "[dstar][status]")
{
    /*
     * The 20-character message is four blocks of five characters, each block
     * spread over two frames (three slow bytes each).  It is the D-STAR
     * equivalent of the APRS comment, and it is what the screen shows under the
     * caller's callsign.
     */
    dstarStatus_clear();

    struct dstarHeader h = testHeader();
    dstarStatus_header(&h, 0, 1000);

    const char *msg = "HOLA DESDE EL C62  ";   /* 20 characters */
    uint32_t index = 0;

    for (int b = 0; b < 4; b++) {
        uint8_t block[DSTAR_SLOW_BLOCK];
        memset(block, DSTAR_SLOW_FILL, sizeof(block));
        block[0] = (uint8_t)(DSTAR_SLOW_TEXT | b);
        memcpy(&block[1], msg + 5 * b, 5);

        for (int half = 0; half < 2; half++) {
            const struct dstarPhyFrame f = frame(index++, false,
                                                &block[half * 3]);
            dstarStatus_frame(&f, 1000 + (uint32_t)index * 20);
        }
    }

    struct dstarStatus st;
    dstar_getStatus(&st);
    REQUIRE(strcmp(st.text, "HOLA DESDE EL C62") == 0);  /* trailing spaces cut */
    REQUIRE(st.texts >= 1);

    /* The heard entry carries the same call */
    const struct dstarHeardStation *heard = dstarHeard(0);
    REQUIRE(heard != nullptr);
    REQUIRE(strcmp(heard->call, "EA5TEST") == 0);
}
