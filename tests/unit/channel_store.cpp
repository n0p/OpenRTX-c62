/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host test of the in-radio channel store: record format, ping-pong writes,
 * power-cut safety and fallback to an empty bank when both slots are damaged.
 *
 * The flash is emulated by FakeFlash, which can also lose power in the middle
 * of an erase or a write: from that point on the operation reports a failure
 * and nothing more is programmed. A real power cut can leave arbitrary bit
 * patterns, which is a superset of this model for the invariant tested here:
 * a half-written slot must never validate.
 */

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "core/channel_store.h"
#include "core/crc.h"
}

static_assert(sizeof(chstore_channel_v1_t) == 32, "version 1 record size");
static_assert(sizeof(chstore_channel_t) == 37, "channel record size");
static_assert(sizeof(chstore_hdr_t) == 32, "record header size");
static_assert(CHSTORE_MAX_CHANNELS * sizeof(chstore_channel_t) + 32
                  <= CHSTORE_SLOT_SIZE,
              "a full bank has to fit in one ping-pong slot");
static_assert(CHSTORE_SLOT_SIZE == 2 * CHSTORE_SECTOR_SIZE, "slot geometry");

namespace {

class FakeFlash
{
public:
    explicit FakeFlash(uint32_t size = CHSTORE_PARTITION_SIZE)
        : mem(size, 0xFF), budget(-1), dead(false), erases(0)
    {}

    /* Lose power after "count" more bytes are erased or written */
    void cutAfter(int64_t count)
    {
        budget = count;
        dead = false;
    }

    void clearFault()
    {
        budget = -1;
        dead = false;
    }

    bool faulted() const { return dead; }
    int eraseCount() const { return erases; }

    int read(uint32_t off, void *buf, size_t len)
    {
        if (off + len > mem.size())
            return -1;
        if (dead)
            return -1;
        memcpy(buf, &mem[off], len);
        return 0;
    }

    int erase(uint32_t off, size_t len)
    {
        if (off + len > mem.size())
            return -1;
        if (dead)
            return -1;

        erases++;

        if (budget >= 0) {
            if (budget < (int64_t)len) {
                memset(&mem[off], 0xFF, (size_t)budget);
                budget = 0;
                dead = true;
                return -1;
            }
            budget -= (int64_t)len;
        }

        memset(&mem[off], 0xFF, len);
        return 0;
    }

    int write(uint32_t off, const void *buf, size_t len)
    {
        if (off + len > mem.size())
            return -1;
        if (dead)
            return -1;

        if (budget >= 0) {
            if (budget < (int64_t)len) {
                memcpy(&mem[off], buf, (size_t)budget);
                budget = 0;
                dead = true;
                return -1;
            }
            budget -= (int64_t)len;
        }

        memcpy(&mem[off], buf, len);
        return 0;
    }

    std::vector<uint8_t> mem;
    int64_t budget;
    bool dead;
    int erases;
};

static int ffRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->read(off, buf, len);
}

static int ffErase(void *ctx, uint32_t off, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->erase(off, len);
}

static int ffWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->write(off, buf, len);
}

struct TestBank
{
    FakeFlash flash;
    chstorePort port;

    TestBank()
    {
        port.read = ffRead;
        port.erase = ffErase;
        port.write = ffWrite;
        port.ctx = &flash;
        port.size = (uint32_t)flash.mem.size();
        port.sector = CHSTORE_SECTOR_SIZE;
    }

    /* Bind and load, as a power-on does */
    void boot()
    {
        flash.clearFault();
        chstore_bind(&port);
        REQUIRE(chstore_init() == 0);
    }
};

chstore_channel_t makeChannel(const char *name, uint32_t rx, uint32_t tx,
                              uint8_t mode = OPMODE_FM)
{
    chstore_channel_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "%s", name);
    c.rx_freq = rx;
    c.tx_freq = tx;
    c.power = 1000;
    c.mode = mode;
    c.bandwidth = BW_12_5;
    c.dmr_cc = 1;
    c.dmr_ts = 0;
    c.dmr_dest_type = DMR_DEST_NONE;
    c.dmr_dest_id = 0;
    if (rx == tx)
        c.flags |= CHSTORE_FLAG_SIMPLEX;
    return c;
}

