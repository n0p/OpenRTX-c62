/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests of the post-mortem recorder (core/blackbox.h, docs/BLACKBOX.md).
 *
 * Three things are tested, and they are the three that a hardware session
 * cannot check:
 *
 *  1. the stall detector, with a real thread that stops beating while the
 *     clock is driven by the test -- so "2 s" is a number and not a hope;
 *  2. the record: fields, order of the breadcrumbs, CRC, version, and what a
 *     corrupt or half-written record looks like to the reader;
 *  3. the boot-time read from flash, including the invalidation that keeps the
 *     record from being served twice.
 *
 * The platform hooks are the strong symbols at the top of this file, over the
 * weak defaults in core/blackbox.c: the same code the radio runs, with a fake
 * clock, a fake flash and no reboot.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "core/blackbox.h"
#include "core/crc.h"
}

/* ------------------------------------------------------------------------- */
/* Platform hooks                                                            */
/* ------------------------------------------------------------------------- */

namespace
{

std::atomic<uint32_t> fakeNow{ 0 };
uint32_t hangMask = 0;
bool hangCalled = false;

/* The last record the recorder tried to persist. */
std::vector<uint8_t> persisted;
int persistResult = 0;

/* Thread table handed to the recorder, and the radio fields around it. */
bbSnapshot_t snapshot;

} // namespace

extern "C" uint32_t bb_platformNowMs(void)
{
    return fakeNow.load();
}

extern "C" const void *bb_platformCurrentThread(void)
{
    /* The address of a thread_local object is a distinct, stable pointer per
     * thread: enough for bb_threadRole() and for a test that runs the beater
     * on a thread of its own. */
    static thread_local uint8_t id;
    return &id;
}

extern "C" void bb_platformSnapshot(bbSnapshot_t *s)
{
    *s = snapshot;
}

extern "C" int bb_platformPersist(const uint8_t *rec, size_t len)
{
    persisted.assign(rec, rec + len);
    return persistResult;
}

extern "C" void bb_platformReboot(void)
{
    /* Never in a test: reaching this would take the harness down. */
}

extern "C" void bb_platformHang(uint32_t mask, uint32_t nowMs)
{
    (void)nowMs;
    hangMask = mask;
    hangCalled = true;
}

/* ------------------------------------------------------------------------- */
/* Fake flash: NOR semantics (erase to 0xFF, writes can only clear bits)      */
/* ------------------------------------------------------------------------- */

namespace
{

struct FakeFlash {
    std::vector<uint8_t> mem;
    uint32_t erases = 0;
    uint32_t writes = 0;
    uint32_t reads = 0;

    FakeFlash() : mem(0x1000, 0xFF)
    {
    }
};

FakeFlash flash;

int flashRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    FakeFlash *f = static_cast<FakeFlash *>(ctx);
    if ((off + len) > f->mem.size())
        return -1;
    std::memcpy(buf, &f->mem[off], len);
    f->reads++;
    return 0;
}

int flashWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    FakeFlash *f = static_cast<FakeFlash *>(ctx);
    if ((off + len) > f->mem.size())
        return -1;

    const uint8_t *p = static_cast<const uint8_t *>(buf);
    for (size_t i = 0; i < len; i++)
        f->mem[off + i] &= p[i]; /* 1 -> 0 only, as the hardware does */

    f->writes++;
    return 0;
}

/* What the radio's flash layer does for a whole record: erase, then write. */
int flashStore(const uint8_t *rec, size_t len)
{
    FakeFlash *f = &flash;
    std::memset(&f->mem[0], 0xFF, 0x1000);
    f->erases++;
    return flashWrite(f, 0, rec, len);
}

bbFlashIo_t flashIo()
{
    bbFlashIo_t io;
    io.read = flashRead;
    io.write = flashWrite;
    io.ctx = &flash;
    return io;
}

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

} // namespace

/* ------------------------------------------------------------------------- */
/* 1. The detector                                                           */
/* ------------------------------------------------------------------------- */

