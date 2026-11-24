/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests for the shared message store: core/messaging.c, which the UI
 * writes to and APRS, DMR SMS and TETRA SDS pump.
 *
 *   cc -O2 -std=gnu17 -Wall -Wextra -I openrtx/include \
 *      tests/platform/c62_messaging_host.c openrtx/src/core/messaging.c \
 *      -o c62_messaging_host && ./c62_messaging_host
 *
 * The flash backend of the firmware (messaging_store_zephyr.c) is replaced
 * here by a RAM buffer with the same contract.
 */

#include <stdio.h>
#include <string.h>

#include "core/messaging.h"
#include "core/messaging_store.h"

/* ------------------------------------------------------- RAM store stub --- */

static uint8_t store[MSG_STORE_MAX];
static size_t storeLen;
static unsigned storeWrites;

size_t messaging_store_size(void)
{
    return storeLen;
}

size_t messaging_store_read(uint8_t *buf, size_t cap)
{
    if ((storeLen == 0) || (storeLen > cap))
        return 0;
    memcpy(buf, store, storeLen);
    return storeLen;
}

bool messaging_store_write(const uint8_t *buf, size_t len)
{
    if (len > sizeof(store))
        return false;
    memcpy(store, buf, len);
    storeLen = len;
    storeWrites++;
    return true;
}

void messaging_store_clear(void)
{
    storeLen = 0;
}

/* ----------------------------------------------------------------- tests --- */

static unsigned failures;
static unsigned checks;

static void check(int ok, const char *what)
{
    checks++;
    if (ok)
        return;
    failures++;
    printf("  FAIL %s\n", what);
}

static void reset(void)
{
    storeLen = 0;
    storeWrites = 0;
    messaging_init();
}

static void testSendReceive(void)
{
    printf("send and receive\n");
    reset();

    int id = messaging_send(MSG_TRANSPORT_DMR, 1234568, "Hola", false, 1000);

    check(id > 0, "send returns an id");
    check(messaging_pending(MSG_TRANSPORT_DMR) == 1, "one queued for DMR");
    check(messaging_pending(MSG_TRANSPORT_APRS) == 0, "none for APRS");

    const struct message *m = messaging_head(MSG_TRANSPORT_DMR);

    check(m != NULL, "head is not NULL");
    check((m != NULL) && (m->dst == 1234568), "destination");
    check((m != NULL) && (m->state == MSG_STATE_QUEUED), "queued");
    check((m != NULL) && (strcmp(m->text, "Hola") == 0), "text");
    check((m != NULL) && (m->textLen == 4), "text length");

    check(messaging_send(MSG_TRANSPORT_DMR, 1, "", false, 1000) < 0,
          "empty text refused");
    check(messaging_send(MSG_TRANSPORT_COUNT, 1, "x", false, 1000) < 0,
          "bad transport refused");

    int rid = messaging_received(MSG_TRANSPORT_TETRA, 4242, 99, "Recibido",
                                 true, 2000);

    check(rid > 0, "inbound returns an id");
    check(messaging_unread() == 1, "one unread");
    const struct message *in = messaging_inbox(0);

    check((in != NULL) && (in->src == 4242), "inbound source");
    check((in != NULL) && ((in->flags & MSG_FLAG_CONFIRM) != 0),
          "confirmation requested");
    check((in != NULL) && (in->transport == MSG_TRANSPORT_TETRA), "transport");
    check(messaging_inbox(1) == NULL, "only one inbound");
    messaging_markRead();
    check(messaging_unread() == 0, "marked read");
}