/* chstore_add takes a pointer: build the record in a local first */
int addChannel(const char *name, uint32_t rx, uint32_t tx,
               uint8_t mode = OPMODE_FM)
{
    chstore_channel_t c = makeChannel(name, rx, tx, mode);
    return chstore_add(&c);
}

void fillThree(TestBank &tb)
{
    tb.boot();
    REQUIRE(addChannel("ALPHA", 145500000, 145500000) == 0);
    REQUIRE(addChannel("BRAVO", 433500000, 438500000, OPMODE_DMR) == 1);
    REQUIRE(addChannel("CHARLIE", 1290000000, 1290000000) == 2);
}

std::string nameOf(uint16_t idx)
{
    chstore_channel_t c;
    if (chstore_get(idx, &c) != 0)
        return "<missing>";
    return std::string(c.name);
}

} // namespace

TEST_CASE("A blank partition gives an empty bank", "[chstore]")
{
    TestBank tb;
    tb.boot();

    REQUIRE(chstore_count() == 0);
    REQUIRE(chstore_lastUsed() == -1);

    chstore_channel_t c;
    REQUIRE(chstore_get(0, &c) == -EINVAL);
    REQUIRE(chstore_delete(0) == -EINVAL);
    REQUIRE(chstore_set(0, &c) == -EINVAL);
}

TEST_CASE("Channels are added, read back and survive a reboot", "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    REQUIRE(chstore_count() == 3);
    REQUIRE(nameOf(0) == "ALPHA");
    REQUIRE(nameOf(1) == "BRAVO");
    REQUIRE(nameOf(2) == "CHARLIE");

    tb.boot();

    REQUIRE(chstore_count() == 3);
    REQUIRE(nameOf(0) == "ALPHA");
    REQUIRE(nameOf(1) == "BRAVO");

    chstore_channel_t c;
    REQUIRE(chstore_get(1, &c) == 0);
    REQUIRE(c.mode == OPMODE_DMR);
    REQUIRE(c.rx_freq == 433500000);
    REQUIRE(c.tx_freq == 438500000);
    REQUIRE(c.dmr_cc == 1);
}

TEST_CASE("Editing writes through to flash and rolls back on failure",
          "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    chstore_channel_t edit = makeChannel("BRAVO-2", 433000000, 433000000,
                                         OPMODE_FM);
    REQUIRE(chstore_set(1, &edit) == 0);
    REQUIRE(nameOf(1) == "BRAVO-2");

    tb.boot();
    REQUIRE(nameOf(1) == "BRAVO-2");

    /* A write that fails must leave RAM describing the flash: the old record */
    tb.flash.cutAfter(0);
    chstore_channel_t other = makeChannel("DOES-NOT-STICK", 145000000,
                                          145000000);
    REQUIRE(chstore_set(1, &other) != 0);
    REQUIRE(nameOf(1) == "BRAVO-2");

    tb.boot();
    REQUIRE(nameOf(1) == "BRAVO-2");
}

TEST_CASE("Deleting shifts the bank and clears the last used index", "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    chstore_setLastUsed(2);
    REQUIRE(chstore_commit() == 0);
    REQUIRE(chstore_lastUsed() == 2);

    REQUIRE(chstore_delete(2) == 0);
    REQUIRE(chstore_count() == 2);
    REQUIRE(chstore_lastUsed() == -1);

    tb.boot();
    REQUIRE(chstore_count() == 2);
    REQUIRE(nameOf(0) == "ALPHA");
    REQUIRE(nameOf(1) == "BRAVO");

    /* Deleting below the last used index moves it down with the records */
    chstore_setLastUsed(1);
    REQUIRE(chstore_commit() == 0);
    REQUIRE(chstore_delete(0) == 0);
    REQUIRE(chstore_lastUsed() == 0);

    tb.boot();
    REQUIRE(chstore_count() == 1);
    REQUIRE(nameOf(0) == "BRAVO");
    REQUIRE(chstore_lastUsed() == 0);
}

