/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The internal backup (docs/BACKUP.md): the record, and the two rules that make
 * it worth having.
 *
 *  - The header: magic, version, size, directory and the CRC-16 of the whole
 *    record, checked against a mirror of the on-flash format (the format is
 *    data, so the test reads it the way a tool would).
 *  - A save never costs the previous record: two areas, the header written
 *    last, and a power cut simulated by a flash that stops accepting writes.
 *  - A restore verifies before it writes: a damaged record is refused with the
 *    stores exactly as they were.
 *  - And the round trip itself, store by store, over the six of them.
 *
 * The stores are the real ones (channel_store, contact_store, messaging) over
 * a RAM flash; the settings go through the host NVM, pointed at a scratch
 * directory so a test run does not touch the state of the desktop application.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/backup_image.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/crc.h"
#include "core/messaging.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/zones.h"
#include "interfaces/nvmem.h"
}

extern state_t state;

namespace
{

/** RAM flash with a write budget: a power cut is "the next write fails". */
struct FakeFlash
{
    std::vector<uint8_t> mem;
    int64_t budget;   /* -1: unlimited */
    bool dead;

    explicit FakeFlash(size_t size, uint8_t fill = 0xFF)
        : mem(size, fill), budget(-1), dead(false)
    {
    }

    void clearFault()
    {
        budget = -1;
        dead = false;
    }

    /** Accept this many more bytes of writes, then fail. */
    void cutAfter(int64_t bytes)
    {
        budget = bytes;
        dead = false;
    }

    int read(uint32_t off, void *buf, size_t len)
    {
        if ((size_t)off + len > mem.size())
            return -1;
        memcpy(buf, &mem[off], len);
        return 0;
    }

    int erase(uint32_t off, size_t len)
    {
        if (dead)
            return -1;
        if ((size_t)off + len > mem.size())
            return -1;
        memset(&mem[off], 0xFF, len);
        return 0;
    }

    int write(uint32_t off, const void *buf, size_t len)
    {
        if (dead)
            return -1;
        if ((size_t)off + len > mem.size())
            return -1;

        if (budget >= 0) {
            if (budget < (int64_t)len) {
                /* A partial write is what a power cut leaves behind */
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
};

int ffRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->read(off, buf, len);
}

int ffErase(void *ctx, uint32_t off, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->erase(off, len);
}

int ffWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->write(off, buf, len);
}

/*
 * The record header, as it is on flash (docs/BACKUP.md). Mirrored here on
 * purpose: the format is data and a test that reads it the way an external tool
 * would is what catches a silent change of layout.
 */
struct HdrDir
{
    uint8_t  id;
    uint8_t  flags;
    uint16_t length;
    uint32_t offset;
} __attribute__((packed));

struct Hdr
{
    uint32_t magic;
    uint16_t version;
    uint16_t hdrSize;
    uint32_t sequence;
    uint32_t totalSize;
    uint32_t crc;
    uint16_t count;
    int8_t   hour, minute, second, day, date, month;
    uint8_t  year;
    uint8_t  reserved;
    uint16_t pad;
    HdrDir   dir[6];
} __attribute__((packed));

static_assert(sizeof(Hdr) == 80, "the header of a record is 80 bytes");

/** Everything a backup carries, as a comparable snapshot. */
struct Snapshot
{
    uint8_t     brightness;
    char        callsign[16];
    uint32_t    vfoFreq;
    int         channels;
    char        firstChannel[16];
    int         contacts;
    char        firstContact[16];
    int         messages;
    bool        shiftInverted;
};

struct Env
{
    FakeFlash backup;
    FakeFlash bank;
    FakeFlash book;

    bkpPort_t bkpPort;
    chstorePort chPort;
    slot_ops_t cPort;

    Env()
        : backup(2U * BKP_AREA_SIZE), bank(0x4000), book(0x4000)
    {
        bkpPort.read = ffRead;
        bkpPort.erase = ffErase;
        bkpPort.write = ffWrite;
        bkpPort.ctx = &backup;
        bkpPort.size = (uint32_t)backup.mem.size();
        bkpPort.sector = 0x1000;

        chPort.read = ffRead;
        chPort.erase = ffErase;
        chPort.write = ffWrite;
        chPort.ctx = &bank;
        chPort.size = (uint32_t)bank.mem.size();
        chPort.sector = CHSTORE_SECTOR_SIZE;

        cPort.read = ffRead;
        cPort.erase = ffErase;
        cPort.write = ffWrite;
        cPort.ctx = &book;
        cPort.slot_size = 0x2000;
        cPort.sector = 0x1000;

        bkp_bind(&bkpPort);
        chstore_bind(&chPort);
        REQUIRE(chstore_init() == 0);
        cstore_bind(&cPort);
        cstore_init();

        messaging_init();
        messaging_clear();
    }

