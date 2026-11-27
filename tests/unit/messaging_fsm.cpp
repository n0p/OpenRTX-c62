/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The messaging queue as a state machine (core/messaging.h): what a transport
 * does to an entry between the moment the screen queues it and the moment it is
 * done with it, and what the store keeps across a power cycle.
 *
 * docs/TEST_PLAN.md section 3 asks for the messaging machine -- "borrador,
 * cola, ACK, reintentos" -- and this is the model half of it (the screens are
 * tests/unit/ui_messages.cpp).  The rules under test are the ones a transport
 * relies on and cannot see for itself:
 *
 *   - an entry is only handed out once at a time (begin marks it sending, a
 *     second begin finds nothing);
 *   - a failed transmission is retried up to MSG_MAX_ATTEMPTS and then given
 *     up, so a dead address costs three attempts and not the whole queue;
 *   - "on the air" is SENT when nobody asked for a confirmation and SENDING
 *     when somebody did, and only the later confirmation makes it DELIVERED;
 *   - a cancelled entry is never picked again and a late outcome does not
 *     revive it;
 *   - the store round-trips the outbox, the inbox and the compose draft, and
 *     refuses a record whose CRC does not check out rather than restoring half
 *     of it.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

extern "C" {
#include "core/messaging.h"
#include "core/messaging_store.h"
}

namespace
{

/** A clean store: messaging_init() then drop everything it loaded. */
void resetStore(void)
{
    messaging_init();
    messaging_clear();
    REQUIRE(messaging_ready());
    REQUIRE(messaging_outCount() == 0);
}

/** The state of the entry with that id, re-read from the model every time. */
uint8_t stateOf(int id)
{
    const struct message *m = messaging_find(id);

    REQUIRE(m != NULL);
    return m->state;
}

} /* namespace */

/* ======================================================================== */

TEST_CASE("An outbox entry is sent, retried and then given up", "[messaging][fsm]")
{
    resetStore();

    int id = messaging_send(MSG_TRANSPORT_DMR, 1234567u, "hola", true, 5000u);

    REQUIRE(id > 0);
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 1);
    REQUIRE(messaging_pending(MSG_TRANSPORT_APRS) == 0);

    const struct message *head = messaging_head(MSG_TRANSPORT_DMR);

    REQUIRE(head != NULL);
    REQUIRE(head->id == id);
    REQUIRE(head->state == MSG_STATE_QUEUED);
    REQUIRE(head->attempts == 0);
    REQUIRE(std::string(head->text) == "hola");

    for (int attempt = 1; attempt <= MSG_MAX_ATTEMPTS; attempt++) {
        INFO("attempt " << attempt);

        REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == id);

        /* Handed to the transport: a second poll does not pick it again,
           which is what keeps one message from going out twice */
        REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == -1);
        REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 0);

        struct message *m = messaging_find(id);

        REQUIRE(m != NULL);
        REQUIRE(m->state == MSG_STATE_SENDING);
        REQUIRE(m->attempts == attempt);

        /* The transmission failed: back to the queue, or out of it */
        messaging_done(id, false, (uint16_t)(100 + attempt), 6000u + attempt);

        m = messaging_find(id);
        REQUIRE(m != NULL);
        REQUIRE(m->ref == (100 + attempt));

        if (attempt < MSG_MAX_ATTEMPTS) {
            REQUIRE(m->state == MSG_STATE_QUEUED);
            REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 1);
            REQUIRE(messaging_head(MSG_TRANSPORT_DMR) != NULL);
        } else {
            REQUIRE(m->state == MSG_STATE_FAILED);
            REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 0);
            /* Given up for good: nothing to poll, nothing to retry */
            REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == -1);
            REQUIRE(m->attempts == MSG_MAX_ATTEMPTS);
        }
    }
}