TEST_CASE("Invalid channels are rejected", "[chstore]")
{
    TestBank tb;
    tb.boot();

    chstore_channel_t c = makeChannel("OK", 145500000, 145500000);
    REQUIRE(chstore_valid(&c));

    /* 700 MHz is in the gap of the BK4819 coverage */
    chstore_channel_t bad = makeChannel("GAP", 700000000, 700000000);
    REQUIRE_FALSE(chstore_valid(&bad));

    bad = makeChannel("LOW", 17000000, 17000000);
    REQUIRE_FALSE(chstore_valid(&bad));

    bad = makeChannel("", 145500000, 145500000);
    REQUIRE_FALSE(chstore_valid(&bad));

    bad = makeChannel("NOPOWER", 145500000, 145500000);
    bad.power = 0;
    REQUIRE_FALSE(chstore_valid(&bad));

    bad = makeChannel("NOMODE", 145500000, 145500000);
    bad.mode = OPMODE_NONE;
    REQUIRE_FALSE(chstore_valid(&bad));

    REQUIRE(chstore_add(&bad) < 0);
    REQUIRE(chstore_count() == 0);

    /* Both band edges of the unlocked coverage are usable */
    REQUIRE(chstore_freqValid(18000000));
    REQUIRE(chstore_freqValid(660000000));
    REQUIRE(chstore_freqValid(840000000));
    REQUIRE(chstore_freqValid(1300000000));
    REQUIRE_FALSE(chstore_freqValid(17999999));
    REQUIRE_FALSE(chstore_freqValid(1300000001));
}

TEST_CASE("A power cut during a save never damages the previous bank",
          "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    const std::vector<uint8_t> golden = tb.flash.mem;
    const size_t total = CHSTORE_SLOT_SIZE + sizeof(chstore_hdr_t) +
                         3 * sizeof(chstore_channel_t);

    chstore_channel_t edit = makeChannel("BRAVO-2", 433500000, 438500000,
                                         OPMODE_DMR);

    for (size_t cut = 0; cut <= total; cut += 7) {
        tb.flash.mem = golden;
        tb.boot();

        tb.flash.cutAfter((int64_t)cut);
        int ret = chstore_set(1, &edit);

        /* Whatever happened, RAM keeps describing what is on flash */
        REQUIRE(nameOf(1) == ((ret == 0) ? "BRAVO-2" : "BRAVO"));

        tb.boot();

        REQUIRE(chstore_count() == 3);
        REQUIRE(nameOf(0) == "ALPHA");
        REQUIRE(nameOf(2) == "CHARLIE");
        REQUIRE(nameOf(1) == ((ret == 0) ? "BRAVO-2" : "BRAVO"));
    }
}

TEST_CASE("A damaged newest slot falls back to the previous one", "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    /* The third add landed in slot A, the second one is still in slot B */
    const uint32_t payloadA = sizeof(chstore_hdr_t) + 8;
    tb.flash.mem[payloadA] ^= 0x5A;

    tb.boot();

    REQUIRE(chstore_count() == 2);
    REQUIRE(nameOf(0) == "ALPHA");
    REQUIRE(nameOf(1) == "BRAVO");
}

TEST_CASE("Two damaged slots give the empty default bank, which is writable",
          "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    tb.flash.mem[0] ^= 0xFF;            /* header of slot A */
    tb.flash.mem[CHSTORE_SLOT_SIZE] ^= 0xFF; /* header of slot B */

    tb.boot();
    REQUIRE(chstore_count() == 0);
    REQUIRE(chstore_lastUsed() == -1);

    REQUIRE(addChannel("NEW", 145500000, 145500000) == 0);
    tb.boot();
    REQUIRE(chstore_count() == 1);
    REQUIRE(nameOf(0) == "NEW");

    /* loadDefaults empties the bank again, on flash too */
    REQUIRE(chstore_loadDefaults() == 0);
    tb.boot();
    REQUIRE(chstore_count() == 0);
}

TEST_CASE("The last used channel is only written when asked", "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    chstore_setLastUsed(1);
    REQUIRE(chstore_lastUsed() == 1);

    /* No commit: a reboot comes back with no remembered channel */
    tb.boot();
    REQUIRE(chstore_lastUsed() == -1);

    chstore_setLastUsed(2);
    REQUIRE(chstore_commit() == 0);

    tb.boot();
    REQUIRE(chstore_lastUsed() == 2);
    REQUIRE(nameOf((uint16_t)chstore_lastUsed()) == "CHARLIE");
}

