/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file messaging.c
 * @brief The message model shared by APRS, DMR SMS and TETRA SDS: outbox,
 * inbox, states and the flash record.  See messaging.h.
 *
 * The transports pump this: each one asks for the next message addressed to
 * it (messaging_head / messaging_begin), transmits it and reports the outcome
 * (messaging_done).  A confirmation that arrives later than the transmission
 * (a DMR delivery response, a TMS acknowledgement) comes back through
 * messaging_confirmed() and moves the entry to "delivered" while it waits in
 * the outbox for the UI to show it.
 *
 * Threading: the RTX thread and the UI thread both touch the store, so every
 * entry point is guarded by the same mutex the radio status uses.  Nothing
 * here blocks for long: the record is a memcpy of at most 3 kB, and it is
 * only packed from messaging_task(), which runs outside the real-time path.
 */

#include <string.h>

#include "core/messaging.h"
#include "core/messaging_store.h"

/*
 * The store is shared by the RTX thread and the UI thread.  On the radio that
 * is a Zephyr mutex; a host build of this file (the messaging tests) runs
 * single threaded and compiles the lock away.
 */
#ifdef __ZEPHYR__
#include <zephyr/kernel.h>
static struct k_mutex msgLock;
#define MSG_LOCK()   ((void)k_mutex_lock(&msgLock, K_FOREVER))
#define MSG_UNLOCK() ((void)k_mutex_unlock(&msgLock))
#else
#define MSG_LOCK()   ((void)0)
#define MSG_UNLOCK() ((void)0)
#endif

/** Debounce of the flash write: the radio writes once, a few seconds later. */
#define MSG_WRITE_DEBOUNCE_MS 4000

#define MSG_RECORD_MAGIC 0x3147534dU /* "MSG1" */
#define MSG_RECORD_VERSION 2

/** The packed record, shared by the load and the write: 3,3 kB, not on a
 *  stack and not duplicated. */
static uint8_t recordBuf[MSG_STORE_MAX];

static struct message outbox[MSG_OUTBOX_LEN];
static struct message inbox[MSG_INBOX_LEN];
static unsigned inboxCount;
static unsigned inboxUnread;
static uint8_t nextId = 1;
static bool dirty;
static uint32_t lastChange;

/**
 * The compose draft.  It is one more entry of the same record, not a queue:
 * the messaging screen edits it in place and the store writes it with the rest
 * of the changes.
 */
static struct message draft;
static bool draftUsed;

/** True once messaging_init() has run, so the store is built and the record
 *  read.  Opening a screen must not build it again: that would reload the
 *  record over messages the radio has just received. */
static bool initialised;

/** Bytes one entry takes in the record, before its text: id, transport,
 *  state, flags, attempts, src, dst, time, ref, textLen and the peer name. */
#define ENTRY_HEAD 30

/** A transmission with no confirmation after this long is given up. */
#define MSG_CONFIRM_TIMEOUT_MS 30000

static void lock(void)
{
    MSG_LOCK();
}

static void unlock(void)
{
    MSG_UNLOCK();
}

static void touch(void)
{
    dirty = true;
}

/* ---------------------------------------------------------------- queue --- */

static struct message *freeSlot(void)
{
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        if (outbox[i].state == MSG_STATE_FREE)
            return &outbox[i];
    }
    /* Steal the oldest finished entry rather than refuse a new message. */
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        if ((outbox[i].state == MSG_STATE_DELIVERED)
            || (outbox[i].state == MSG_STATE_FAILED)
            || (outbox[i].state == MSG_STATE_CANCELLED)
            || (outbox[i].state == MSG_STATE_SENT))
            return &outbox[i];
    }
    return NULL;
}

/**
 * Copy a text into a message, cutting it at a UTF-8 character boundary and
 * flagging the truncation.
 */
static void copyText(struct message *m, const char *text)
{
    size_t n = 0;
    bool cut = false;

    if (text != NULL) {
        while ((text[n] != '\0') && (n < MSG_TEXT_MAX))
            n++;
        if (text[n] != '\0') {
            /* do not leave half a multi-octet character at the end */
            cut = true;
            while ((n > 0) && (((uint8_t)text[n] & 0xC0u) == 0x80u))
                n--;
        }
        memcpy(m->text, text, n);
    }
    m->text[n] = '\0';
    m->textLen = (uint8_t)n;
    if (cut)
        m->flags |= MSG_FLAG_TRUNCATED;
}

