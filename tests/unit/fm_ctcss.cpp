/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * CTCSS on the analogue modes: the tone table, how a channel's tone reaches the
 * transmitter and the receiver, and what the radio says about it.
 *
 * The three things an operator can get wrong with a tone are checked here:
 *
 *  - the tone itself. The table is 50 EIA tones, it is what the chip is
 *    programmed with and what every screen and prompt reads, so it is checked
 *    against its own standard rather than against a copy of itself: the
 *    published frequencies, in order, from 67.0 to 254.1 Hz;
 *  - the selection. A channel carries an enable flag and an index per
 *    direction, and the index lives in a 7 bit field while the table has 50
 *    entries: an index off the end must not read flash past the table, and a
 *    disabled tone must reach the radio as "no tone" whatever index was left
 *    behind it;
 *  - the announcement. The voice prompt says the tone once when both
 *    directions share it and says which is which when they differ, which is the
 *    only way an operator without the screen open can tell an RX tone from a TX
 *    one.
 *
 * The prompt is read back with vp_pendingSequence(), the player's own queue:
 * what is announced is the sequence queued, and that is what is compared.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <vector>

extern "C" {
#include "emulator/emulator.h"
#include "interfaces/radio.h"
#include "core/cps.h"
#include "core/utils.h"
#include "core/state.h"
#include "core/channel_store.h"
#include "core/voicePromptUtils.h"
#include "core/voicePrompts.h"
#include "rtx/rtx.h"
}

namespace
{

pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;

/* Long enough for any single tone announcement (ten prompts at most). */
constexpr size_t SEQ_MAX = 64;

/*
 * The 50 tones of the EIA/CTCSS plan, in tenths of a hertz, as published. This
 * is deliberately a literal table and not a copy of the firmware's: a test that
 * asks the code what the answer should be cannot catch a wrong answer.
 */
constexpr uint16_t EIA_TONES[] = {
    670,  693,  719,  744,  770,  797,  825,  854,  885,  915,
    948,  974,  1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
    1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679,
    1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966, 1995,
    2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};

/// The prompt sequence a fresh announcement leaves queued.
std::vector<uint16_t> pending()
{
    uint16_t buf[SEQ_MAX];
    size_t n = vp_pendingSequence(buf, SEQ_MAX);

    return std::vector<uint16_t>(buf, buf + n);
}

/**
 * A tone the way the prompt spells it: "88.5" is 8, 8, point, 5.
 *
 * The digits come from printing the number, not from the prompt's own
 * arithmetic, so the test knows what the operator should hear.
 */
std::vector<uint16_t> spokenTone(uint16_t tenths)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d.%d", tenths / 10, tenths % 10);

    std::vector<uint16_t> v;
    for (const char *p = buf; *p != '\0'; p++) {
        if (*p == '.')
            v.push_back(PROMPT_POINT);
        else
            v.push_back((uint16_t)(PROMPT_0 + (*p - '0')));
    }
    v.push_back(PROMPT_HERTZ);
    return v;
}

void append(std::vector<uint16_t> &dst, const std::vector<uint16_t> &src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

/// The string-table index of the word "off", which is not a fixed prompt.
uint16_t promptOff()
{
    return (uint16_t)(NUM_VOICE_PROMPTS
                      + (&currentLanguage->off
                         - &currentLanguage->languageName));
}

/// Announce a tone pair as the channel editor does.
std::vector<uint16_t> announce(bool rxEn, uint8_t rx,
                               bool txEn, uint8_t tx,
                               enum vpQueueFlags flags)
{
    vp_flush();
    vp_announceCTCSS(rxEn, rx, txEn, tx, flags);

    return pending();
}

void expectSequence(const std::vector<uint16_t> &got,
                    const std::vector<uint16_t> &want)
{
    INFO("queued " << got.size() << " prompts, expected " << want.size());
    REQUIRE(got.size() == want.size());

    for (size_t i = 0; (i < want.size()) && (i < got.size()); i++) {
        INFO("prompt " << i);
        REQUIRE(got[i] == want[i]);
    }
}

/*
 * The bank, in RAM: enough of a chstorePort for a channel to be stored and read
 * back the way the radio does it, without a flash driver.
 */
struct RamBank
{
    std::vector<uint8_t> mem = std::vector<uint8_t>(CHSTORE_PARTITION_SIZE,
                                                    0xFF);
    chstorePort port;

    RamBank()
    {
        port.read   = read;
        port.erase  = erase;
        port.write  = write;
        port.ctx    = this;
        port.size   = (uint32_t)mem.size();
        port.sector = CHSTORE_SECTOR_SIZE;
    }

    void boot()
    {
        chstore_bind(&port);
        REQUIRE(chstore_init() == 0);
    }

    static int read(void *ctx, uint32_t off, void *buf, size_t len)
    {
        RamBank *b = static_cast<RamBank *>(ctx);
        if ((off + len) > b->mem.size())
            return -1;
        memcpy(buf, &b->mem[off], len);
        return 0;
    }

    static int erase(void *ctx, uint32_t off, size_t len)
    {
        RamBank *b = static_cast<RamBank *>(ctx);
        if ((off + len) > b->mem.size())
            return -1;
        memset(&b->mem[off], 0xFF, len);
        return 0;
    }

    static int write(void *ctx, uint32_t off, const void *buf, size_t len)
    {
        RamBank *b = static_cast<RamBank *>(ctx);
        if ((off + len) > b->mem.size())
            return -1;
        memcpy(&b->mem[off], buf, len);
        return 0;
    }
};

} /* namespace */