TEST_CASE("delivered ends at SENT or at DELIVERED, per the confirmation asked",
          "[messaging][fsm]")
{
    resetStore();

    /* Nobody asked for a confirmation: on the air is as good as it gets */
    int plain = messaging_send(MSG_TRANSPORT_APRS, 0u, "sin ack", false, 1000u);

    REQUIRE(plain > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_APRS) == plain);
    messaging_done(plain, true, 0u, 1100u);
    REQUIRE(stateOf(plain) == MSG_STATE_SENT);

    /* The sender asked for one: the entry waits, still "sending", and only the
       confirmation that arrives later makes it delivered */
    int acked = messaging_send(MSG_TRANSPORT_DMR, 2222u, "con ack", true, 2000u);

    REQUIRE(acked > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == acked);
    messaging_done(acked, true, 7u, 2100u);
    REQUIRE(stateOf(acked) == MSG_STATE_SENDING);

    messaging_confirmed(acked, 2222u, 2500u);
    REQUIRE(stateOf(acked) == MSG_STATE_DELIVERED);

    /* A confirmation without an id (messaging_confirmed(0, peer)) matches the
       oldest entry still waiting on that peer, which is how a DMR delivery
       response -- it carries the peer, not our id -- finds its message */
    int first  = messaging_send(MSG_TRANSPORT_DMR, 3333u, "uno", true, 3000u);

    REQUIRE(first > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == first);
    messaging_done(first, true, 0u, 3100u);
    REQUIRE(stateOf(first) == MSG_STATE_SENDING);

    messaging_confirmed(0, 3333u, 3200u);
    REQUIRE(stateOf(first) == MSG_STATE_DELIVERED);

    /* The peer we did not hear from does not get its entry delivered */
    int other = messaging_send(MSG_TRANSPORT_DMR, 4444u, "dos", true, 3300u);

    REQUIRE(other > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == other);
    messaging_done(other, true, 0u, 3400u);

    messaging_confirmed(0, 3333u, 3500u);
    REQUIRE(stateOf(other) == MSG_STATE_SENDING);
}

TEST_CASE("a confirmation that never comes is given up on by the task",
          "[messaging][fsm]")
{
    resetStore();

    int id = messaging_send(MSG_TRANSPORT_TETRA, 99u, "sds", true, 1000u);

    REQUIRE(id > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_TETRA) == id);
    REQUIRE(stateOf(id) == MSG_STATE_SENDING);

    /* The transports have no timer: the model times the wait out. 31 s is past
       the 30 s the header promises a confirmation has. */
    messaging_task(31000u);
    REQUIRE(stateOf(id) == MSG_STATE_QUEUED);
    REQUIRE(messaging_pending(MSG_TRANSPORT_TETRA) == 1);

    /* Still inside the window: nothing happens */
    REQUIRE(messaging_begin(MSG_TRANSPORT_TETRA) == id);
    messaging_task(32000u);
    REQUIRE(stateOf(id) == MSG_STATE_SENDING);

    /* Out of attempts, the timeout is a failure, not another lap */
    messaging_task(62000u);
    REQUIRE(stateOf(id) == MSG_STATE_QUEUED);
    REQUIRE(messaging_begin(MSG_TRANSPORT_TETRA) == id);
    messaging_task(100000u);
    REQUIRE(stateOf(id) == MSG_STATE_FAILED);
}

TEST_CASE("APRS keeps its own retries: the store does not queue it twice",
          "[messaging][fsm][aprs]")
{
    resetStore();

    /*
     * APRS hands the text to its mailbox and the mailbox retries it (12 s, then
     * 24 s, three attempts).  The store used to time the same wait out after
     * 30 s and put the entry back in the queue, so aprs_pumpMessaging() sent a
     * second copy while the first was still being retried: the peer saw the
     * message up to six times for the three the protocol allows.
     */
    int aprs = messaging_sendToText(MSG_TRANSPORT_APRS, "EA5ABC", "hola", true,
                                    1000u);

    REQUIRE(aprs > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_APRS) == aprs);
    messaging_done(aprs, true, 0u, 1100u);      /* on the air, waiting the ack */
    REQUIRE(stateOf(aprs) == MSG_STATE_SENDING);

    /* Well past MSG_CONFIRM_TIMEOUT_MS: no second copy leaves the queue */
    messaging_task(120000u);
    REQUIRE(stateOf(aprs) == MSG_STATE_SENDING);
    REQUIRE(messaging_pending(MSG_TRANSPORT_APRS) == 0);
    REQUIRE(messaging_head(MSG_TRANSPORT_APRS) == NULL);

    /* The mailbox outcome is what ends it, as msg_service's aprsSync() does */
    messaging_fail(aprs, 121000u);
    REQUIRE(stateOf(aprs) == MSG_STATE_FAILED);

    /* A transport without a retry schedule of its own still times out here */
    int sds = messaging_send(MSG_TRANSPORT_TETRA, 99u, "sds", true, 200000u);

    REQUIRE(sds > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_TETRA) == sds);
    messaging_done(sds, true, 0u, 201000u);
    messaging_task(232000u);
    REQUIRE(stateOf(sds) == MSG_STATE_QUEUED);
}