TEST_CASE("A thread that stops beating is a stall after two seconds", "[bb]")
{
    hangCalled = false;
    hangMask = 0;
    persisted.clear();
    persistResult = 0;

    /* A clean recorder, so only this test's beats are watched. */
    bb_suspend();
    bb_clear();

    snapshot = bbSnapshot_t{};
    snapshot.rtxMode = 1; /* FM */

    /* A real thread, beating, then done: this is the shape of a UI thread that
     * parks behind a lock in the middle of a mode change. */
    std::thread beater([] {
        for (int i = 0; i < 10; i++) {
            bb_beat(BB_BEAT_UI);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    beater.join();

    /* Prime the detector at t0.  Nothing may fire before BB_STALL_MS. */
    fakeNow = 1000;
    bb_resume();
    REQUIRE(bb_supervise(1000) == false);
    REQUIRE(hangCalled == false);

    REQUIRE(bb_supervise(1000 + BB_STALL_MS - 1) == false);
    REQUIRE(hangCalled == false);

    /* Exactly at the threshold: the thread has not moved since t0. */
    REQUIRE(bb_supervise(1000 + BB_STALL_MS) == true);
    REQUIRE(hangCalled == true);
    REQUIRE(hangMask == (1U << BB_BEAT_UI));

    /* The other watched threads have no beat at all: a thread this build does
     * not have must not be reported as stalled, or every radio without a PC
     * protocol would reboot at boot. */
    REQUIRE((hangMask & (1U << BB_BEAT_RTX)) == 0);
    REQUIRE((hangMask & (1U << BB_BEAT_PROTO)) == 0);
    REQUIRE((hangMask & (1U << BB_BEAT_MAIN)) == 0);
    REQUIRE((hangMask & (1U << BB_BEAT_KBD)) == 0);

    /* And a hang is reported once: the platform has been handed a mask, it
     * writes the record and resets, and the supervisor must not come back every
     * pass to erase and rewrite that record (tests/unit/blackbox_hang.cpp
     * checks the same thing through the default hook, which is what runs on
     * the radio). */
    hangCalled = false;
    REQUIRE(bb_supervise(1000 + BB_STALL_MS + 100) == false);
    REQUIRE(hangCalled == false);
}

TEST_CASE("The keypad scanner has a beat, and a name", "[bb]")
{
    hangCalled = false;
    hangMask = 0;

    bb_suspend();
    bb_clear();

    /* The keypad is its own thread on the C62 (keyboard_c62.c).  It is watched
     * like the rest: a matrix that stops being scanned is a radio where no key
     * does anything, whatever the screen and the receiver are doing. */
    bb_beat(BB_BEAT_KBD);

    fakeNow = 30000;
    bb_resume();
    REQUIRE(bb_supervise(30000) == false);

    REQUIRE(bb_supervise(30000 + BB_STALL_MS) == true);
    REQUIRE(hangMask == (1U << BB_BEAT_KBD));
    REQUIRE(std::string(bb_beatName(BB_BEAT_KBD)) == "kbd");
}

TEST_CASE("A thread that keeps beating is not a stall", "[bb]")
{
    hangCalled = false;
    hangMask = 0;

    bb_suspend();
    bb_clear();

    std::atomic<bool> stop{ false };
    std::thread beater([&stop] {
        while (!stop.load()) {
            bb_beat(BB_BEAT_RTX);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    fakeNow = 20000;
    bb_resume();
    REQUIRE(bb_supervise(20000) == false);

    /* Four passes, 600 ms apart: the counter moved between every one. */
    for (uint32_t i = 1; i <= 4; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(bb_supervise(20000 + i * 600) == false);
    }

    stop = true;
    beater.join();

    REQUIRE(hangCalled == false);

    /* And once it stops, the 2 s clock starts again from the last pass. */
    REQUIRE(bb_supervise(20000 + 2400 + BB_STALL_MS - 1) == false);
    REQUIRE(bb_supervise(20000 + 2400 + BB_STALL_MS) == true);
    REQUIRE(hangMask == (1U << BB_BEAT_RTX));
}

TEST_CASE("A suspended supervisor does not fire", "[bb]")
{
    hangCalled = false;

    bb_suspend();
    bb_clear();

    /* One beat, then it stops: the freeze this test is about. */
    bb_beat(BB_BEAT_MAIN);

    fakeNow = 40000;
    bb_resume();
    bb_supervise(40000);
    bb_suspend();

    REQUIRE(bb_supervise(40000 + 10 * BB_STALL_MS) == false);
    REQUIRE(hangCalled == false);

    /* Resume re-primes: the long gap while suspended is not a hang. */
    bb_resume();
    REQUIRE(bb_supervise(40000 + 20 * BB_STALL_MS) == false);
    REQUIRE(bb_supervise(40000 + 21 * BB_STALL_MS) == true);
    REQUIRE(hangMask == (1U << BB_BEAT_MAIN));
}

/* ------------------------------------------------------------------------- */
/* 2. The record                                                             */
/* ------------------------------------------------------------------------- */

namespace
{

void fillSnapshot()
{
    snapshot = bbSnapshot_t{};
    snapshot.rtxMode = 10;    /* AM */
    snapshot.uiMode = 10;
    snapshot.rtxFlags = 0x01; /* receiving */
    snapshot.rxFreq = 145500000;
    snapshot.txFreq = 145500000;
    snapshot.resetCause = 0;
    snapshot.threadCount = 3;

    std::memcpy(snapshot.threads[0].name, "ui", 3);
    snapshot.threads[0].prio = 5;
    snapshot.threads[0].state = 0;
    snapshot.threads[0].stackFree = 512;
    snapshot.threads[0].stackSize = 2048;
    snapshot.threads[0].role = BB_BEAT_UI + 1;
    snapshot.threads[0].pc = 0x1801a2b4;
    snapshot.threads[0].lr = 0x1801a2b8;
    snapshot.threads[0].sp = 0x200c1234;

    std::memcpy(snapshot.threads[1].name, "rtx", 4);
    snapshot.threads[1].prio = 4;
    snapshot.threads[1].state = 1; /* pended */
    snapshot.threads[1].stackFree = 9000;
    snapshot.threads[1].stackSize = 9216;
    snapshot.threads[1].role = BB_BEAT_RTX + 1;
    snapshot.threads[1].pc = 0;
    snapshot.threads[1].lr = 0;
    snapshot.threads[1].sp = 0x200c2000;

    std::memcpy(snapshot.threads[2].name, "pcproto", 8);
    snapshot.threads[2].prio = 6;
    snapshot.threads[2].state = 0;
    snapshot.threads[2].stackFree = 3000;
    snapshot.threads[2].stackSize = 4096;
    snapshot.threads[2].role = BB_BEAT_PROTO + 1;
    snapshot.threads[2].pc = 0;
    snapshot.threads[2].lr = 0;
    snapshot.threads[2].sp = 0x200c3000;
}

} // namespace

TEST_CASE("The record carries the header, the threads and the crumbs", "[bb]")
{
    fillSnapshot();

    /* A clean recorder: three marks, three ticks, nothing else in the ring. */
    bb_suspend();
    bb_clear();

    bb_tick(7000);
    bb_mark(BB_M_MODE_ENTER);
    bb_tick(7010);
    bb_mark(BB_M_RADIO_CONF);
    bb_tick(7020);
    bb_mark(BB_M_AUDIO_OPEN);

    const uint32_t marksBefore = bb_markCount();
    REQUIRE(marksBefore == 3);

    uint8_t rec[BB_RECORD_SIZE];
    int n = bb_recordBuild(rec, sizeof(rec), 12345, BB_REASON_STALL,
                           (1U << BB_BEAT_RTX));

    REQUIRE(n == BB_RECORD_SIZE);
    REQUIRE(bb_recordValid(rec, sizeof(rec)));

    REQUIRE(rd32(&rec[BB_OFF_MAGIC]) == BB_RECORD_MAGIC);
    REQUIRE(rec[BB_OFF_VERSION] == (BB_RECORD_VERSION & 0xFF));
    REQUIRE(rec[BB_OFF_HDRSIZE] == BB_HDR_SIZE);
    REQUIRE(rd32(&rec[BB_OFF_VALID]) == BB_VALID_SIGNATURE);
    REQUIRE(rd32(&rec[BB_OFF_UPTIME]) == 12345);
    REQUIRE(rec[BB_OFF_REASON] == BB_REASON_STALL);
    REQUIRE(rd32(&rec[BB_OFF_DETAIL]) == (1U << BB_BEAT_RTX));
    REQUIRE(rec[BB_OFF_RTXMODE] == 10);
    REQUIRE(rec[BB_OFF_UIMODE] == 10);
    REQUIRE(rec[BB_OFF_RTXFLAGS] == 1);
    REQUIRE(rd32(&rec[BB_OFF_RXFREQ]) == 145500000);
    REQUIRE(rd32(&rec[BB_OFF_CRUMBSEQ]) == marksBefore);

    /* Threads */
    REQUIRE(rec[BB_OFF_THREADS] == 3);
    REQUIRE(rec[BB_OFF_THREADSZ] == BB_THREAD_REC_SIZE);

    const uint8_t *t0 = &rec[BB_OFF_THREADS_BASE];
    REQUIRE(std::strcmp(reinterpret_cast<const char *>(&t0[BB_TR_NAME]), "ui")
            == 0);
    REQUIRE(static_cast<int8_t>(t0[BB_TR_PRIO]) == 5);
    REQUIRE(t0[BB_TR_STATE] == 0);
    REQUIRE((t0[BB_TR_FREE] | (t0[BB_TR_FREE + 1] << 8)) == 512);
    REQUIRE((t0[BB_TR_SIZE] | (t0[BB_TR_SIZE + 1] << 8)) == 2048);
    REQUIRE((t0[BB_TR_ROLE] | (t0[BB_TR_ROLE + 1] << 8)) == BB_BEAT_UI + 1);
    REQUIRE(rd32(&t0[BB_TR_PC]) == 0x1801a2b4);
    REQUIRE(rd32(&t0[BB_TR_LR]) == 0x1801a2b8);
    REQUIRE(rd32(&t0[BB_TR_SP]) == 0x200c1234);

    const uint8_t *t1 = &rec[BB_OFF_THREADS_BASE + BB_THREAD_REC_SIZE];
    REQUIRE(std::strcmp(reinterpret_cast<const char *>(&t1[BB_TR_NAME]), "rtx")
            == 0);
    REQUIRE(static_cast<int8_t>(t1[BB_TR_PRIO]) == 4);
    REQUIRE(t1[BB_TR_STATE] == 1);

    /* Breadcrumbs: the last three, oldest first, each with its tick */
    const uint16_t nCrumbs = rec[BB_OFF_CRUMBS];
    REQUIRE(nCrumbs == 3);

    const uint8_t *c = &rec[BB_OFF_CRUMBS_BASE + (nCrumbs - 3) * 4];
    REQUIRE(rd32(&c[0]) == (((uint32_t)BB_M_MODE_ENTER << 24) | 7000));
    REQUIRE(rd32(&c[4]) == (((uint32_t)BB_M_RADIO_CONF << 24) | 7010));
    REQUIRE(rd32(&c[8]) == (((uint32_t)BB_M_AUDIO_OPEN << 24) | 7020));

    /* The CRC covers everything before it */
    REQUIRE(rd32(&rec[BB_OFF_CRC]) >> 16 != 0xFFFFFFFFU);
    REQUIRE((uint16_t)rd32(&rec[BB_OFF_CRC]) == crc_ccitt(rec, BB_OFF_CRC));
}

TEST_CASE("A corrupt or foreign record is refused", "[bb]")
{
    fillSnapshot();

    uint8_t rec[BB_RECORD_SIZE];
    REQUIRE(bb_recordBuild(rec, sizeof(rec), 100, BB_REASON_FATAL, 4)
            == BB_RECORD_SIZE);
    REQUIRE(bb_recordValid(rec, sizeof(rec)));

    /* A flipped bit anywhere in the payload is caught by the CRC. */
    for (uint32_t off : { BB_OFF_UPTIME, BB_OFF_THREADS_BASE + BB_TR_PC,
                          BB_OFF_CRUMBS_BASE, BB_OFF_REASON }) {
        uint8_t copy[BB_RECORD_SIZE];
        std::memcpy(copy, rec, sizeof(copy));
        copy[off] ^= 0x40;
        REQUIRE(bb_recordValid(copy, sizeof(copy)) == false);
    }

    /* A flipped bit in the CRC itself, same thing. */
    {
        uint8_t copy[BB_RECORD_SIZE];
        std::memcpy(copy, rec, sizeof(copy));
        copy[BB_OFF_CRC] ^= 0x01;
        REQUIRE(bb_recordValid(copy, sizeof(copy)) == false);
    }

    /* Wrong magic (an erased sector reads 0xFF everywhere) */
    {
        uint8_t copy[BB_RECORD_SIZE];
        std::memset(copy, 0xFF, sizeof(copy));
        REQUIRE(bb_recordValid(copy, sizeof(copy)) == false);
    }

    /* Wrong version: a newer firmware's record is not read as if it were
     * this one's. */
    {
        uint8_t copy[BB_RECORD_SIZE];
        std::memcpy(copy, rec, sizeof(copy));
        copy[BB_OFF_VERSION] = BB_RECORD_VERSION + 1;
        REQUIRE(bb_recordValid(copy, sizeof(copy)) == false);
    }

    /* Truncated */
    REQUIRE(bb_recordValid(rec, BB_RECORD_SIZE - 1) == false);

    /* Thread and crumb counts out of range */
    {
        uint8_t copy[BB_RECORD_SIZE];
        std::memcpy(copy, rec, sizeof(copy));
        copy[BB_OFF_THREADS] = BB_MAX_THREADS + 1;
        REQUIRE(bb_recordValid(copy, sizeof(copy)) == false);
    }
}

TEST_CASE("A capture with no flash still says what happened", "[bb]")
{
    fillSnapshot();

    persisted.clear();
    persistResult = -1; /* the write failed */

    REQUIRE(bb_capture(BB_REASON_MANUAL, 0, 500) == false);
    /* The record was built and handed to the platform even so: this is what a
     * bench session sees when the sector is worn out. */
    REQUIRE(persisted.size() == BB_RECORD_SIZE);
    REQUIRE(bb_recordValid(persisted.data(), persisted.size()));

    persistResult = 0;
    REQUIRE(bb_capture(BB_REASON_FATAL, 3, 500) == true);
    REQUIRE(persisted.size() == BB_RECORD_SIZE);
    REQUIRE(persisted[BB_OFF_REASON] == BB_REASON_FATAL);
    REQUIRE(rd32(&persisted[BB_OFF_DETAIL]) == 3);
}

/* ------------------------------------------------------------------------- */
/* 3. Boot-time read                                                         */
/* ------------------------------------------------------------------------- */

TEST_CASE("The record is read at boot and invalidated once", "[bb]")
{
    fillSnapshot();

    bbFlashIo_t io = flashIo();

    /* Empty flash: no record, and the reader says so without touching it. */
    flash = FakeFlash{};
    uint8_t rec[BB_RECORD_SIZE];
    REQUIRE(bb_recordLoad(&io, rec, sizeof(rec)) != 0);
    REQUIRE(flash.erases == 0);
    REQUIRE(flash.writes == 0);

    /* A crash writes the record and resets. */
    bb_tick(9000);
    bb_mark(BB_M_MODE_ENTER);
    REQUIRE(bb_capture(BB_REASON_STALL, (1U << BB_BEAT_UI), 9100) == true);
    flashStore(persisted.data(), persisted.size());

    /* Next boot: it is there, valid, and it is the one that was written. */
    REQUIRE(bb_recordLoad(&io, rec, sizeof(rec)) == 0);
    REQUIRE(bb_recordValid(rec, sizeof(rec)));
    REQUIRE(rd32(&rec[BB_OFF_UPTIME]) == 9100);
    REQUIRE(rec[BB_OFF_REASON] == BB_REASON_STALL);
    REQUIRE(rd32(&rec[BB_OFF_DETAIL]) == (1U << BB_BEAT_UI));

    /* Invalidate: four bytes, no erase, and the sector is still readable. */
    const uint32_t erasesBefore = flash.erases;
    REQUIRE(bb_recordInvalidate(&io) == 0);
    REQUIRE(flash.erases == erasesBefore);
    REQUIRE(rd32(&flash.mem[BB_OFF_VALID]) == 0);
    /* Everything else is untouched, so a forensic read of the sector still
     * finds the record; only the validity word says "already served". */
    REQUIRE(flash.mem[BB_OFF_UPTIME] == (9100 & 0xFF));

    /* Second boot: no record is served twice. */
    REQUIRE(bb_recordLoad(&io, rec, sizeof(rec)) != 0);

    /* And the sector is ready for the next crash without an erase in between:
     * the write path erases it, which is what the radio's layer does. */
    REQUIRE(bb_capture(BB_REASON_MANUAL, 0, 9200) == true);
    flashStore(persisted.data(), persisted.size());
    REQUIRE(bb_recordLoad(&io, rec, sizeof(rec)) == 0);
    REQUIRE(rd32(&rec[BB_OFF_UPTIME]) == 9200);
}

TEST_CASE("A write cut in half leaves an invalid record, not a wrong one",
          "[bb]")
{
    fillSnapshot();
    bbFlashIo_t io = flashIo();

    flash = FakeFlash{};
    REQUIRE(bb_capture(BB_REASON_STALL, 1, 5000) == true);

    /* Half of the record made it: erase, then 200 bytes. */
    std::memset(&flash.mem[0], 0xFF, 0x1000);
    flashWrite(&flash, 0, persisted.data(), 200);

    uint8_t rec[BB_RECORD_SIZE];
    REQUIRE(bb_recordLoad(&io, rec, sizeof(rec)) != 0);
}

/* ------------------------------------------------------------------------- */
/* 4. The text the operator and the console read                             */
/* ------------------------------------------------------------------------- */

TEST_CASE("The dump is readable and names what it can", "[bb]")
{
    fillSnapshot();

    bb_suspend();
    bb_clear();

    bb_tick(4200);
    bb_mark(BB_M_MODE_ENTER);
    bb_tick(4300);
    bb_mark(BB_M_RADIO_CONF);
    bb_tick(4350);
    bb_mark(BB_M_DISP_DROP);
    bb_tick(4400);
    bb_mark(BB_M_HANG);

    uint8_t rec[BB_RECORD_SIZE];
    REQUIRE(bb_recordBuild(rec, sizeof(rec), 4500, BB_REASON_STALL,
                           (1U << BB_BEAT_UI) | (1U << BB_BEAT_KBD))
            == BB_RECORD_SIZE);

    std::vector<std::string> lines;
    bb_textDump(
        rec, sizeof(rec),
        [](void *ctx, const char *line) {
            static_cast<std::vector<std::string> *>(ctx)->push_back(line);
        },
        &lines);

    REQUIRE(lines.size() > 8);

    auto find = [&lines](const char *needle) {
        for (const auto &l : lines) {
            if (l.find(needle) != std::string::npos)
                return true;
        }
        return false;
    };

    REQUIRE(find("C62 post-mortem: record v1"));
    REQUIRE(find("crc ok"));
    REQUIRE(find("reason: STALL"));
    REQUIRE(find("[ui kbd]"));
    REQUIRE(find("uptime: 4500 ms"));
    REQUIRE(find("rtx=AM(10)"));
    REQUIRE(find("threads: 3"));
    REQUIRE(find("ui"));
    REQUIRE(find("pcproto"));
    REQUIRE(find("0x1801a2b4"));
    REQUIRE(find("breadcrumbs:"));
    REQUIRE(find("MODE_ENTER"));
    REQUIRE(find("RADIO_CONF"));
    REQUIRE(find("DISP_DROP"));
    REQUIRE(find("HANG"));

    /* Every line has to fit the console and the log ring it travels in. */
    for (const auto &l : lines)
        REQUIRE(l.size() < 120);
}

TEST_CASE("A record with no crumbs and no threads still renders", "[bb]")
{
    snapshot = bbSnapshot_t{};
    snapshot.rtxMode = 0;

    bb_suspend();
    bb_clear();

    uint8_t rec[BB_RECORD_SIZE];
    REQUIRE(bb_recordBuild(rec, sizeof(rec), 10, BB_REASON_MANUAL, 0)
            == BB_RECORD_SIZE);

    std::vector<std::string> lines;
    bb_textDump(
        rec, sizeof(rec),
        [](void *ctx, const char *line) {
            static_cast<std::vector<std::string> *>(ctx)->push_back(line);
        },
        &lines);

    REQUIRE(!lines.empty());
    REQUIRE(lines[1].find("MANUAL") != std::string::npos);
}
