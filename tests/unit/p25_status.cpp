/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p25_status.cpp
 * @brief What the P25 mode publishes to the UI, frame by frame.
 *
 * The screen reads this block (openrtx/src/ui/default/ui_mode_state.c): the
 * NAC and the DUID of the last frame, the talkgroup and the source of the call,
 * whether it is encrypted, and the counters behind the diagnostics page.  Two
 * things are easy to get wrong and invisible on the host today:
 *
 *  - the empty state.  Before a frame decodes, the identifiers are -1, not 0,
 *    which is what makes the screen read "SRCH" and "-" instead of a NAC 000
 *    and a talkgroup 0 that look like a real call (docs/TEST_PLAN.md §3).
 *  - the bad frame.  A frame whose link control the Reed-Solomon refused
 *    carries no talkgroup: publishing its zeroed field would move the call to
 *    "talkgroup 0, source 0" mid-transmission, and a TSBK whose CRC failed
 *    must not be counted as one that decoded.
 *
 * The status block is compiled into the firmware with CONFIG_P25 (the host
 * build carries the simulator stub instead), so this test builds its own copy
 * of P25Status.c with that switch on: it is the translation unit the radio
 * runs, not the stand-in the UI tests walk.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <thread>

extern "C" {
#include "protocols/P25/P25Defs.h"
#include "protocols/P25/P25Phy.h"
#include "protocols/P25/P25Status.h"
}

namespace {

/** One LDU1 as the demodulator hands it over, with the fields a test varies. */
p25Frame voiceFrame(uint16_t nac = 0x293, uint16_t talkgroup = 7,
                    uint32_t source = 0x123456, bool lcValid = true)
{
    p25Frame f;
    memset(&f, 0, sizeof(f));

    f.duid = P25_DUID_LDU1;
    f.nac = nac;
    f.spanSymbols = P25_FRAME_SYMBOLS;
    f.syncErrors = 0;
    f.bchErrors = 0;
    f.lcValid = lcValid;
    f.lcRsErrors = lcValid ? 0 : -1;
    f.lcFormat = 0;
    f.lcService = 0;
    f.talkgroup = talkgroup;
    f.source = source;
    f.voiceCount = P25_IMBE_PER_LDU;
    f.voiceBad = 0;
    f.imbeErrors = 0;

    return f;
}

/** A control-channel block, as decodeTsbk accepts or refuses it. */
p25Frame tsbkFrame(bool crcOk)
{
    p25Frame f;
    memset(&f, 0, sizeof(f));

    f.duid = P25_DUID_TSBK;
    f.nac = 0x293;
    f.spanSymbols = (uint16_t)p25_frame_symbols(P25_DUID_TSBK);
    f.tsbkValid = crcOk;

    return f;
}

/** A header, with the encryption algorithm it announces. */
p25Frame hduFrame(uint8_t algid)
{
    p25Frame f;
    memset(&f, 0, sizeof(f));

    f.duid = P25_DUID_HDU;
    f.nac = 0x293;
    f.hdu = true;
    f.hduAlgid = algid;
    f.hduKeyid = 0x1234;
    f.hduGroup = 7;

    return f;
}

} // namespace

TEST_CASE("a cleared P25 status reads as unknown, not as zero", "[p25][status]")
{
    /*
     * The rule of docs/TEST_PLAN.md §3: with no frame decoded the identifiers
     * are -1.  A zero here is a NAC 0x000 and a talkgroup 0 on the screen,
     * i.e. a call that does not exist.
     */
    p25Status_clear();

    struct p25Status st;
    p25Status_get(&st);

    REQUIRE_FALSE(st.sync);
    REQUIRE_FALSE(st.voice);
    REQUIRE_FALSE(st.encrypted);
    REQUIRE(st.nac == -1);
    REQUIRE(st.duid == -1);
    REQUIRE(st.talkgroup == -1);
    REQUIRE(st.source == -1);
    REQUIRE(st.frames == 0);
    REQUIRE(st.voiceFrames == 0);
    REQUIRE(st.imbe == 0);
    REQUIRE(st.imbeBad == 0);
    REQUIRE(st.tsbk == 0);
    REQUIRE(st.crcOk == 0);

    REQUIRE_FALSE(p25Status_sync());
}

TEST_CASE("a good voice frame publishes its NAC, call and counters",
          "[p25][status]")
{
    p25Status_clear();

    const p25Frame f = voiceFrame();
    p25Status_frame(&f);

    struct p25Status st;
    p25Status_get(&st);

    REQUIRE(st.sync);
    REQUIRE(st.voice);
    REQUIRE(st.nac == 0x293);
    REQUIRE(st.duid == (int8_t)P25_DUID_LDU1);
    REQUIRE(st.talkgroup == 7);
    REQUIRE(st.source == 0x123456);
    REQUIRE(st.frames == 1);
    REQUIRE(st.voiceFrames == 1);
    REQUIRE(st.imbe == P25_IMBE_PER_LDU);
    REQUIRE(st.imbeBad == 0);
    REQUIRE(st.imbeErrors == 0);
    REQUIRE_FALSE(st.encrypted);

    REQUIRE(p25Status_sync());
}