void messaging_init(void)
{
#ifdef __ZEPHYR__
    k_mutex_init(&msgLock);
#endif
    lock();
    memset(outbox, 0, sizeof(outbox));
    memset(inbox, 0, sizeof(inbox));
    inboxCount = 0;
    inboxUnread = 0;
    nextId = 1;
    dirty = false;
    lastChange = 0;
    unlock();
    initialised = true;

    size_t n = messaging_store_size();

    if ((n > 0) && (n <= MSG_STORE_MAX)) {
        size_t got = messaging_store_read(recordBuf, sizeof(recordBuf));

        if (got > 0)
            (void)messaging_deserialise(recordBuf, got);
    }
}

int messaging_sendAddr(enum msgTransport transport, uint32_t dst,
                       const char *peer, const char *text, bool confirm,
                       uint32_t now)
{
    if ((transport >= MSG_TRANSPORT_COUNT) || (text == NULL)
        || (text[0] == '\0'))
        return -1;

    lock();
    struct message *m = freeSlot();

    if (m == NULL) {
        unlock();
        return -1;
    }
    memset(m, 0, sizeof(*m));
    m->id = nextId++;
    if (nextId == 0)
        nextId = 1;
    m->transport = (uint8_t)transport;
    m->state = MSG_STATE_QUEUED;
    m->dst = dst;
    m->time = now;
    if (confirm)
        m->flags |= MSG_FLAG_CONFIRM;
    if (peer != NULL) {
        size_t n = 0;

        while ((peer[n] != '\0') && (n < sizeof(m->peer) - 1))
            n++;
        memcpy(m->peer, peer, n);
        m->peer[n] = '\0';
    }
    copyText(m, text);
    int id = (int)m->id;

    touch();
    unlock();
    return id;
}

int messaging_send(enum msgTransport transport, uint32_t dst, const char *text,
                   bool confirm, uint32_t now)
{
    return messaging_sendAddr(transport, dst, NULL, text, confirm, now);
}

int messaging_sendToText(enum msgTransport transport, const char *peer,
                         const char *text, bool confirm, uint32_t now)
{
    if ((peer == NULL) || (peer[0] == '\0'))
        return -1;

    /* a stable 24-bit hash of the name, so the numeric field means something */
    uint32_t hash = 2166136261u;

    for (const char *p = peer; (*p != '\0') && (p < peer + 11); p++) {
        hash = (hash ^ (uint32_t)(uint8_t)*p) * 16777619u;
    }
    return messaging_sendAddr(transport, hash & 0xFFFFFFu, peer, text, confirm,
                              now);
}

int messaging_pending(enum msgTransport transport)
{
    int n = 0;

    lock();
    for (int i = 0; i < MSG_OUTBOX_LEN; i++)
        if ((outbox[i].state == MSG_STATE_QUEUED)
            && (outbox[i].transport == (uint8_t)transport))
            n++;
    unlock();
    return n;
}

const struct message *messaging_head(enum msgTransport transport)
{
    const struct message *best = NULL;

    lock();
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        const struct message *m = &outbox[i];

        if ((m->state != MSG_STATE_QUEUED)
            || (m->transport != (uint8_t)transport))
            continue;
        if ((best == NULL) || (m->time < best->time)
            || ((m->time == best->time) && (m->id < best->id)))
            best = m;
    }
    unlock();
    return best;
}

int messaging_begin(enum msgTransport transport)
{
    lock();
    struct message *m = NULL;

    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        struct message *c = &outbox[i];

        if ((c->state != MSG_STATE_QUEUED)
            || (c->transport != (uint8_t)transport))
            continue;
        if ((m == NULL) || (c->time < m->time)
            || ((c->time == m->time) && (c->id < m->id)))
            m = c;
    }
    if (m == NULL) {
        unlock();
        return -1;
    }
    m->state = MSG_STATE_SENDING;
    m->attempts++;
    int id = (int)m->id;

    touch();
    unlock();
    return id;
}