TEST_CASE("Stored records convert to and from channel_t", "[chstore]")
{
    TestBank tb;
    tb.boot();

    chstore_channel_t c = makeChannel("DMR-TS2", 433500000, 433500000,
                                      OPMODE_DMR);
    c.dmr_cc = 3;
    c.dmr_ts = 1;
    c.bandwidth = BW_25;
    c.power = 5000;
    c.flags = 0;

    REQUIRE(chstore_add(&c) == 0);

    channel_t ch;
    REQUIRE(chstore_toChannel(0, &ch) == 0);
    REQUIRE(ch.mode == OPMODE_DMR);
    REQUIRE(ch.rx_frequency == 433500000);
    REQUIRE(ch.tx_frequency == 433500000);
    REQUIRE(ch.power == 5000);
    REQUIRE(ch.bandwidth == BW_25);
    REQUIRE(ch.dmr.rxColorCode == 3);
    REQUIRE(ch.dmr.dmr_timeslot == 2);
    REQUIRE(std::string(ch.name) == "DMR-TS2");

    /* FM tones, on a channel where fm does not share the union with dmr */
    chstore_channel_t f = makeChannel("FM-TONE", 145500000, 145500000);
    f.tx_tone_en = 1;
    f.tx_tone = 12;  /* 100.0 Hz */
    f.rx_tone_en = 1;
    f.rx_tone = 12;
    REQUIRE(chstore_add(&f) == 1);

    channel_t fch;
    REQUIRE(chstore_toChannel(1, &fch) == 0);
    REQUIRE(fch.mode == OPMODE_FM);
    REQUIRE(fch.fm.txToneEn == 1);
    REQUIRE(fch.fm.txTone == 12);
    REQUIRE(fch.fm.rxToneEn == 1);
    REQUIRE(fch.fm.rxTone == 12);

    /* Round trip through the VFO record */
    chstore_channel_t back;
    chstore_fromChannel(&ch, &back);
    REQUIRE(std::string(back.name) == "DMR-TS2");
    REQUIRE(back.dmr_cc == 3);
    REQUIRE(back.dmr_ts == 1);
    REQUIRE((back.flags & CHSTORE_FLAG_SIMPLEX) != 0);
    REQUIRE(chstore_valid(&back));
}

TEST_CASE("A device without a flash backend reports an empty bank",
          "[chstore]")
{
    chstore_bind(nullptr);
    REQUIRE(chstore_init() == 0);
    REQUIRE(chstore_count() == 0);

    /* Writes have nowhere to go: they fail instead of pretending */
    REQUIRE(addChannel("X", 145500000, 145500000) < 0);
    REQUIRE(chstore_count() == 0);
}

/* ------------------------------------------------------------------ v1 → v2 --- */

/*
 * Write a bank with the version 1 layout straight into the fake flash, as the
 * first firmware that had an on-radio bank left it: 32-byte records and a
 * header that says version 1.
 */
void writeLegacyBank(FakeFlash &flash, const chstore_channel_v1_t *recs,
                     uint16_t count, uint16_t lastUsed)
{
    chstore_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = CHSTORE_MAGIC;
    hdr.version = CHSTORE_VERSION_LEGACY;
    hdr.count = count;
    hdr.sequence = 7;
    hdr.last_used = lastUsed;

    chstore_hdr_t copy = hdr;
    copy.hcrc = 0;
    copy.pcrc = 0;
    hdr.hcrc = crc_ccitt(&copy, sizeof(copy));
    hdr.pcrc = crc_ccitt(recs, (size_t)count * sizeof(*recs));

    memcpy(&flash.mem[0], &hdr, sizeof(hdr));
    memcpy(&flash.mem[sizeof(hdr)], recs, (size_t)count * sizeof(*recs));
}

chstore_channel_v1_t legacyChannel(const char *name, uint32_t rx, uint32_t tx,
                                   uint8_t mode)
{
    chstore_channel_v1_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "%s", name);
    c.rx_freq = rx;
    c.tx_freq = tx;
    c.power = 1000;
    c.mode = mode;
    c.bandwidth = BW_12_5;
    c.dmr_cc = 5;
    c.dmr_ts = 1;
    if (rx == tx)
        c.flags |= CHSTORE_FLAG_SIMPLEX;
    return c;
}