    /** The state a backup taken now would carry, as comparable fields. */
    Snapshot snapshot()
    {
        Snapshot s;
        memset(&s, 0, sizeof(s));

        s.brightness = state.settings.brightness;
        snprintf(s.callsign, sizeof(s.callsign), "%s",
                 state.settings.callsign);
        s.vfoFreq = state.channel.rx_frequency;
        s.channels = chstore_count();
        s.contacts = cstore_count();
        s.messages = messaging_outCount() + messaging_unread();
        s.shiftInverted = zones_shiftInverted();

        chstore_channel_t ch;
        if (s.channels > 0 && chstore_get(0, &ch) == 0)
            snprintf(s.firstChannel, sizeof(s.firstChannel), "%s", ch.name);

        cstore_contact_t ct;
        if (s.contacts > 0 && cstore_get(0, &ct) == 0)
            snprintf(s.firstContact, sizeof(s.firstContact), "%s", ct.call);

        return s;
    }

    /** A state that is different in all six stores from the one above. */
    void changeEverything()
    {
        state.settings.brightness = (uint8_t)(state.settings.brightness + 7);
        snprintf(state.settings.callsign, sizeof(state.settings.callsign),
                 "%s", "CHANGED");

        state.channel.rx_frequency = 433500000;
        state.channel.tx_frequency = 433500000;

        chstore_loadDefaults();
        chstore_channel_t ch;
        memset(&ch, 0, sizeof(ch));
        snprintf(ch.name, sizeof(ch.name), "%s", "OTHER");
        ch.rx_freq = 145500000;
        ch.tx_freq = 145500000;
        ch.power = 1000;
        ch.mode = OPMODE_FM;
        ch.bandwidth = BW_12_5;
        ch.flags = CHSTORE_FLAG_SIMPLEX;
        REQUIRE(chstore_add(&ch) == 0);

        cstore_bank_t *bk = cstore_bank();
        memset(bk, 0, sizeof(*bk));
        bk->count = 1;
        snprintf(bk->list[0].call, CSTORE_CALL_LEN, "%s", "OTHER");
        snprintf(bk->list[0].name, CSTORE_NAME_LEN, "%s", "OTHER");
        /* The book is written through the store, not behind its back */
        REQUIRE(cstore_import(bk, sizeof(*bk)) == 0);

        messaging_clear();
        messaging_send(MSG_TRANSPORT_APRS, 0, "other text", false, 1000);
        messaging_markRead();

        zones_setShiftInverted(!zones_shiftInverted());
    }

    /** The record of area `a`, as a header the test can poke at. */
    Hdr *header(uint8_t a)
    {
        return reinterpret_cast<Hdr *>(&backup.mem[(size_t)a * BKP_AREA_SIZE]);
    }

    /** Recompute the CRC of a record after the test has edited it. */
    void reseal(uint8_t a)
    {
        Hdr *h = header(a);
        Hdr copy = *h;
        copy.crc = 0;

        uint16_t crc = crc_ccitt(&copy, sizeof(copy));
        crc = crc_ccitt_update(crc,
                               &backup.mem[(size_t)a * BKP_AREA_SIZE +
                                           sizeof(Hdr)],
                               h->totalSize - sizeof(Hdr));
        h->crc = crc;
    }

    void corruptPayload(uint8_t a)
    {
        Hdr *h = header(a);
        backup.mem[(size_t)a * BKP_AREA_SIZE + sizeof(Hdr) + 3] ^= 0x5A;
    }
};

/** A scratch state directory for the settings NVM, per test binary run. */
struct ScratchState
{
    char dir[128];