TEST_CASE("the tone table is the EIA plan, in order", "[ctcss][fm]")
{
    REQUIRE(CTCSS_FREQ_NUM == sizeof(EIA_TONES) / sizeof(EIA_TONES[0]));

    /* Same values, same order: every index is the tone the plan puts there */
    for (size_t i = 0; i < CTCSS_FREQ_NUM; i++) {
        INFO("index " << i);
        REQUIRE(ctcss_tone[i] == EIA_TONES[i]);
    }

    /* Strictly increasing, so an index is a tone and never a repeat */
    for (size_t i = 1; i < CTCSS_FREQ_NUM; i++)
        REQUIRE(ctcss_tone[i] > ctcss_tone[i - 1]);

    /* And the marks a radio is specified against */
    REQUIRE(ctcss_tone[CTCSS_88_5] == 885);
    REQUIRE(ctcss_tone[CTCSS_100_0] == 1000);
    REQUIRE(ctcss_tone[CTCSS_151_4] == 1514);
    REQUIRE(ctcss_tone[CTCSS_FREQ_NUM - 1] == 2541);
}

TEST_CASE("a tone index maps to its frequency and back", "[ctcss][fm]")
{
    for (uint8_t i = 0; i < CTCSS_FREQ_NUM; i++) {
        REQUIRE(ctcssFreqFromIndex(i) == ctcss_tone[i]);
        REQUIRE(ctcssFreqToIndex(ctcss_tone[i]) == i);
    }

    /* A frequency that is not a tone has no index: 255 is "none" */
    REQUIRE(ctcssFreqToIndex(0) == 255);
    REQUIRE(ctcssFreqToIndex(880) == 255);
    REQUIRE(ctcssFreqToIndex(3000) == 255);
}

TEST_CASE("an index past the end of the table is a tone, not flash",
          "[ctcss][fm]")
{
    /*
     * The index is a 7 bit field, so 50..127 are values a damaged record or a
     * codeplug from another tool can carry. Wrapping keeps them inside the
     * table: the alternative is programming the chip with whatever follows it
     * in flash and reading that number aloud.
     */
    for (unsigned i = CTCSS_FREQ_NUM; i < 128; i++) {
        uint16_t tone = ctcssFreqFromIndex((uint8_t)i);

        INFO("index " << i);
        REQUIRE(tone == ctcss_tone[i % CTCSS_FREQ_NUM]);
        REQUIRE(ctcssFreqToIndex(tone) == i % CTCSS_FREQ_NUM);

        /* It is one of the published tones, not a number from elsewhere */
        bool legal = false;
        for (uint16_t t : EIA_TONES)
            legal |= (t == tone);
        REQUIRE(legal);
    }
}