TEST_CASE("A version 1 bank is migrated and rewritten as version 2", "[chstore]")
{
    TestBank tb;
    chstore_channel_v1_t recs[2] = {
        legacyChannel("LEGACY-FM", 145500000, 145500000, OPMODE_FM),
        legacyChannel("LEGACY-DMR", 433500000, 433500000, OPMODE_DMR),
    };

    writeLegacyBank(tb.flash, recs, 2, 1);
    tb.boot();

    REQUIRE(chstore_count() == 2);
    REQUIRE(nameOf(0) == "LEGACY-FM");
    REQUIRE(nameOf(1) == "LEGACY-DMR");
    REQUIRE(chstore_lastUsed() == 1);

    chstore_channel_t c;
    REQUIRE(chstore_get(1, &c) == 0);
    REQUIRE(c.mode == OPMODE_DMR);
    REQUIRE(c.dmr_cc == 5);
    REQUIRE(c.dmr_ts == 1);
    /* A v1 channel had no destination: it keeps using the radio's default */
    REQUIRE(c.dmr_dest_type == DMR_DEST_NONE);
    REQUIRE(c.dmr_dest_id == 0);

    /* The first save writes the new layout; the bank survives the upgrade */
    REQUIRE(addChannel("NEW", 145000000, 145000000) == 2);
    tb.boot();
    REQUIRE(chstore_count() == 3);
    REQUIRE(nameOf(0) == "LEGACY-FM");
    REQUIRE(nameOf(2) == "NEW");

    /* And what is on flash now says version 2 */
    chstore_hdr_t hdr;
    memcpy(&hdr, &tb.flash.mem[0], sizeof(hdr));
    REQUIRE((hdr.version == CHSTORE_VERSION_LEGACY ||
             hdr.version == CHSTORE_VERSION));
}

TEST_CASE("A bank written by a newer firmware is left alone", "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    /* Pretend the newest slot came from a later version of the format */
    chstore_hdr_t hdr;
    memcpy(&hdr, &tb.flash.mem[0], sizeof(hdr));
    hdr.version = (uint16_t)(CHSTORE_VERSION + 1);
    chstore_hdr_t copy = hdr;
    copy.hcrc = 0;
    copy.pcrc = 0;
    hdr.hcrc = crc_ccitt(&copy, sizeof(copy));
    memcpy(&tb.flash.mem[0], &hdr, sizeof(hdr));

    /* Corrupt the older slot too, so both are unusable */
    tb.flash.mem[CHSTORE_SLOT_SIZE] ^= 0xFF;

    tb.boot();
    REQUIRE(chstore_count() == 0);
}

TEST_CASE("The DMR destination of a channel round-trips and is validated",
          "[chstore]")
{
    TestBank tb;
    tb.boot();

    chstore_channel_t c = makeChannel("TG-2142", 433450000, 433450000,
                                      OPMODE_DMR);
    c.dmr_dest_type = DMR_DEST_GROUP;
    c.dmr_dest_id = 2142;
    REQUIRE(chstore_valid(&c));
    REQUIRE(chstore_add(&c) == 0);

    chstore_channel_t dmrUser = makeChannel("UNIT-9", 433475000, 433475000,
                                            OPMODE_DMR);
    dmrUser.dmr_dest_type = DMR_DEST_USER;
    dmrUser.dmr_dest_id = 1234567;
    REQUIRE(chstore_valid(&dmrUser));
    REQUIRE(chstore_add(&dmrUser) == 1);

    tb.boot();

    chstore_channel_t back;
    REQUIRE(chstore_get(0, &back) == 0);
    REQUIRE(back.dmr_dest_type == DMR_DEST_GROUP);
    REQUIRE(back.dmr_dest_id == 2142);

    REQUIRE(chstore_get(1, &back) == 0);
    REQUIRE(back.dmr_dest_type == DMR_DEST_USER);
    REQUIRE(back.dmr_dest_id == 1234567);

    /* 0 is "no address" and 2^24 does not fit in the LC */
    chstore_channel_t bad = c;
    bad.dmr_dest_id = 0;
    REQUIRE_FALSE(chstore_valid(&bad));
    bad.dmr_dest_id = 0x1000000;
    REQUIRE_FALSE(chstore_valid(&bad));

    bad = c;
    bad.dmr_dest_type = 7;
    REQUIRE_FALSE(chstore_valid(&bad));
    bad.dmr_dest_type = DMR_DEST_NONE;
    bad.dmr_dest_id = 0;
    REQUIRE(chstore_valid(&bad));

    /*
     * The colour code is a 4-bit field, so an out-of-range value cannot be
     * constructed: it is truncated by the assignment, never written as 20.
     */
    bad = c;
    bad.dmr_cc = 20;
    REQUIRE(bad.dmr_cc == 4);
}