    ScratchState()
    {
        const char *tmp = getenv("TMPDIR");
        snprintf(dir, sizeof(dir), "%s/bkp-nvm", (tmp != nullptr) ? tmp : "/tmp");
        setenv("XDG_STATE_HOME", dir, 1);
        nvm_init();
    }
};

ScratchState &scratch()
{
    static ScratchState s;
    return s;
}

void addChannel(const char *name, uint32_t rx)
{
    chstore_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    snprintf(ch.name, sizeof(ch.name), "%s", name);
    ch.rx_freq = rx;
    ch.tx_freq = rx;
    ch.power = 1000;
    ch.mode = OPMODE_FM;
    ch.bandwidth = BW_12_5;
    ch.flags = CHSTORE_FLAG_SIMPLEX;
    chstore_add(&ch);
}

void addContact(const char *call)
{
    cstore_contact_t ct;
    memset(&ct, 0, sizeof(ct));
    snprintf(ct.call, sizeof(ct.call), "%s", call);
    snprintf(ct.name, sizeof(ct.name), "%s", call);
    ct.mode = OPMODE_DMR;
    cstore_add(&ct);
}

/** The six stores in a known, distinct state. */
void stockEverything()
{
    state.settings = default_settings;
    state.settings.brightness = 42;
    snprintf(state.settings.callsign, sizeof(state.settings.callsign), "%s",
             "N0CALL-7");

    state.channel = cps_getDefaultChannel();
    state.channel.rx_frequency = 145600000;
    state.channel.tx_frequency = 145600000;
    state.channel.mode = OPMODE_FM;

    chstore_loadDefaults();
    addChannel("ALPHA", 145500000);
    addChannel("BRAVO", 433500000);

    cstore_bank_t *bk = cstore_bank();
    memset(bk, 0, sizeof(*bk));
    bk->count = 0;

    cstore_contact_t ct;
    memset(&ct, 0, sizeof(ct));
    snprintf(ct.call, sizeof(ct.call), "%s", "EA5ABC");
    snprintf(ct.name, sizeof(ct.name), "%s", "EA5ABC");
    ct.mode = OPMODE_DMR;
    cstore_add(&ct);

    messaging_clear();
    messaging_send(MSG_TRANSPORT_APRS, 0, "hola desde el aire", false, 1000);

    zones_setShiftInverted(false);
}

} /* namespace */

/* ============================== the record ============================== */

TEST_CASE("A backup writes a header the format documents", "[backup][record]")
{
    scratch();
    Env env;
    stockEverything();

    REQUIRE(bkp_backup() == BKP_OK);

    /* The header of the first area, read as an external tool would */
    const Hdr *h = env.header(0);

    CHECK(h->magic == 0x50554B42U);      /* "BKUP" */
    CHECK(h->version == BKP_VERSION);
    CHECK(h->hdrSize == sizeof(Hdr));
    CHECK(h->sequence == 1);
    CHECK(h->count == 6);
    CHECK(h->totalSize > sizeof(Hdr));
    CHECK(h->totalSize <= BKP_AREA_SIZE);

    /* The directory carries the six stores, each once, in order */
    for (uint8_t i = 0; i < 6; i++) {
        CHECK(h->dir[i].id == i);
        CHECK(h->dir[i].length > 0);
        CHECK(h->dir[i].offset >= sizeof(Hdr));
        CHECK(h->dir[i].offset + h->dir[i].length <= h->totalSize);
    }

    /* And the CRC is the one the bytes give */
    Hdr zeroed = *h;
    zeroed.crc = 0;
    uint16_t crc = crc_ccitt(&zeroed, sizeof(zeroed));
    crc = crc_ccitt_update(crc, &env.backup.mem[0] + sizeof(Hdr),
                           h->totalSize - sizeof(Hdr));
    CHECK(crc == h->crc);

    /* bkp_inspect() agrees with the bytes */
    bkpInfo_t info;
    REQUIRE(bkp_inspect(&info) == BKP_OK);
    CHECK(info.valid);
    CHECK(info.version == BKP_VERSION);
    CHECK(info.sequence == 1);
    CHECK(info.area == 0);
    CHECK(info.count == 6);
    CHECK(info.size == h->totalSize);
    for (uint8_t i = 0; i < 6; i++)
        CHECK(info.lengths[i] == h->dir[i].length);
}

TEST_CASE("A second backup goes to the other area and keeps the first",
          "[backup][record]")
{
    scratch();
    Env env;
    stockEverything();

    REQUIRE(bkp_backup() == BKP_OK);
    const uint32_t firstSize = env.header(0)->totalSize;
    std::vector<uint8_t> first(env.backup.mem.begin(),
                               env.backup.mem.begin() + BKP_AREA_SIZE);

    addChannel("CHARLIE", 446006250);
    REQUIRE(bkp_backup() == BKP_OK);

    bkpInfo_t info;
    REQUIRE(bkp_inspect(&info) == BKP_OK);
    CHECK(info.sequence == 2);
    CHECK(info.area == 1);

    /* The first record is still there, byte for byte */
    CHECK(std::memcmp(&env.backup.mem[0], first.data(), BKP_AREA_SIZE) == 0);
    CHECK(env.header(0)->sequence == 1);
    CHECK(env.header(0)->totalSize == firstSize);
}

TEST_CASE("A record cut in half costs the backup, never the previous one",
          "[backup][record][power]")
{
    scratch();
    Env env;
    stockEverything();

    REQUIRE(bkp_backup() == BKP_OK);
    const Snapshot good = env.snapshot();

    /* Everything changes, and the save that would have stored it dies in the
     * middle of the payload (header not written yet: the record is the last
     * thing that goes to flash) */
    env.changeEverything();
    env.backup.cutAfter(200);

    CHECK(bkp_backup() == BKP_ERR_IO);

    /* The half written area has no header, so the first record still wins */
    bkpInfo_t info;
    REQUIRE(bkp_inspect(&info) == BKP_OK);
    CHECK(info.sequence == 1);
    CHECK(info.area == 0);

    /* And it restores: the radio is where it was before the cut */
    env.backup.clearFault();
    REQUIRE(bkp_restore() == BKP_OK);

    const Snapshot back = env.snapshot();
    CHECK(back.brightness == good.brightness);
    CHECK(std::string(back.firstChannel) == good.firstChannel);
    CHECK(back.channels == good.channels);
    CHECK(back.contacts == good.contacts);
    CHECK(back.messages == good.messages);
}

/* ============================= the restore ============================== */

TEST_CASE("A damaged record is refused and the stores are not touched",
          "[backup][restore]")
{
    scratch();
    Env env;
    stockEverything();

    REQUIRE(bkp_backup() == BKP_OK);

    env.changeEverything();
    const Snapshot moved = env.snapshot();

    env.corruptPayload(0);

    bkpInfo_t info;
    CHECK(bkp_inspect(&info) == BKP_ERR_CORRUPT);
    CHECK(bkp_restore() == BKP_ERR_CORRUPT);

    /* Not one store moved: the restore wrote nothing at all */
    const Snapshot now = env.snapshot();
    CHECK(now.brightness == moved.brightness);
    CHECK(std::string(now.firstChannel) == "OTHER");
    CHECK(std::string(now.firstContact) == "OTHER");
    CHECK(now.channels == 1);
    CHECK(now.contacts == 1);
    CHECK(now.messages == 1);
    CHECK(now.shiftInverted == moved.shiftInverted);
    CHECK(std::string(bkp_lastResult()) == "CRC BAD");
}

TEST_CASE("A record of another version is refused, even with a good CRC",
          "[backup][restore][version]")
{
    scratch();
    Env env;
    stockEverything();

    REQUIRE(bkp_backup() == BKP_OK);
    env.changeEverything();
    const Snapshot moved = env.snapshot();

    env.header(0)->version = (uint16_t)(BKP_VERSION + 1);
    env.reseal(0);

    bkpInfo_t info;
    CHECK(bkp_inspect(&info) == BKP_ERR_VERSION);
    CHECK(bkp_restore() == BKP_ERR_VERSION);
    CHECK(std::string(bkp_lastResult()) == "VERSION");

    /* Nothing was written */
    CHECK(env.snapshot().channels == moved.channels);
    CHECK(std::string(env.snapshot().firstChannel) == "OTHER");
}

TEST_CASE("Both stores round trip through a backup and a restore",
          "[backup][restore][roundtrip]")
{
    scratch();
    Env env;
    stockEverything();

    const Snapshot before = env.snapshot();
    REQUIRE(before.channels == 2);
    REQUIRE(before.contacts == 1);
    REQUIRE(before.messages == 1);

    REQUIRE(bkp_backup() == BKP_OK);

    /* Everything changes: settings, VFO, bank, book, messages, zones */
    env.changeEverything();
    const Snapshot moved = env.snapshot();
    CHECK(moved.channels == 1);
    CHECK(std::string(moved.firstChannel) == "OTHER");
    CHECK(moved.brightness != before.brightness);
    CHECK(moved.shiftInverted != before.shiftInverted);
    CHECK(moved.vfoFreq != before.vfoFreq);

    REQUIRE(bkp_restore() == BKP_OK);

    /* And every one of the six comes back */
    const Snapshot after = env.snapshot();

    CHECK(after.brightness == before.brightness);              /* settings */
    CHECK(std::string(after.callsign) == before.callsign);     /* settings */
    CHECK(after.vfoFreq == before.vfoFreq);                    /* VFO      */
    CHECK(after.channels == before.channels);                  /* channels */
    CHECK(std::string(after.firstChannel) == "ALPHA");
    CHECK(after.contacts == before.contacts);                  /* contacts */
    CHECK(std::string(after.firstContact) == "EA5ABC");
    CHECK(after.messages == before.messages);                  /* messages */
    CHECK(after.shiftInverted == before.shiftInverted);        /* zones    */

    /* The settings the radio is using are the restored ones, and they were
     * written to the NVM as well, not only to RAM */
    settings_t reloaded;
    REQUIRE(nvm_readSettings(&reloaded) == 0);
    CHECK(reloaded.brightness == before.brightness);
    CHECK(std::string(reloaded.callsign) == before.callsign);

    /* Same for the VFO, which the restore writes on its own with a NULL
     * settings pointer: this is the check that the half write reached the
     * flash and not only state.channel */
    channel_t vfo;
    REQUIRE(nvm_readVfoChannelData(&vfo) == 0);
    CHECK(vfo.rx_frequency == before.vfoFreq);
}

TEST_CASE("The NVM port writes only the half it is given",
          "[backup][restore][nvmem]")
{
    scratch();

    /*
     * The contract the restore relies on (interfaces/nvmem.h): a NULL means
     * "leave that record alone". The host port used to hand the NULL down to
     * write(2), whose EFAULT came back as a positive errno that
     * nvm_writeSettingsAndVfo() -- testing "< 0" -- read as success: the VFO
     * was written, the settings write that had failed disappeared, and a
     * restore answered WRITE FAILED with the code the wrong way round.
     */
    settings_t s = default_settings;
    s.brightness = 33;
    s.sqlLevel = 4;
    REQUIRE(nvm_writeSettings(&s) == 0);

    channel_t ch = cps_getDefaultChannel();
    ch.rx_frequency = 444000000;
    ch.tx_frequency = 444000000;

    /* What applyVfo() does: settings NULL, VFO not */
    CHECK(nvm_writeSettingsAndVfo(NULL, &ch) == 0);

    settings_t back;
    REQUIRE(nvm_readSettings(&back) == 0);
    CHECK(back.brightness == 33);

    channel_t vfo;
    REQUIRE(nvm_readVfoChannelData(&vfo) == 0);
    CHECK(vfo.rx_frequency == 444000000);

    /* And the other half on its own: the settings, the VFO untouched */
    s.brightness = 44;
    CHECK(nvm_writeSettingsAndVfo(&s, NULL) == 0);

    REQUIRE(nvm_readSettings(&back) == 0);
    CHECK(back.brightness == 44);
    REQUIRE(nvm_readVfoChannelData(&vfo) == 0);
    CHECK(vfo.rx_frequency == 444000000);

    /* Nothing to write is not an error either */
    CHECK(nvm_writeSettingsAndVfo(NULL, NULL) == 0);
}

TEST_CASE("The zone plan of the build is part of the record",
          "[backup][restore][zones]")
{
    scratch();
    Env env;
    stockEverything();

    REQUIRE(bkp_backup() == BKP_OK);

    /* A backup taken with another plan is refused before anything is written:
     * the signature in the record is what tells the two plans apart */
    Hdr *h = env.header(0);
    HdrDir *zones = nullptr;
    for (uint8_t i = 0; i < h->count; i++)
        if (h->dir[i].id == BKP_E_ZONES)
            zones = &h->dir[i];
    REQUIRE(zones != nullptr);

    uint8_t *payload = &env.backup.mem[zones->offset + 4];
    payload[0] ^= 0xFF;
    env.reseal(0);

    CHECK(bkp_restore() == BKP_ERR_STORE);
    CHECK(std::string(bkp_lastResult()).substr(0, 12) == "STORE FAILED");
}

TEST_CASE("Without storage the screens get NO STORAGE and nothing crashes",
          "[backup][port]")
{
    scratch();
    Env env;

    bkp_bind(nullptr);

    CHECK(bkp_backup() == BKP_ERR_NO_PORT);
    CHECK(bkp_restore() == BKP_ERR_NO_PORT);

    bkpInfo_t info;
    CHECK(bkp_inspect(&info) == BKP_ERR_NO_PORT);
    CHECK_FALSE(info.valid);
    CHECK(std::string(bkp_lastResult()) == "NO STORAGE");

    bkp_bind(&env.bkpPort);
}

TEST_CASE("An empty part is NO BACKUP, not a damaged record", "[backup][record]")
{
    scratch();
    Env env;

    bkpInfo_t info;
    CHECK(bkp_inspect(&info) == BKP_ERR_NO_BACKUP);
    CHECK_FALSE(info.valid);
    CHECK(bkp_restore() == BKP_ERR_NO_BACKUP);
    CHECK(std::string(bkp_lastResult()) == "NO BACKUP");
}