struct message *messaging_find(int id)
{
    struct message *found = NULL;

    lock();
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        if ((outbox[i].id == (uint8_t)id) && (outbox[i].state != MSG_STATE_FREE)) {
            found = &outbox[i];
            break;
        }
    }
    unlock();
    return found;
}

void messaging_done(int id, bool delivered, uint16_t ref, uint32_t now)
{
    lock();
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        struct message *m = &outbox[i];

        if ((m->id != (uint8_t)id) || (m->state == MSG_STATE_FREE))
            continue;
        /* A cancelled message keeps its state: the outcome is not interesting */
        if (m->state == MSG_STATE_CANCELLED)
            break;
        if (ref != 0)
            m->ref = ref;
        if (delivered) {
            /*
             * Without a confirmation request the message is as good as it
             * gets once it is on the air: "sent", not "delivered".
             */
            m->state = ((m->flags & MSG_FLAG_CONFIRM) != 0) ? MSG_STATE_SENDING
                                                            : MSG_STATE_SENT;
        } else if (m->attempts >= MSG_MAX_ATTEMPTS) {
            m->state = MSG_STATE_FAILED;
        } else {
            m->state = MSG_STATE_QUEUED;
        }
        m->time = now;
        break;
    }
    touch();
    unlock();
}

void messaging_fail(int id, uint32_t now)
{
    lock();
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        struct message *m = &outbox[i];

        if ((m->id != (uint8_t)id) || (m->state == MSG_STATE_FREE))
            continue;
        m->state = MSG_STATE_FAILED;
        m->time = now;
        touch();
        break;
    }
    unlock();
}

void messaging_confirmed(int id, uint32_t peer, uint32_t now)
{
    lock();
    struct message *pick = NULL;

    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        struct message *m = &outbox[i];

        if ((m->state != MSG_STATE_SENDING) && (m->state != MSG_STATE_SENT))
            continue;
        if ((id != 0) && (m->id != (uint8_t)id))
            continue;
        if ((id == 0) && (m->dst != peer))
            continue;
        if ((pick == NULL) || (m->time <= pick->time))
            pick = m;
    }
    if (pick != NULL) {
        pick->state = MSG_STATE_DELIVERED;
        pick->time = now;
        touch();
    }
    unlock();
}

/* -------------------------------------------------------------- inbound --- */

int messaging_received(enum msgTransport transport, uint32_t src, uint32_t dst,
                       const char *text, bool confirm, uint32_t now)
{
    if (transport >= MSG_TRANSPORT_COUNT)
        return -1;

    lock();
    /* newest first: shift the older ones up, dropping the oldest */
    unsigned n = (inboxCount < MSG_INBOX_LEN) ? inboxCount : (MSG_INBOX_LEN - 1);

    memmove(&inbox[1], &inbox[0], n * sizeof(inbox[0]));
    struct message *m = &inbox[0];

    memset(m, 0, sizeof(*m));
    m->id = nextId++;
    if (nextId == 0)
        nextId = 1;
    m->transport = (uint8_t)transport;
    m->state = MSG_STATE_DELIVERED; /* it arrived: that is its final state */
    m->src = src;
    m->dst = dst;
    m->time = now;
    m->flags = MSG_FLAG_INBOUND | MSG_FLAG_UNREAD;
    if (confirm)
        m->flags |= MSG_FLAG_CONFIRM;
    copyText(m, text);
    if (inboxCount < MSG_INBOX_LEN)
        inboxCount++;
    if (inboxUnread < inboxCount)
        inboxUnread++;
    int id = (int)m->id;

    touch();
    unlock();
    return id;
}

const struct message *messaging_inbox(int i)
{
    if ((i < 0) || ((unsigned)i >= inboxCount))
        return NULL;
    return &inbox[i];
}

const struct message *messaging_outbox(int i)
{
    if (i < 0)
        return NULL;

    /* Dense: the i-th entry in use, skipping the free slots between them */
    for (int k = 0; k < MSG_OUTBOX_LEN; k++) {
        if (outbox[k].state == MSG_STATE_FREE)
            continue;
        if (i == 0)
            return &outbox[k];
        i--;
    }
    return NULL;
}