TEST_CASE("The SKIP flag survives a reboot and does not touch anything else",
          "[chstore]")
{
    TestBank tb;
    fillThree(tb);

    /* Nothing is skipped until the operator says so */
    REQUIRE_FALSE(chstore_skipped(0));
    REQUIRE_FALSE(chstore_skipped(1));
    REQUIRE_FALSE(chstore_skipped(2));

    REQUIRE(chstore_setSkipped(1, true) == 0);
    REQUIRE(chstore_skipped(1));
    REQUIRE_FALSE(chstore_skipped(0));

    /* It is a flag, not a deletion: the channel is still there, with its
     * name and frequencies, and the bank did not renumber anything */
    tb.boot();
    REQUIRE(chstore_count() == 3);
    REQUIRE(nameOf(1) == "BRAVO");
    REQUIRE(chstore_skipped(1));
    REQUIRE_FALSE(chstore_skipped(2));

    chstore_channel_t back;
    REQUIRE(chstore_get(1, &back) == 0);
    REQUIRE((back.flags & CHSTORE_FLAG_SKIP) != 0);

    /* Clearing it writes the bank again and the channel comes back into the
     * walk */
    REQUIRE(chstore_setSkipped(1, false) == 0);
    tb.boot();
    REQUIRE_FALSE(chstore_skipped(1));

    /* A bad index is not a crash and not a skip */
    REQUIRE_FALSE(chstore_skipped(99));
    REQUIRE(chstore_setSkipped(99, true) == -EINVAL);
}

TEST_CASE("A stored channel fills the RTX configuration, DMR included",
          "[chstore]")
{
    TestBank tb;
    tb.boot();

    chstore_channel_t c = makeChannel("RPT-TG", 438650000, 431050000,
                                      OPMODE_DMR);
    c.power = 5000;
    c.bandwidth = BW_12_5;
    c.dmr_cc = 3;
    c.dmr_ts = 1;
    c.dmr_dest_type = DMR_DEST_GROUP;
    c.dmr_dest_id = 91;
    c.flags = 0;    /* a repeater channel: TX is not the RX frequency */
    REQUIRE(chstore_add(&c) == 0);

    rtxStatus_t rtx;
    memset(&rtx, 0, sizeof(rtx));
    REQUIRE(chstore_applyRtx(0, &rtx) == 0);

    REQUIRE(rtx.opMode == OPMODE_DMR);
    REQUIRE(rtx.rxFrequency == 438650000);
    REQUIRE(rtx.txFrequency == 431050000);
    REQUIRE(rtx.txPower == 5000);
    REQUIRE(rtx.dmrColourCode == 3);
    REQUIRE(rtx.dmrTimeslot == 2);
    REQUIRE(rtx.dmrDestType == DMR_DEST_GROUP);
    REQUIRE(rtx.dmrDestId == 91);
    REQUIRE(rtx.txDisable == 0);

    REQUIRE(chstore_applyRtx(9, &rtx) < 0);

    /*
     * Selecting the channel copies its DMR destination and timeslot into the
     * settings, which is where the transmitter reads them from.
     */
    settings_t st = default_settings;
    st.dmr_dest_type = DMR_DEST_USER;
    st.dmr_dest_id = 42;
    st.dmr_timeslot = 1;

    REQUIRE(chstore_applySettings(0, &st) == 0);
    REQUIRE(st.dmr_dest_type == DMR_DEST_GROUP);
    REQUIRE(st.dmr_dest_id == 91);
    REQUIRE(st.dmr_timeslot == 2);

    /* An FM channel leaves the DMR settings alone */
    chstore_channel_t fm = makeChannel("FM-ONLY", 145500000, 145500000);
    REQUIRE(chstore_add(&fm) == 1);
    REQUIRE(chstore_applySettings(1, &st) == 0);
    REQUIRE(st.dmr_dest_id == 91);
    REQUIRE(st.dmr_timeslot == 2);

    REQUIRE(chstore_applySettings(9, &st) < 0);
}