TEST_CASE("a channel's tones reach the radio only when they are enabled",
          "[ctcss][fm][channel]")
{
    RamBank bank;
    bank.boot();

    /*
     * chstore_applyRtx() is the path a stored channel takes to the transmitter
     * and the receiver: the enable flag decides, and a disabled direction is
     * zero to the driver even when an index is still stored behind it, which is
     * what the editor leaves when a tone is switched off.
     */
    chstore_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    snprintf(ch.name, sizeof(ch.name), "TONE");
    ch.mode = OPMODE_FM;
    ch.rx_freq = 145500000;
    ch.tx_freq = 145500000;
    ch.power = 500;
    ch.rx_tone_en = 1;
    ch.rx_tone = CTCSS_88_5;
    ch.tx_tone_en = 1;
    ch.tx_tone = CTCSS_100_0;
    ch.flags = CHSTORE_FLAG_SIMPLEX;

    int idx = chstore_add(&ch);
    REQUIRE(idx == 0);
    REQUIRE(chstore_commit() == 0);

    rtxStatus_t rtx;
    memset(&rtx, 0, sizeof(rtx));
    REQUIRE(chstore_applyRtx(0, &rtx) == 0);

    REQUIRE(rtx.rxToneEn == 1);
    REQUIRE(rtx.rxTone == 885);
    REQUIRE(rtx.txToneEn == 1);
    REQUIRE(rtx.txTone == 1000);

    /* Off: no tone reaches the radio, even with the index left in place */
    ch.rx_tone_en = 0;
    ch.tx_tone_en = 0;
    ch.rx_tone = CTCSS_151_4;
    ch.tx_tone = CTCSS_151_4;
    REQUIRE(chstore_set(0, &ch) == 0);

    memset(&rtx, 0, sizeof(rtx));
    REQUIRE(chstore_applyRtx(0, &rtx) == 0);
    REQUIRE(rtx.rxToneEn == 0);
    REQUIRE(rtx.rxTone == 0);
    REQUIRE(rtx.txToneEn == 0);
    REQUIRE(rtx.txTone == 0);

    /*
     * An index off the end of the table cannot be stored at all: the record is
     * refused, so the bank never carries one. What protects the paths that do
     * not go through this validation -- a codeplug imported by a legacy reader,
     * or the same byte read through the digital modes' union -- is
     * ctcssFreqFromIndex(), checked above.
     */
    chstore_channel_t bad = ch;
    bad.rx_tone_en = 1;
    bad.rx_tone = 127;
    REQUIRE_FALSE(chstore_valid(&bad));

    bad.rx_tone_en = 0;
    bad.tx_tone_en = 1;
    bad.tx_tone = 60;
    bad.rx_tone = 0;
    REQUIRE_FALSE(chstore_valid(&bad));

    /* The last tone of the table is still a valid channel */
    bad.rx_tone_en = 1;
    bad.rx_tone = CTCSS_FREQ_NUM - 1;
    bad.tx_tone_en = 0;
    REQUIRE(chstore_valid(&bad));

    /* Both directions of the editor's mapping keep the same four fields */
    ch.rx_tone_en = 1;
    ch.rx_tone = CTCSS_88_5;
    ch.tx_tone_en = 0;
    ch.tx_tone = 0;
    REQUIRE(chstore_set(0, &ch) == 0);

    channel_t from;
    memset(&from, 0, sizeof(from));
    from.fm.rxToneEn = 1;
    from.fm.rxTone = CTCSS_110_9;
    from.fm.txToneEn = 0;
    from.fm.txTone = 0;

    chstore_channel_t out;
    chstore_fromChannel(&from, &out);
    REQUIRE(out.rx_tone_en == 1);
    REQUIRE(out.rx_tone == CTCSS_110_9);
    REQUIRE(out.tx_tone_en == 0);
    REQUIRE(out.tx_tone == 0);

    channel_t back;
    memset(&back, 0, sizeof(back));
    REQUIRE(chstore_toChannel(0, &back) == 0);
    REQUIRE(back.mode == OPMODE_FM);
    REQUIRE(back.fm.rxToneEn == 1);
    REQUIRE(back.fm.rxTone == CTCSS_88_5);

    chstore_bind(nullptr);
}

TEST_CASE("the FM tone fields read through a DMR channel still name a tone",
          "[ctcss][fm][channel]")
{
    /*
     * The FM tones and the DMR colour codes share the same two bytes of the
     * channel union, and the RTX configuration copies the tone fields whatever
     * the channel's mode is (core/threads.c). A DMR channel whose colour codes
     * sit in those bytes therefore hands the copy a "tone index" of up to 127 --
     * which is exactly the case ctcssFreqFromIndex() has to survive, because
     * the value is not a tone index at all.
     */
    channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.mode = OPMODE_DMR;
    ch.dmr.rxColorCode = 1;
    ch.dmr.txColorCode = 7;
    ch.dmr.contact_index = 400;

    uint16_t rx = ctcssFreqFromIndex(ch.fm.rxTone);
    uint16_t tx = ctcssFreqFromIndex(ch.fm.txTone);

    bool rxLegal = false;
    bool txLegal = false;
    for (uint16_t t : EIA_TONES) {
        rxLegal |= (t == rx);
        txLegal |= (t == tx);
    }

    REQUIRE(rxLegal);
    REQUIRE(txLegal);
    REQUIRE(ctcssFreqToIndex(rx) != 255);
    REQUIRE(ctcssFreqToIndex(tx) != 255);
}