TEST_CASE("a refused link control does not move the call it belongs to",
          "[p25][status]")
{
    /*
     * Frame two of the same transmission arrived with its LCW outside the
     * Reed-Solomon's reach: lcValid is false and talkgroup/source are zero.
     * The call is still the one frame one announced -- the screen keeps showing
     * talkgroup 7 from source 0x123456 -- while the frame counters keep
     * counting and the NAC/DUID of the latest frame win.
     */
    p25Status_clear();

    const p25Frame good = voiceFrame(0x293, 7, 0x123456, true);
    p25Status_frame(&good);

    const p25Frame bad = voiceFrame(0x12A, 0, 0, false);
    p25Status_frame(&bad);

    struct p25Status st;
    p25Status_get(&st);

    REQUIRE(st.frames == 2);
    REQUIRE(st.voiceFrames == 2);
    REQUIRE(st.talkgroup == 7);
    REQUIRE(st.source == 0x123456);
    /* The last frame is what the screen labels: a new NAC means a new site */
    REQUIRE(st.nac == 0x12A);
    REQUIRE(st.duid == (int8_t)P25_DUID_LDU1);
}

TEST_CASE("a TSBK whose CRC failed is counted, not accepted", "[p25][status]")
{
    /*
     * tsbk counts the blocks the trellis decoded, crcOk the ones whose CRC-16
     * then checked out.  A receiver that counts a refused block as accepted
     * hides a control channel that is not decoding -- which is exactly the
     * failure the diagnostics page is for.
     */
    p25Status_clear();

    const p25Frame bad = tsbkFrame(false);
    p25Status_frame(&bad);

    struct p25Status st;
    p25Status_get(&st);
    REQUIRE(st.tsbk == 1);
    REQUIRE(st.crcOk == 0);
    REQUIRE(st.duid == (int8_t)P25_DUID_TSBK);
    REQUIRE_FALSE(st.voice);

    const p25Frame good = tsbkFrame(true);
    p25Status_frame(&good);

    p25Status_get(&st);
    REQUIRE(st.tsbk == 2);
    REQUIRE(st.crcOk == 1);
}

TEST_CASE("encryption is published only when the call announces it",
          "[p25][status]")
{
    /*
     * A clear header (ALGID 0x80) must not raise the flag, an encrypted one
     * must, and the service options of a link control (bit 0x40) are the other
     * way a call says it is encrypted.
     */
    p25Status_clear();

    const p25Frame clear = hduFrame(0x80);
    p25Status_frame(&clear);

    struct p25Status st;
    p25Status_get(&st);
    REQUIRE(st.duid == (int8_t)P25_DUID_HDU);
    REQUIRE(st.hduAlgid == 0x80);
    REQUIRE_FALSE(st.encrypted);

    const p25Frame encrypted = hduFrame(0x84);
    p25Status_frame(&encrypted);
    p25Status_get(&st);
    REQUIRE(st.hduAlgid == 0x84);
    REQUIRE(st.encrypted);

    /* The flag is sticky for the rest of the call, which is what the operator
     * needs: a clear LDU after an encrypted header does not make it clear. */
    p25Status_frame(&clear);
    p25Status_get(&st);
    REQUIRE(st.encrypted);
}

TEST_CASE("the link control's service options raise the encryption flag",
          "[p25][status]")
{
    p25Status_clear();

    p25Frame f = voiceFrame();
    f.lcService = 0x40;
    p25Status_frame(&f);

    struct p25Status st;
    p25Status_get(&st);
    REQUIRE(st.encrypted);
    REQUIRE(st.lcFormat == 0);
}

TEST_CASE("a reset forgets the call and keeps the counters", "[p25][status]")
{
    /*
     * p25Status_reset() is what a new frequency calls: the identifiers go back
     * to unknown so the screen does not show the previous site's NAC, but the
     * counters survive because they are the record of what was heard.
     */
    p25Status_clear();

    const p25Frame f = voiceFrame();
    p25Status_frame(&f);

    struct p25Status before;
    p25Status_get(&before);
    REQUIRE(before.frames == 1);

    p25Status_reset();

    struct p25Status st;
    p25Status_get(&st);
    REQUIRE(st.frames == before.frames);
    REQUIRE(st.imbe == before.imbe);
    REQUIRE(st.nac == -1);
    REQUIRE(st.duid == -1);
    REQUIRE(st.talkgroup == -1);
    REQUIRE(st.source == -1);
    REQUIRE_FALSE(st.sync);
    REQUIRE_FALSE(st.voice);
}

TEST_CASE("the synchronisation indicator falls off after its hold",
          "[p25][status]")
{
    /*
     * sync means "a frame in the last 300 ms": the LED and the screen follow
     * it, so a receiver that leaves it on after the carrier drops looks locked
     * on a dead channel.  Only the falling edge is testable in real time, and
     * it is the one that matters.
     */
    p25Status_clear();

    const p25Frame f = voiceFrame();
    p25Status_frame(&f);
    REQUIRE(p25Status_sync());

    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    REQUIRE_FALSE(p25Status_sync());

    struct p25Status st;
    p25Status_get(&st);
    REQUIRE_FALSE(st.sync);
}