int messaging_outCount(void)
{
    int n = 0;

    lock();
    for (int i = 0; i < MSG_OUTBOX_LEN; i++)
        if (outbox[i].state != MSG_STATE_FREE)
            n++;
    unlock();
    return n;
}

int messaging_unread(void)
{
    return (int)inboxUnread;
}

void messaging_markRead(void)
{
    lock();
    for (unsigned i = 0; i < inboxCount; i++)
        inbox[i].flags &= (uint8_t)~MSG_FLAG_UNREAD;
    inboxUnread = 0;
    touch();
    unlock();
}

void messaging_markReadId(int id)
{
    lock();
    for (unsigned i = 0; i < inboxCount; i++) {
        if ((inbox[i].id == (uint8_t)id)
            && ((inbox[i].flags & MSG_FLAG_UNREAD) != 0)) {
            inbox[i].flags &= (uint8_t)~MSG_FLAG_UNREAD;
            if (inboxUnread > 0)
                inboxUnread--;
            touch();
            break;
        }
    }
    unlock();
}

void messaging_cancel(int id, uint32_t now)
{
    lock();
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        struct message *m = &outbox[i];

        if ((m->id != (uint8_t)id) || (m->state == MSG_STATE_FREE))
            continue;
        if ((m->state == MSG_STATE_DELIVERED)
            || (m->state == MSG_STATE_SENT)
            || (m->state == MSG_STATE_CANCELLED))
            break; /* already over: nothing to cancel */
        m->state = MSG_STATE_CANCELLED;
        m->time = now;
        touch();
        break;
    }
    unlock();
}

void messaging_clear(void)
{
    lock();
    memset(outbox, 0, sizeof(outbox));
    memset(inbox, 0, sizeof(inbox));
    inboxCount = 0;
    inboxUnread = 0;
    memset(&draft, 0, sizeof(draft));
    draftUsed = false;
    dirty = true;
    lastChange = 0;
    unlock();
    messaging_store_clear();
}

/* --------------------------------------------------------------- draft --- */

bool messaging_ready(void)
{
    return initialised;
}

struct message *messaging_draft(void)
{
    return &draft;
}

void messaging_draftTouch(void)
{
    lock();
    size_t len = 0;

    while ((len < MSG_TEXT_MAX) && (draft.text[len] != '\0'))
        len++;
    draft.textLen = (uint8_t)len;
    draftUsed = (len > 0) || (draft.peer[0] != '\0');
    touch();
    unlock();
}

void messaging_draftClear(void)
{
    lock();
    memset(&draft, 0, sizeof(draft));
    draftUsed = false;
    touch();
    unlock();
}

/* ---------------------------------------------------------- persistence --- */

static uint16_t crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)buf[i] << 8);
        for (int k = 0; k < 8; k++)
            crc = ((crc & 0x8000u) != 0) ? (uint16_t)((crc << 1) ^ 0x1021)
                                         : (uint16_t)(crc << 1);
    }
    return crc;
}