TEST_CASE("fail gives an entry up whatever its attempt count", "[messaging][fsm]")
{
    resetStore();

    int id = messaging_send(MSG_TRANSPORT_DMR, 1u, "mala direccion", false, 1000u);

    REQUIRE(id > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == id);
    REQUIRE(messaging_find(id)->attempts == 1);

    /* An address the transport cannot use: no three laps, straight out */
    messaging_fail(id, 1100u);
    REQUIRE(stateOf(id) == MSG_STATE_FAILED);
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == -1);

    /* A queued one can be failed before it is ever picked, too */
    int queued = messaging_send(MSG_TRANSPORT_APRS, 0u, "otra", false, 1200u);

    REQUIRE(queued > 0);
    messaging_fail(queued, 1300u);
    REQUIRE(stateOf(queued) == MSG_STATE_FAILED);
    REQUIRE(messaging_begin(MSG_TRANSPORT_APRS) == -1);
}

TEST_CASE("a cancelled entry is never picked again and ignores a late outcome",
          "[messaging][fsm]")
{
    resetStore();

    /* Cancelled while queued: gone from the transport's view */
    int queued = messaging_send(MSG_TRANSPORT_DMR, 1u, "uno", false, 1000u);

    REQUIRE(queued > 0);
    messaging_cancel(queued, 1100u);
    REQUIRE(stateOf(queued) == MSG_STATE_CANCELLED);
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == -1);

    /* The entry is still in the outbox for the screen to show, though */
    REQUIRE(messaging_outCount() == 1);

    /* Cancelled while on the air: the outcome that arrives afterwards must not
       requeue it and must not claim it was sent */
    int flying = messaging_send(MSG_TRANSPORT_DMR, 2u, "dos", false, 2000u);

    REQUIRE(flying > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == flying);
    messaging_cancel(flying, 2100u);
    REQUIRE(stateOf(flying) == MSG_STATE_CANCELLED);

    messaging_done(flying, false, 0u, 2200u);
    REQUIRE(stateOf(flying) == MSG_STATE_CANCELLED);

    messaging_done(flying, true, 0u, 2300u);
    REQUIRE(stateOf(flying) == MSG_STATE_CANCELLED);
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 0);

    /* And a message already delivered is not cancellable any more */
    int done = messaging_send(MSG_TRANSPORT_DMR, 3u, "tres", false, 3000u);

    REQUIRE(done > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == done);
    messaging_done(done, true, 0u, 3100u);
    REQUIRE(stateOf(done) == MSG_STATE_SENT);
    messaging_cancel(done, 3200u);
    REQUIRE(stateOf(done) == MSG_STATE_SENT);
}

TEST_CASE("the outbox refuses a seventh entry and reuses a finished slot",
          "[messaging][fsm]")
{
    resetStore();

    int ids[MSG_OUTBOX_LEN];

    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        ids[i] = messaging_send(MSG_TRANSPORT_DMR, (uint32_t)i, "lleno", false,
                                (uint32_t)(1000 + i));
        INFO("slot " << i);
        REQUIRE(ids[i] > 0);
    }

    REQUIRE(messaging_outCount() == MSG_OUTBOX_LEN);
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == MSG_OUTBOX_LEN);

    /* Every slot is queued or sending: the seventh is refused, loudly */
    REQUIRE(messaging_send(MSG_TRANSPORT_DMR, 7u, "siete", false, 2000u) == -1);
    REQUIRE(messaging_outCount() == MSG_OUTBOX_LEN);

    /* A finished entry gives its slot back: the queue is a budget, not a
       tombstone that grows until the radio is restarted */
    REQUIRE(messaging_begin(MSG_TRANSPORT_DMR) == ids[0]);
    messaging_done(ids[0], true, 0u, 2100u);
    REQUIRE(stateOf(ids[0]) == MSG_STATE_SENT);

    int fresh = messaging_send(MSG_TRANSPORT_DMR, 8u, "ocho", false, 2200u);

    REQUIRE(fresh > 0);
    REQUIRE(messaging_outCount() == MSG_OUTBOX_LEN);
    /* One finished and one fresh queued: the same six slots, six waiting */
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == MSG_OUTBOX_LEN);

    /* A cancelled one gives its slot back too */
    messaging_cancel(ids[1], 2300u);
    int next = messaging_send(MSG_TRANSPORT_DMR, 9u, "nueve", false, 2400u);

    REQUIRE(next > 0);
    REQUIRE(messaging_outCount() == MSG_OUTBOX_LEN);
}