static void testStates(void)
{
    printf("outbox states\n");
    reset();

    /* without a confirmation request the message is sent, not delivered */
    int a = messaging_send(MSG_TRANSPORT_DMR, 1, "sin ack", false, 100);

    check(messaging_begin(MSG_TRANSPORT_DMR) == a, "begin returns the id");
    check(messaging_pending(MSG_TRANSPORT_DMR) == 0, "no longer queued");
    messaging_done(a, true, 0, 200);
    struct message *m = messaging_find(a);

    check((m != NULL) && (m->state == MSG_STATE_SENT), "state sent");

    /* with one, it stays "sending" until the peer confirms */
    reset();
    int b = messaging_send(MSG_TRANSPORT_DMR, 7, "con ack", true, 100);

    messaging_begin(MSG_TRANSPORT_DMR);
    messaging_done(b, true, 0, 200);
    m = messaging_find(b);
    check((m != NULL) && (m->state == MSG_STATE_SENDING), "waiting for the ack");
    messaging_confirmed(0, 7, 300);
    m = messaging_find(b);
    check((m != NULL) && (m->state == MSG_STATE_DELIVERED), "delivered");

    /* a failure is retried and then given up */
    reset();
    int c = messaging_send(MSG_TRANSPORT_DMR, 8, "falla", false, 100);

    for (int i = 0; i < MSG_MAX_ATTEMPTS; i++) {
        check(messaging_begin(MSG_TRANSPORT_DMR) == c, "begin for a retry");
        messaging_done(c, false, 0, (uint32_t)(200 + i));
    }
    m = messaging_find(c);
    check((m != NULL) && (m->state == MSG_STATE_FAILED), "failed after 3 tries");

    /* the timeout of a confirmation nobody sent */
    reset();
    int d = messaging_send(MSG_TRANSPORT_DMR, 9, "sin respuesta", true, 100);

    messaging_begin(MSG_TRANSPORT_DMR);
    messaging_done(d, true, 0, 200);
    messaging_task(200 + 1000);
    m = messaging_find(d);
    check((m != NULL) && (m->state == MSG_STATE_SENDING), "still inside timeout");
    messaging_task(200 + 60000);
    m = messaging_find(d);
    check((m != NULL) && (m->state == MSG_STATE_QUEUED), "back in the queue");
}

static void testOrderAndFull(void)
{
    printf("order and a full outbox\n");
    reset();

    messaging_send(MSG_TRANSPORT_DMR, 1, "primero", false, 10);
    messaging_send(MSG_TRANSPORT_DMR, 2, "segundo", false, 20);
    messaging_send(MSG_TRANSPORT_APRS, 3, "aprs", false, 15);

    const struct message *m = messaging_head(MSG_TRANSPORT_DMR);

    check((m != NULL) && (m->dst == 1), "oldest first");

    /* fill the outbox; it must steal a finished entry rather than refuse */
    for (int i = 0; i < MSG_OUTBOX_LEN + 4; i++) {
        int id = messaging_send(MSG_TRANSPORT_DMR, (uint32_t)(100 + i), "x",
                                false, (uint32_t)(100 + i));

        if (id > 0) {
            messaging_begin(MSG_TRANSPORT_DMR);
            messaging_done(id, true, 0, (uint32_t)(100 + i));
        }
    }
    bool any = false;

    for (int i = 0; i < MSG_OUTBOX_LEN; i++)
        any = any || (messaging_outbox(i) != NULL);
    check(any, "the outbox still holds messages after overflowing");
}

