/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host test of the two-slot record container and of the two stores built on
 * it (contact book, message store): round trip, newest-slot wins, corrupt
 * newest slot falls back, and a power cut in the middle of a save costs the
 * save but never the previous bank.
 */

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <vector>

extern "C" {
#include "core/slot_store.h"
#include "core/contact_store.h"
#include "core/msg_store.h"
#include "core/msg_service.h"
}

static_assert(sizeof(msg_rec_t) == 191, "message record size");
static_assert(sizeof(msg_bank_t) + SLOT_HDR_SIZE <= 0x2000,
              "the message bank has to fit one slot");
static_assert(sizeof(cstore_contact_t) == CSTORE_RECORD_SIZE,
              "contact record size");
static_assert(sizeof(cstore_bank_t) + SLOT_HDR_SIZE <= 0x2000,
              "the contact bank has to fit one slot");

namespace {

class FakeFlash
{
public:
    explicit FakeFlash(uint32_t size = 4 * 0x2000)
        : mem(size, 0xFF), budget(-1), dead(false)
    {}

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

    int read(uint32_t off, void *buf, size_t len)
    {
        if (dead || off + len > mem.size())
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

struct TestPort
{
    FakeFlash flash;
    slot_ops_t ops;

    TestPort()
    {
        ops.read = ffRead;
        ops.erase = ffErase;
        ops.write = ffWrite;
        ops.ctx = &flash;
        ops.slot_size = 0x2000;
        ops.sector = 0x1000;
    }
};

struct Payload
{
    uint32_t a;
    uint16_t b;
    char text[32];
};

} // namespace

TEST_CASE("Slot store round trips and keeps the newest copy", "[store]")
{
    TestPort p;
    Payload out = {};
    slot_store_t st;

    /* Blank: nothing to load, and the first save goes to slot 0 */
    REQUIRE(slot_store_load(&st, &p.ops, 0x12345678, 1, &out, sizeof(out))
            == -ENOENT);
    REQUIRE(slot_store_blank(&st));

    Payload v1 = { 1, 2, "one" };
    REQUIRE(slot_store_save(&st, &v1, sizeof(v1)) == 0);
    REQUIRE(st.slot == 0);
    REQUIRE(st.seq == 1);

    Payload v2 = { 3, 4, "two" };
    REQUIRE(slot_store_save(&st, &v2, sizeof(v2)) == 0);
    REQUIRE(st.slot == 1);

    slot_store_t st2;
    Payload got;
    REQUIRE(slot_store_load(&st2, &p.ops, 0x12345678, 1, &got, sizeof(got)) == 0);
    REQUIRE(memcmp(&got, &v2, sizeof(v2)) == 0);
    REQUIRE(st2.seq == 2);

    /* A wrong magic is not a record: the loader reports blank */
    slot_store_t st3;
    REQUIRE(slot_store_load(&st3, &p.ops, 0xAABBCCDD, 1, &got, sizeof(got))
            == -ENOENT);
}

TEST_CASE("Slot store falls back when the newest slot is damaged", "[store]")
{
    TestPort p;
    slot_store_t st;
    Payload got;

    Payload v1 = { 7, 8, "good" };
    REQUIRE(slot_store_load(&st, &p.ops, 0x1, 1, &got, sizeof(got)) == -ENOENT);
    REQUIRE(slot_store_save(&st, &v1, sizeof(v1)) == 0);

    Payload v2 = { 9, 10, "newer" };
    REQUIRE(slot_store_save(&st, &v2, sizeof(v2)) == 0);

    /* Corrupt one payload byte of the newest slot (slot 1) */
    p.flash.mem[0x2000 + SLOT_HDR_SIZE + 3] ^= 0xFF;

    slot_store_t st2;
    REQUIRE(slot_store_load(&st2, &p.ops, 0x1, 1, &got, sizeof(got)) == 0);
    REQUIRE(memcmp(&got, &v1, sizeof(v1)) == 0);
    REQUIRE(st2.slot == 0);
}

TEST_CASE("Slot store survives a power cut during a save", "[store]")
{
    TestPort p;
    slot_store_t st;
    Payload got;

    Payload v1 = { 42, 1, "safe" };
    REQUIRE(slot_store_load(&st, &p.ops, 0x2, 1, &got, sizeof(got)) == -ENOENT);
    REQUIRE(slot_store_save(&st, &v1, sizeof(v1)) == 0);

    /* Power dies while erasing the second slot */
    p.flash.cutAfter(16);
    Payload v2 = { 43, 2, "lost" };
    REQUIRE(slot_store_save(&st, &v2, sizeof(v2)) != 0);

    p.flash.clearFault();
    slot_store_t st2;
    REQUIRE(slot_store_load(&st2, &p.ops, 0x2, 1, &got, sizeof(got)) == 0);
    REQUIRE(memcmp(&got, &v1, sizeof(v1)) == 0);

    /* And the store still accepts the next save */
    Payload v3 = { 44, 3, "again" };
    REQUIRE(slot_store_save(&st2, &v3, sizeof(v3)) == 0);
    slot_store_t st3;
    REQUIRE(slot_store_load(&st3, &p.ops, 0x2, 1, &got, sizeof(got)) == 0);
    REQUIRE(memcmp(&got, &v3, sizeof(v3)) == 0);
}

TEST_CASE("Contact book add, edit, delete and reload", "[store][contacts]")
{
    TestPort p;
    cstore_bind(&p.ops);
    cstore_forget();
    REQUIRE(cstore_init() == 0);
    REQUIRE(cstore_count() == 0);

    cstore_contact_t c = {};
    snprintf(c.call, sizeof(c.call), "EA5ABC-7");
    snprintf(c.name, sizeof(c.name), "Pepe");
    c.mode = 0; /* FM */
    int idx = cstore_add(&c);
    REQUIRE(idx == 0);
    REQUIRE(cstore_count() == 1);

    cstore_contact_t got;
    REQUIRE(cstore_get(0, &got) == 0);
    REQUIRE(std::strcmp(got.call, "EA5ABC-7") == 0);
    REQUIRE(std::strcmp(got.name, "Pepe") == 0);

    /* Empty callsign is refused, and nothing is written */
    cstore_contact_t bad = {};
    REQUIRE(cstore_valid(&bad) == false);
    REQUIRE(cstore_add(&bad) < 0);
    REQUIRE(cstore_count() == 1);

    /* Edit in place */
    snprintf(got.name, sizeof(got.name), "Jose");
    REQUIRE(cstore_set(0, &got) == 0);
    REQUIRE(cstore_get(0, &got) == 0);
    REQUIRE(std::strcmp(got.name, "Jose") == 0);

    /* Lookup */
    REQUIRE(cstore_findCall("EA5ABC-7") == 0);
    REQUIRE(cstore_findCall("NOPE") == CSTORE_NONE);

    /* Simulated power cycle: the same flash loads the same book */
    cstore_forget();
    REQUIRE(cstore_init() == 0);
    REQUIRE(cstore_count() == 1);
    REQUIRE(cstore_get(0, &got) == 0);
    REQUIRE(std::strcmp(got.name, "Jose") == 0);

    /* Delete */
    REQUIRE(cstore_delete(0) == 0);
    REQUIRE(cstore_count() == 0);
    cstore_forget();
    REQUIRE(cstore_init() == 0);
    REQUIRE(cstore_count() == 0);
}

TEST_CASE("Message store keeps inbox, sent and draft", "[store][messages]")
{
    TestPort p;
    msg_bind(&p.ops);
    msg_forget();
    REQUIRE(msg_init() == 0);
    REQUIRE(msg_inbox_count() == 0);
    REQUIRE(msg_unread() == 0);

    msg_rec_t *out = msg_addOut(MSG_TP_APRS, "EA5ABC-7", "hola", 1000);
    REQUIRE(out != NULL);
    REQUIRE(out->status == MSG_ST_QUEUED);
    REQUIRE(msg_sent_count() == 1);

    msg_rec_t *in = msg_addIn(MSG_TP_APRS, "EA1XYZ", "que tal", 2000);
    REQUIRE(in != NULL);
    REQUIRE(msg_inbox_count() == 1);
    REQUIRE(msg_unread() == 1);

    /* Unread survives a power cycle, until it is read */
    msg_forget();
    REQUIRE(msg_init() == 0);
    REQUIRE(msg_inbox_count() == 1);
    REQUIRE(msg_unread() == 1);

    msg_markAllRead();
    msg_forget();
    REQUIRE(msg_init() == 0);
    REQUIRE(msg_unread() == 0);
    REQUIRE(msg_inbox_count() == 1);

    /* Draft */
    msg_rec_t *d = msg_draft();
    d->tp = MSG_TP_APRS;
    snprintf(d->peer, sizeof(d->peer), "EA5ABC-7");
    snprintf(d->text, sizeof(d->text), "sin enviar");
    REQUIRE(msg_save() == 0);
    msg_forget();
    REQUIRE(msg_init() == 0);
    REQUIRE(std::strcmp(msg_draft()->text, "sin enviar") == 0);

    /* Sequence numbers are unique and grow */
    uint16_t s1 = msg_inbox_at(0)->seq;
    msg_rec_t *second = msg_addIn(MSG_TP_APRS, "EA2DEF", "otro", 3000);
    REQUIRE(second != NULL);
    REQUIRE(second->seq != s1);
}

TEST_CASE("Message service refuses what it cannot send", "[messages]")
{
    TestPort p;
    msg_bind(&p.ops);
    msg_forget();
    REQUIRE(msg_init() == 0);

    msg_service_init();

    /* Without APRS/DMR/TETRA compiled in, no transport is available and
       nothing is queued or stored. */
    for (uint8_t tp = 0; tp < MSG_TP_NUM; tp++)
        REQUIRE(msg_service_available(tp) == false);

    REQUIRE(msg_service_queue(MSG_TP_APRS, "EA5ABC", "hola", 1000) == NULL);
    REQUIRE(msg_sent_count() == 0);

    /* Empty text, even on an available transport, is refused */
    REQUIRE(msg_service_queue(MSG_TP_APRS, "EA5ABC", "", 1000) == NULL);

    /* Status text always has a meaning */
    REQUIRE(std::strcmp(msg_service_statusText(MSG_ST_ACKED), "ACK") == 0);
    REQUIRE(std::strcmp(msg_service_statusText(99), "-") == 0);
}