TEST_CASE("an empty text or an unknown transport is refused", "[messaging][fsm]")
{
    resetStore();

    REQUIRE(messaging_send(MSG_TRANSPORT_DMR, 1u, "", false, 1000u) == -1);
    REQUIRE(messaging_send(MSG_TRANSPORT_DMR, 1u, NULL, false, 1000u) == -1);
    REQUIRE(messaging_send(MSG_TRANSPORT_COUNT, 1u, "x", false, 1000u) == -1);
    REQUIRE(messaging_sendToText(MSG_TRANSPORT_APRS, "", "x", false, 1000u) == -1);
    REQUIRE(messaging_outCount() == 0);

    /* An address by name keeps a stable hash of it, so two messages to the
       same callsign group together and a screen can sort them */
    int a = messaging_sendToText(MSG_TRANSPORT_APRS, "EA5ABC", "hola", false,
                                 1000u);
    int b = messaging_sendToText(MSG_TRANSPORT_APRS, "EA5ABC", "adios", false,
                                 1001u);

    REQUIRE(a > 0);
    REQUIRE(b > 0);
    REQUIRE(messaging_find(a)->dst == messaging_find(b)->dst);
    REQUIRE(std::string(messaging_find(a)->peer) == "EA5ABC");
}

TEST_CASE("a text longer than the record is cut at a character boundary",
          "[messaging][fsm]")
{
    resetStore();

    char long_[200];

    memset(long_, 'A', sizeof(long_));
    long_[sizeof(long_) - 1] = '\0';

    int id = messaging_send(MSG_TRANSPORT_DMR, 1u, long_, false, 1000u);

    REQUIRE(id > 0);

    const struct message *m = messaging_find(id);

    REQUIRE(m != NULL);
    REQUIRE(m->textLen == MSG_TEXT_MAX);
    REQUIRE(strlen(m->text) == MSG_TEXT_MAX);
    REQUIRE((m->flags & MSG_FLAG_TRUNCATED) != 0);

    /* 159 'A' and a two-octet character that does not fit: the whole
       character goes, not half of it */
    memset(long_, 'A', 159);
    long_[159] = (char)0xC3;
    long_[160] = (char)0xA9;
    long_[161] = '\0';

    id = messaging_send(MSG_TRANSPORT_DMR, 1u, long_, false, 1100u);
    REQUIRE(id > 0);
    m = messaging_find(id);
    REQUIRE(m->textLen == 159);
    REQUIRE((m->flags & MSG_FLAG_TRUNCATED) != 0);
    REQUIRE((uint8_t)m->text[158] == (uint8_t)'A');
}

TEST_CASE("the outbox, the inbox and the draft round-trip through the record",
          "[messaging][fsm][store]")
{
    resetStore();

    /* Two outbound entries, one of them mid-flight, one inbound, a draft */
    int queued  = messaging_send(MSG_TRANSPORT_DMR, 111u, "pendiente", true,
                                 1000u);
    int flying  = messaging_send(MSG_TRANSPORT_APRS, 0u, "en el aire", false,
                                 1100u);

    REQUIRE(queued > 0);
    REQUIRE(flying > 0);
    REQUIRE(messaging_begin(MSG_TRANSPORT_APRS) == flying);
    REQUIRE(stateOf(flying) == MSG_STATE_SENDING);

    REQUIRE(messaging_received(MSG_TRANSPORT_TETRA, 555u, 1u, "recibido", true,
                               1200u) > 0);
    REQUIRE(messaging_received(MSG_TRANSPORT_APRS, 666u, 0u, "nuevo", false,
                               1300u) > 0);
    REQUIRE(messaging_unread() == 2);

    struct message *d = messaging_draft();

    d->transport = (uint8_t)MSG_TRANSPORT_TETRA;
    d->dst = 777u;
    snprintf(d->peer, sizeof(d->peer), "%s", "EA5ABC");
    snprintf(d->text, sizeof(d->text), "%s", "borrador a medias");
    messaging_draftTouch();

    uint8_t record[MSG_STORE_MAX];
    int n = messaging_serialise(record, sizeof(record));

    REQUIRE(n > 0);

    /* A power cycle: the model comes up empty and the record fills it */
    messaging_clear();
    REQUIRE(messaging_outCount() == 0);
    REQUIRE(messaging_inbox(0) == NULL);
    REQUIRE(messaging_draft()->text[0] == '\0');

    REQUIRE(messaging_deserialise(record, (size_t)n) == 4);

    REQUIRE(messaging_outCount() == 2);
    REQUIRE(messaging_pending(MSG_TRANSPORT_DMR) == 1);
    /* The one that was on the air is queued again, so its transport sees it */
    REQUIRE(messaging_pending(MSG_TRANSPORT_APRS) == 1);

    const struct message *out = messaging_outbox(0);

    REQUIRE(out != NULL);
    REQUIRE(out->id == queued);
    REQUIRE(out->state == MSG_STATE_QUEUED);
    REQUIRE(out->dst == 111u);
    REQUIRE((out->flags & MSG_FLAG_CONFIRM) != 0);
    REQUIRE(std::string(out->text) == "pendiente");

    /* An entry that was on the air when the power went has no known outcome:
       it comes back queued, never claimed as sent */
    out = messaging_outbox(1);
    REQUIRE(out != NULL);
    REQUIRE(out->id == flying);
    REQUIRE(out->state == MSG_STATE_QUEUED);
    REQUIRE(out->attempts == 1);

    REQUIRE(messaging_inbox(0) != NULL);
    REQUIRE(std::string(messaging_inbox(0)->text) == "nuevo");
    REQUIRE(std::string(messaging_inbox(1)->text) == "recibido");
    REQUIRE((messaging_inbox(1)->flags & MSG_FLAG_INBOUND) != 0);
    REQUIRE(messaging_unread() == 2);

    d = messaging_draft();
    REQUIRE(d->transport == (uint8_t)MSG_TRANSPORT_TETRA);
    REQUIRE(d->dst == 777u);
    REQUIRE(std::string(d->peer) == "EA5ABC");
    REQUIRE(std::string(d->text) == "borrador a medias");

    /*
     * And the record is stable: writing it again after a load gives the same
     * bytes, so a radio that reboots twice does not lose or rewrite anything.
     * (The first record is not byte for byte the same: the entry that was
     * sending comes back queued, which is the point of the load.)
     */
    uint8_t again[MSG_STORE_MAX];
    uint8_t third[MSG_STORE_MAX];

    REQUIRE(messaging_serialise(again, sizeof(again)) == n);
    REQUIRE(messaging_deserialise(again, (size_t)n) == 4);
    REQUIRE(messaging_serialise(third, sizeof(third)) == n);
    REQUIRE(memcmp(again, third, (size_t)n) == 0);
}