static void testTruncation(void)
{
    printf("text handling\n");
    reset();

    char big[MSG_TEXT_MAX + 40];
    size_t i = 0;

    /* 'ñ' is two octets in UTF-8: the cut must not split it */
    while (i + 2 < sizeof(big)) {
        big[i++] = (char)0xC3;
        big[i++] = (char)0xB1;
    }
    big[i] = '\0';

    int id = messaging_send(MSG_TRANSPORT_DMR, 1, big, false, 0);
    const struct message *m = messaging_find(id);

    check(m != NULL, "long text queued");
    check((m != NULL) && (m->textLen <= MSG_TEXT_MAX), "text cut to the size");
    check((m != NULL) && ((m->flags & MSG_FLAG_TRUNCATED) != 0), "truncated flag");
    /* every character of the text is a whole 'ñ' (C3 B1) */
    bool whole = (m != NULL) && ((m->textLen % 2) == 0);

    for (size_t k = 0; whole && (k + 1 < (size_t)m->textLen); k += 2)
        whole = (((uint8_t)m->text[k] == 0xC3) && ((uint8_t)m->text[k + 1] == 0xB1));
    check(whole, "cut on a character boundary");
    check((m != NULL) && (m->text[m->textLen] == '\0'), "NUL terminated");

    /* a name-addressed message keeps the name and a hash of it */
    reset();
    int a = messaging_sendToText(MSG_TRANSPORT_APRS, "EA5YYY-1", "hola", false, 0);
    const struct message *aprs = messaging_find(a);

    check(aprs != NULL, "addressed by callsign");
    check((aprs != NULL) && (strcmp(aprs->peer, "EA5YYY-1") == 0), "callsign kept");
    check((aprs != NULL) && (aprs->dst != 0), "hash in the numeric address");
}

static void testPersistence(void)
{
    printf("flash record\n");
    reset();

    messaging_send(MSG_TRANSPORT_DMR, 1234568, "en el aire", true, 500);
    messaging_received(MSG_TRANSPORT_DMR, 1234567, 1234568, "para ti", false, 600);

    static uint8_t rec[MSG_STORE_MAX];
    int n = messaging_serialise(rec, sizeof(rec));

    check(n > 0, "serialised");
    check(messaging_store_write(rec, (size_t)((n > 0) ? n : 0)), "stored");

    /* a power cycle: the RAM is gone, the record is not */
    messaging_init();
    check(messaging_pending(MSG_TRANSPORT_DMR) == 1, "queued message survived");
    check(messaging_unread() == 1, "inbound message survived");
    const struct message *in = messaging_inbox(0);

    check((in != NULL) && (strcmp(in->text, "para ti") == 0), "inbound text");

    /* a backlog in "sending" is queued again: its outcome was never known */
    reset();
    int id = messaging_send(MSG_TRANSPORT_DMR, 5, "a medias", true, 10);

    messaging_begin(MSG_TRANSPORT_DMR);
    n = messaging_serialise(rec, sizeof(rec));
    messaging_store_write(rec, (size_t)((n > 0) ? n : 0));
    messaging_init();
    const struct message *m = messaging_find(id);

    check((m != NULL) && (m->state == MSG_STATE_QUEUED), "sending is requeued");

    /* a corrupt record is rejected whole */
    reset();
    messaging_send(MSG_TRANSPORT_DMR, 5, "texto", false, 10);
    n = messaging_serialise(rec, sizeof(rec));
    rec[12] ^= 0x5A;
    check(messaging_deserialise(rec, (size_t)((n > 0) ? n : 0)) < 0,
          "corrupt record rejected");
    check(messaging_outbox(0) == NULL, "nothing restored from it");

    /* and the debounced write only happens when asked */
    reset();
    messaging_send(MSG_TRANSPORT_DMR, 5, "texto", false, 10);
    unsigned before = storeWrites;

    messaging_task(10);
    check(storeWrites == before, "no write before the debounce");
    messaging_task(100000);
    check(storeWrites == before + 1, "written after the debounce");
    check(!messaging_dirty(), "not dirty after the write");
}

static void testNames(void)
{
    printf("diagnostics\n");
    check(strcmp(messaging_transportName(MSG_TRANSPORT_DMR), "DMR") == 0,
          "transport name");
    check(strcmp(messaging_stateName(MSG_STATE_DELIVERED), "delivered") == 0,
          "state name");
}

int main(void)
{
    testSendReceive();
    testStates();
    testOrderAndFull();
    testTruncation();
    testPersistence();
    testNames();

    printf("%u checks, %u failures\n", checks, failures);
    if (failures != 0) {
        printf("RESULT FAIL\n");
        return 1;
    }
    printf("RESULT OK: the message store behaves as the UI and the transports expect\n");
    return 0;
}
