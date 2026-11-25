/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host test of the two-slot record container, of the contact book built on it
 * and of the message record of the shared store: round trip, newest-slot wins,
 * corrupt newest slot falls back, a power cut in the middle of a save costs the
 * save but never the previous bank, and the message record carries the queues,
 * the states and the compose draft through a round trip.
 */

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

extern "C" {
#include "core/slot_store.h"
#include "core/contact_store.h"
#include "core/messaging.h"
#include "core/messaging_store.h"
#include "core/msg_service.h"
}

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

TEST_CASE("A contact whose write fails is not added and says so",
          "[store][contacts]")
{
    TestPort p;
    cstore_bind(&p.ops);
    cstore_forget();
    REQUIRE(cstore_init() == 0);

    cstore_contact_t c = {};
    snprintf(c.call, sizeof(c.call), "EA5ABC-7");
    snprintf(c.name, sizeof(c.name), "Pepe");
    REQUIRE(cstore_add(&c) == 0);
    REQUIRE(cstore_count() == 1);

    /*
     * The flash refuses the next save. cstore_add() used to keep the record in
     * RAM and return -EIO anyway: the contact book then showed a contact that
     * was never written, and it disappeared on the next power cycle. The list
     * is what the operator believes, so a failed save must not change it.
     */
    p.flash.dead = true;

    cstore_contact_t second = {};
    snprintf(second.call, sizeof(second.call), "EA5DEF");
    REQUIRE(cstore_add(&second) == -EIO);
    REQUIRE(cstore_count() == 1);

    /* The failed record is gone, not merely hidden */
    cstore_contact_t got;
    REQUIRE(cstore_get(1, &got) < 0);
    REQUIRE(cstore_findCall("EA5DEF") == CSTORE_NONE);

    /* And the first one is still the one on flash */
    cstore_forget();
    p.flash.dead = false;
    REQUIRE(cstore_init() == 0);
    REQUIRE(cstore_count() == 1);
    REQUIRE(cstore_findCall("EA5ABC-7") == 0);
}

TEST_CASE("Message store keeps inbox, outbox, states and draft",
          "[store][messages]")
{
    static uint8_t record[MSG_STORE_MAX];
    int recordLen = 0;

    messaging_init();
    messaging_clear();
    REQUIRE(messaging_outCount() == 0);
    REQUIRE(messaging_unread() == 0);

    int out = messaging_sendAddr(MSG_TRANSPORT_DMR, 1234567, "EA5ABC-7",
                                 "hola", true, 1000);
    REQUIRE(out > 0);
    REQUIRE(messaging_outCount() == 1);

    const struct message *o = messaging_outbox(0);
    REQUIRE(o != NULL);
    REQUIRE(o->state == MSG_STATE_QUEUED);
    REQUIRE(o->transport == MSG_TRANSPORT_DMR);
    REQUIRE(o->dst == 1234567);
    REQUIRE(std::strcmp(o->peer, "EA5ABC-7") == 0);
    REQUIRE((o->flags & MSG_FLAG_CONFIRM) != 0);

    int in = messaging_received(MSG_TRANSPORT_DMR, 7654321, 1234567, "que tal",
                                false, 2000);
    REQUIRE(in > 0);
    REQUIRE(messaging_unread() == 1);
    REQUIRE(std::strcmp(messaging_inbox(0)->text, "que tal") == 0);

    /* The draft is one more entry of the same record */
    struct message *d = messaging_draft();
    REQUIRE(d != NULL);
    snprintf(d->peer, sizeof(d->peer), "EA5ABC-7");
    snprintf(d->text, sizeof(d->text), "sin enviar");
    d->transport = MSG_TRANSPORT_DMR;
    messaging_draftTouch();

    recordLen = messaging_serialise(record, sizeof(record));
    REQUIRE(recordLen > 0);

    /* A restart: the queues, the unread flag and the draft come back */
    messaging_clear();
    REQUIRE(messaging_unread() == 0);
    REQUIRE(messaging_outCount() == 0);
    REQUIRE(messaging_draft()->text[0] == '\0');

    REQUIRE(messaging_deserialise(record, (size_t)recordLen) > 0);
    REQUIRE(messaging_outCount() == 1);
    REQUIRE(messaging_unread() == 1);
    REQUIRE(messaging_inbox(0) != NULL);
    REQUIRE(std::strcmp(messaging_draft()->text, "sin enviar") == 0);

    /* A record whose CRC does not close leaves nothing behind */
    record[recordLen / 2] ^= 0xFF;
    REQUIRE(messaging_deserialise(record, (size_t)recordLen) == -1);
    REQUIRE(messaging_unread() == 0);
    REQUIRE(messaging_outCount() == 0);
}