TEST_CASE("a mode that is not FM carries no tones", "[ctcss][fm][rtx]")
{
    /*
     * The tones share storage with the digital modes' fields, and only the FM
     * mode reads them. Selecting another mode has to clear the enables, or a
     * channel's tone stays programmed into the chip behind a digital mode and
     * behind the airband's AM profile.
     */
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;

    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.opMode   = OPMODE_FM;
    cfg.rxToneEn = 1;
    cfg.rxTone   = 885;
    cfg.txToneEn = 1;
    cfg.txTone   = 885;
    rtx_configure(&cfg);
    rtx_task();

    /* FM keeps them: they are what its squelch and its transmitter use */
    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_FM);
    REQUIRE(rtx_getCurrentStatus().rxToneEn == 1);
    REQUIRE(rtx_getCurrentStatus().txToneEn == 1);
    REQUIRE(rtx_getCurrentStatus().rxTone == 885);

    /* Airband does not, and says so in the status the UI reads */
    cfg = rtx_getCurrentStatus();
    cfg.opMode = OPMODE_AIRBAND;
    cfg.rxFrequency = 118000000;
    cfg.txFrequency = 118000000;
    rtx_configure(&cfg);
    rtx_task();

    REQUIRE(rtx_getCurrentStatus().opMode == OPMODE_AIRBAND);
    REQUIRE(rtx_getCurrentStatus().rxToneEn == 0);
    REQUIRE(rtx_getCurrentStatus().txToneEn == 0);

    /* The tone comes back with FM, because it is the UI that carries it */
    cfg = rtx_getCurrentStatus();
    cfg.opMode = OPMODE_FM;
    cfg.rxToneEn = 1;
    cfg.txToneEn = 1;
    rtx_configure(&cfg);
    rtx_task();
    REQUIRE(rtx_getCurrentStatus().rxToneEn == 1);

    cfg.opMode = OPMODE_NONE;
    rtx_configure(&cfg);
    rtx_task();
    rtx_terminate();
}

TEST_CASE("the voice prompt names the tone the channel carries",
          "[ctcss][fm][prompt]")
{
    const uint8_t previousLevel = state.settings.vpLevel;
    state.settings.vpLevel = vpHigh;

    std::vector<uint16_t> want;

    /*
     * Both directions on the same tone: the radio says the tone once and does
     * not make the operator listen to the same number twice.
     */
    want = { PROMPT_TONE };
    append(want, spokenTone(885));
    expectSequence(announce(true, CTCSS_88_5, true, CTCSS_88_5,
                            vpqIncludeDescriptions), want);

    /* Different tones: it says which is which, receive first */
    want = { PROMPT_RECEIVE, PROMPT_TONE };
    append(want, spokenTone(885));
    append(want, { PROMPT_TRANSMIT, PROMPT_TONE });
    append(want, spokenTone(1000));
    expectSequence(announce(true, CTCSS_88_5, true, CTCSS_100_0,
                            vpqIncludeDescriptions), want);

    /* Only one direction set: just that one, and no other word */
    want = { PROMPT_RECEIVE, PROMPT_TONE };
    append(want, spokenTone(1514));
    expectSequence(announce(true, CTCSS_151_4, false, 0,
                            vpqIncludeDescriptions), want);

    want = { PROMPT_TRANSMIT, PROMPT_TONE };
    append(want, spokenTone(719));
    expectSequence(announce(false, 0, true, CTCSS_71_9,
                            vpqIncludeDescriptions), want);

    /* Both off is "tone off", not "tone 67.0" */
    expectSequence(announce(false, 0, false, 0, vpqIncludeDescriptions),
                   { PROMPT_TONE, promptOff() });

    /*
     * At low verbosity the descriptions go and only the value is spoken, which
     * is what the operator hears while turning the tone with the quick menu.
     */
    expectSequence(announce(true, CTCSS_88_5, true, CTCSS_88_5, vpqDefault),
                   spokenTone(885));

    /*
     * And a channel whose stored index is off the end is announced as a tone of
     * the plan: the prompt reads the same table the driver is programmed from,
     * so it has to wrap the index the same way.
     */
    want = { PROMPT_TONE };
    append(want, spokenTone(ctcss_tone[127 % CTCSS_FREQ_NUM]));
    expectSequence(announce(true, 127, true, 127, vpqIncludeDescriptions), want);

    state.settings.vpLevel = previousLevel;
}