static void put3(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static uint32_t get3(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/**
 * Serialise one message into the record.
 *
 * @return the new write position, 0 if the entry does not fit in `cap`.
 */
static size_t putMessage(uint8_t *buf, size_t cap, size_t n,
                         const struct message *m)
{
    /*
     * The length is taken from the string, not from textLen: the compose draft
     * is edited in place by the screen, which only guarantees that the string
     * is current and NUL terminated.
     */
    size_t len = 0;

    while ((len < MSG_TEXT_MAX) && (m->text[len] != '\0'))
        len++;
    if (n + ENTRY_HEAD + len > cap)
        return 0;

    buf[n++] = m->id;
    buf[n++] = m->transport;
    buf[n++] = m->state;
    buf[n++] = m->flags;
    buf[n++] = m->attempts;
    put3(buf + n, m->src);
    n += 3;
    put3(buf + n, m->dst);
    n += 3;
    buf[n++] = (uint8_t)(m->time >> 24);
    buf[n++] = (uint8_t)(m->time >> 16);
    buf[n++] = (uint8_t)(m->time >> 8);
    buf[n++] = (uint8_t)m->time;
    buf[n++] = (uint8_t)(m->ref >> 8);
    buf[n++] = (uint8_t)m->ref;
    buf[n++] = (uint8_t)len;
    memcpy(buf + n, m->peer, sizeof(m->peer));
    n += sizeof(m->peer);
    memcpy(buf + n, m->text, len);
    n += len;
    return n;
}

/**
 * Read one message back.
 *
 * @return false if the entry runs past `end`, in which case the caller throws
 *         the whole record away: a half read entry is not a message.
 */
static bool getMessage(const uint8_t *buf, size_t end, size_t *pos,
                       struct message *m)
{
    size_t n = *pos;

    memset(m, 0, sizeof(*m));
    if (n + ENTRY_HEAD > end)
        return false;

    m->id = buf[n++];
    m->transport = buf[n++];
    m->state = buf[n++];
    m->flags = buf[n++];
    m->attempts = buf[n++];
    m->src = get3(buf + n);
    n += 3;
    m->dst = get3(buf + n);
    n += 3;
    m->time = ((uint32_t)buf[n] << 24) | ((uint32_t)buf[n + 1] << 16)
              | ((uint32_t)buf[n + 2] << 8) | buf[n + 3];
    n += 4;
    m->ref = (uint16_t)(((uint16_t)buf[n] << 8) | buf[n + 1]);
    n += 2;
    m->textLen = buf[n++];
    if ((m->textLen > MSG_TEXT_MAX)
        || (m->transport >= MSG_TRANSPORT_COUNT)
        || (n + sizeof(m->peer) + m->textLen > end))
        return false;
    memcpy(m->peer, buf + n, sizeof(m->peer));
    m->peer[sizeof(m->peer) - 1] = '\0';
    n += sizeof(m->peer);
    memcpy(m->text, buf + n, m->textLen);
    m->text[m->textLen] = '\0';
    n += m->textLen;
    *pos = n;
    return true;
}

int messaging_serialise(uint8_t *buf, size_t cap)
{
    if (buf == NULL)
        return -1;

    lock();
    size_t n = 8;
    unsigned outCount = 0;

    if (cap < n)
        goto tooBig;

    for (int i = 0; i < MSG_OUTBOX_LEN; i++)
        if (outbox[i].state != MSG_STATE_FREE)
            outCount++;

    memcpy(buf, "MSG1", 4);
    buf[4] = MSG_RECORD_VERSION;
    buf[5] = (uint8_t)inboxCount;
    buf[6] = (uint8_t)outCount;
    buf[7] = (uint8_t)((inboxUnread > 255) ? 255 : inboxUnread);

    /* The outbox is written packed, without its free slots */
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        if (outbox[i].state == MSG_STATE_FREE)
            continue;
        n = putMessage(buf, cap, n, &outbox[i]);
        if (n == 0)
            goto tooBig;
    }

    for (unsigned i = 0; i < inboxCount; i++) {
        n = putMessage(buf, cap, n, &inbox[i]);
        if (n == 0)
            goto tooBig;
    }

    /* The compose draft: a marker byte and, when there is one, its entry */
    if (n + 1 > cap)
        goto tooBig;
    buf[n++] = (draftUsed != 0) ? 1u : 0u;
    if (draftUsed) {
        n = putMessage(buf, cap, n, &draft);
        if (n == 0)
            goto tooBig;
    }

    if (n + 2 > cap)
        goto tooBig;
    uint16_t crc = crc16(buf, n);

    buf[n++] = (uint8_t)(crc >> 8);
    buf[n++] = (uint8_t)crc;
    unlock();
    return (int)n;

tooBig:
    unlock();
    return -1;
}

int messaging_deserialise(const uint8_t *buf, size_t len)
{
    bool usable = (buf != NULL) && (len >= 11)
                  && (memcmp(buf, "MSG1", 4) == 0)
                  && (buf[4] == MSG_RECORD_VERSION);

    if (usable) {
        uint16_t want = (uint16_t)(((uint16_t)buf[len - 2] << 8) | buf[len - 1]);

        usable = (crc16(buf, len - 2) == want)
                 && (buf[6] <= MSG_OUTBOX_LEN) && (buf[5] <= MSG_INBOX_LEN);
    }

    if (!usable) {
        /* a record that does not check out must not leave stale entries */
        lock();
        memset(outbox, 0, sizeof(outbox));
        memset(inbox, 0, sizeof(inbox));
        memset(&draft, 0, sizeof(draft));
        draftUsed = false;
        inboxCount = 0;
        inboxUnread = 0;
        unlock();
        return -1;
    }

    unsigned outCount = buf[6];
    unsigned inCount = buf[5];

    lock();
    memset(outbox, 0, sizeof(outbox));
    memset(inbox, 0, sizeof(inbox));
    memset(&draft, 0, sizeof(draft));
    draftUsed = false;
    inboxCount = 0;
    inboxUnread = buf[7];

    size_t n = 8;
    int restored = 0;

    for (unsigned i = 0; i < outCount; i++) {
        if (!getMessage(buf, len - 2, &n, &outbox[i]))
            goto bad;
        /*
         * A message that was on the air when the radio lost power has no
         * known outcome: queue it again rather than claim it was sent.
         */
        if (outbox[i].state == MSG_STATE_SENDING)
            outbox[i].state = MSG_STATE_QUEUED;
        restored++;
    }

    for (unsigned i = 0; i < inCount; i++) {
        if (!getMessage(buf, len - 2, &n, &inbox[i]))
            goto bad;
        inboxCount++;
        restored++;
    }

    if (n + 1 > len - 2)
        goto bad;
    if (buf[n++] != 0) {
        if (!getMessage(buf, len - 2, &n, &draft))
            goto bad;
        draftUsed = true;
    }

    dirty = false;
    lastChange = 0;
    unlock();
    return restored;

bad:
    memset(outbox, 0, sizeof(outbox));
    memset(inbox, 0, sizeof(inbox));
    memset(&draft, 0, sizeof(draft));
    draftUsed = false;
    inboxCount = 0;
    inboxUnread = 0;
    unlock();
    return -1;
}

void messaging_task(uint32_t now)
{
    /*
     * A message whose confirmation never came is given up on here: the
     * transports have no timer of their own, and the UI asks the model.
     */
    lock();
    for (int i = 0; i < MSG_OUTBOX_LEN; i++) {
        struct message *m = &outbox[i];

        if (m->state != MSG_STATE_SENDING)
            continue;
        if ((now - m->time) < MSG_CONFIRM_TIMEOUT_MS)
            continue;
        m->state = (m->attempts >= MSG_MAX_ATTEMPTS) ? MSG_STATE_FAILED
                                                     : MSG_STATE_QUEUED;
        m->time = now;
        dirty = true;
    }
    unlock();

    if (!dirty)
        return;
    if ((now - lastChange) < MSG_WRITE_DEBOUNCE_MS)
        return;

    int n = messaging_serialise(recordBuf, sizeof(recordBuf));

    if (n <= 0)
        return;
    if (messaging_store_write(recordBuf, (size_t)n)) {
        lock();
        dirty = false;
        lastChange = now;
        unlock();
    }
}

bool messaging_dirty(void)
{
    return dirty;
}

/* --------------------------------------------------------------- names --- */

const char *messaging_transportName(enum msgTransport transport)
{
    switch (transport) {
        case MSG_TRANSPORT_APRS:  return "APRS";
        case MSG_TRANSPORT_DMR:   return "DMR";
        case MSG_TRANSPORT_TETRA: return "TETRA SDS";
        default:                  return "?";
    }
}

const char *messaging_stateName(enum msgState state)
{
    switch (state) {
        case MSG_STATE_FREE:      return "free";
        case MSG_STATE_QUEUED:    return "queued";
        case MSG_STATE_SENDING:   return "sending";
        case MSG_STATE_SENT:      return "sent";
        case MSG_STATE_DELIVERED: return "delivered";
        case MSG_STATE_FAILED:    return "failed";
        case MSG_STATE_CANCELLED: return "cancelled";
    }
    return "?";
}