TEST_CASE("a record that does not check out restores nothing", "[messaging][fsm][store]")
{
    resetStore();

    REQUIRE(messaging_send(MSG_TRANSPORT_DMR, 1u, "hola", false, 1000u) > 0);
    REQUIRE(messaging_received(MSG_TRANSPORT_DMR, 2u, 1u, "adios", false,
                               1100u) > 0);

    uint8_t good[MSG_STORE_MAX];
    int n = messaging_serialise(good, sizeof(good));

    REQUIRE(n > 0);

    /* 1: a CRC that does not match -- one bit flipped in the payload */
    uint8_t bad[MSG_STORE_MAX];

    memcpy(bad, good, (size_t)n);
    bad[12] ^= 0x01;
    REQUIRE(messaging_deserialise(bad, (size_t)n) == -1);
    REQUIRE(messaging_outCount() == 0);
    REQUIRE(messaging_inbox(0) == NULL);

    /* 2: the magic of another record */
    memcpy(bad, good, (size_t)n);
    bad[0] = 'X';
    REQUIRE(messaging_deserialise(bad, (size_t)n) == -1);

    /* 3: a record cut short */
    REQUIRE(messaging_deserialise(good, (size_t)n - 3) == -1);
    REQUIRE(messaging_deserialise(good, 10) == -1);
    REQUIRE(messaging_deserialise(NULL, (size_t)n) == -1);

    /* And after refusing it the store is still empty, not half filled */
    REQUIRE(messaging_outCount() == 0);
    REQUIRE(messaging_inbox(0) == NULL);
    REQUIRE(messaging_unread() == 0);

    /* A buffer too small is refused instead of overrunning it */
    REQUIRE(messaging_serialise(bad, 4) == -1);

    /* The undamaged record still loads */
    REQUIRE(messaging_deserialise(good, (size_t)n) == 2);
    REQUIRE(messaging_outCount() == 1);
    REQUIRE(messaging_inbox(0) != NULL);
}

TEST_CASE("the draft is a field of the store: touched, cleared and dropped by clear",
          "[messaging][fsm]")
{
    resetStore();

    struct message *d = messaging_draft();

    REQUIRE(d != NULL);
    snprintf(d->text, sizeof(d->text), "%s", "a medias");
    messaging_draftTouch();
    REQUIRE(messaging_draft()->textLen == 8);

    messaging_draftClear();
    REQUIRE(messaging_draft()->text[0] == '\0');
    REQUIRE(messaging_draft()->textLen == 0);

    snprintf(d->text, sizeof(d->text), "%s", "otra vez");
    messaging_draftTouch();
    messaging_clear();
    REQUIRE(messaging_draft()->text[0] == '\0');
}