TEST_CASE("Message states: sending, confirming, cancelling, reading",
          "[messages]")
{
    messaging_init();
    messaging_clear();

    int a = messaging_send(MSG_TRANSPORT_DMR, 111, "uno", false, 100);
    int b = messaging_send(MSG_TRANSPORT_DMR, 222, "dos", true, 200);
    REQUIRE(a > 0);
    REQUIRE(b > 0);
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 2);

    /* The transport takes the oldest one */
    int begun = messaging_begin(MSG_TRANSPORT_DMR);
    REQUIRE(begun == a);
    REQUIRE(messaging_find(a)->state == MSG_STATE_SENDING);
    REQUIRE(messaging_find(a)->attempts == 1);

    /* No confirmation asked for: on the air is as good as it gets */
    messaging_done(a, true, 7, 300);
    REQUIRE(messaging_find(a)->state == MSG_STATE_SENT);
    REQUIRE(messaging_find(a)->ref == 7);

    /* Confirmation asked for: it waits, and the ACK moves it forward */
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == b);
    messaging_done(b, true, 0, 400);
    REQUIRE(messaging_find(b)->state == MSG_STATE_SENDING);
    messaging_confirmed(0, 222, 500);
    REQUIRE(messaging_find(b)->state == MSG_STATE_DELIVERED);

    /* A cancelled message keeps its state whatever the transport reports */
    int c = messaging_send(MSG_TRANSPORT_DMR, 333, "tres", false, 600);
    REQUIRE(c > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == c);
    messaging_cancel(c, 700);
    REQUIRE(messaging_find(c)->state == MSG_STATE_CANCELLED);
    messaging_done(c, true, 0, 800);
    REQUIRE(messaging_find(c)->state == MSG_STATE_CANCELLED);

    /* A cancelled message is never picked again */
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) < 0);

    /* Reading one message leaves the others unread */
    int m1 = messaging_received(MSG_TRANSPORT_APRS, 1, 0, "primero", false, 900);
    REQUIRE(m1 > 0);
    REQUIRE(messaging_received(MSG_TRANSPORT_APRS, 2, 0, "segundo", false,
                               1000) > 0);
    REQUIRE(messaging_unread() == 2);
    messaging_markReadId(m1);
    REQUIRE(messaging_unread() == 1);
    REQUIRE(messaging_inbox(0)->id != (uint8_t)m1);
    messaging_markRead();
    REQUIRE(messaging_unread() == 0);
}

TEST_CASE("Message service refuses what it cannot send", "[messages]")
{
    messaging_init();
    messaging_clear();
    msg_service_init();

    /* Without APRS/DMR/TETRA compiled in, no transport is available and
       nothing is queued or stored. */
    for (uint8_t tp = 0; tp < MSG_TRANSPORT_COUNT; tp++)
        REQUIRE(msg_service_available((enum msgTransport)tp) == false);
    REQUIRE(msg_service_default() == MSG_TRANSPORT_COUNT);

    REQUIRE(msg_service_queue(MSG_TRANSPORT_APRS, "EA5ABC", "hola", false, 1000)
            < 0);
    REQUIRE(messaging_outCount() == 0);

    /* Empty text, even on an available transport, is refused */
    REQUIRE(msg_service_queue(MSG_TRANSPORT_APRS, "EA5ABC", "", false, 1000)
            < 0);

    /* Status text always has a meaning */
    REQUIRE(std::strcmp(msg_service_statusText(MSG_STATE_DELIVERED), "ACK") == 0);
    REQUIRE(std::strcmp(msg_service_statusText(MSG_STATE_CANCELLED), "CANC")
            == 0);
    REQUIRE(std::strcmp(msg_service_statusText(99), "-") == 0);
}
