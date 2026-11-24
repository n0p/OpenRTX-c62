/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/aprs_msg.h"
#include <string.h>
#include <stdio.h>

void aprs_mbox_init(aprs_mbox_t *m)
{
    memset(m, 0, sizeof(*m));
    m->nextId = 1;
}

static void copyStr(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool aprs_mbox_queue(aprs_mbox_t *m, const char *peer, const char *text,
                     uint32_t nowMs)
{
    size_t peerLen = strlen(peer);
    size_t textLen = strlen(text);

    if ((peerLen == 0) || (peerLen > 9) || (textLen == 0)
        || (textLen > APRS_MSG_TEXT_MAX))
        return false;

    for (uint8_t i = 0; i < APRS_OUTBOX_MAX; i++)
    {
        aprs_out_msg_t *o = &m->out[i];
        if (o->inUse)
            continue;

        memset(o, 0, sizeof(*o));
        copyStr(o->peer, sizeof(o->peer), peer);
        copyStr(o->text, sizeof(o->text), text);
        snprintf(o->msgNo, sizeof(o->msgNo), "%u", m->nextId);
        m->nextId = (uint8_t)((m->nextId % 99u) + 1u);
        o->createdMs = nowMs;
        o->nextTxMs  = nowMs;
        o->inUse     = true;
        return true;
    }

    return false;
}

aprs_out_msg_t *aprs_mbox_nextTx(aprs_mbox_t *m, uint32_t nowMs)
{
    /* Channel courtesy: a minimum gap between any two transmissions */
    if (m->lastTxValid
        && ((uint32_t)(nowMs - m->lastTxMs) < APRS_TX_MIN_GAP_MS))
        return NULL;

    aprs_out_msg_t *best = NULL;
    for (uint8_t i = 0; i < APRS_OUTBOX_MAX; i++)
    {
        aprs_out_msg_t *o = &m->out[i];
        if (!o->inUse || o->acked || o->failed)
            continue;
        if ((int32_t)(nowMs - o->nextTxMs) < 0)
            continue;
        if (best == NULL || (int32_t)(o->nextTxMs - best->nextTxMs) < 0)
            best = o;
    }

    return best;
}

void aprs_mbox_markTx(aprs_mbox_t *m, aprs_out_msg_t *msg, uint32_t nowMs)
{
    if (msg == NULL)
        return;

    m->lastTxMs    = nowMs;
    m->lastTxValid = true;

    msg->attempts++;
    if (msg->attempts >= APRS_MAX_ATTEMPTS)
    {
        msg->failed   = true;
        msg->nextTxMs = nowMs;
        return;
    }

    uint32_t backoff = APRS_RETRY_BASE_MS << (msg->attempts - 1u);
    if (backoff > APRS_RETRY_MAX_MS)
        backoff = APRS_RETRY_MAX_MS;

    msg->nextTxMs = nowMs + backoff;
}

aprs_out_msg_t *aprs_mbox_onAck(aprs_mbox_t *m, const char *from,
                                const char *msgNo, bool rejected)
{
    for (uint8_t i = 0; i < APRS_OUTBOX_MAX; i++)
    {
        aprs_out_msg_t *o = &m->out[i];
        if (!o->inUse || o->acked || o->failed)
            continue;
        if (strcmp(o->peer, from) != 0)
            continue;
        if (strcmp(o->msgNo, msgNo) != 0)
            continue;

        if (rejected)
            o->failed = true;
        else
            o->acked = true;

        return o;
    }

    return NULL;
}

static bool sameMessage(const aprs_in_msg_t *e, const char *from,
                        const char *msgNo)
{
    if (strcmp(e->from, from) != 0)
        return false;

    /* A message without a number can only be compared by its text */
    if (msgNo[0] == '\0')
        return false;

    return strcmp(e->msgNo, msgNo) == 0;
}

bool aprs_mbox_onMessage(aprs_mbox_t *m, const char *from, const char *text,
                         const char *msgNo, uint32_t nowMs)
{
    for (uint8_t i = 0; i < APRS_INBOX_MAX; i++)
    {
        if (m->in[i].inUse && sameMessage(&m->in[i], from, msgNo))
            return false;   /* duplicate: retransmission of a message we have */
    }

    for (uint8_t i = 0; i < APRS_INBOX_MAX; i++)
    {
        aprs_in_msg_t *e = &m->in[i];
        if (e->inUse)
            continue;

        memset(e, 0, sizeof(*e));
        copyStr(e->from, sizeof(e->from), from);
        copyStr(e->text, sizeof(e->text), text);
        copyStr(e->msgNo, sizeof(e->msgNo), msgNo);
        e->rxMs  = nowMs;
        e->inUse = true;
        if (m->unread < 0xFFFFu)
            m->unread++;
        return true;
    }

    /* Inbox full: overwrite the oldest entry, which is the one most likely to
     * be stale, and count it as new. */
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < APRS_INBOX_MAX; i++)
    {
        if ((int32_t)(m->in[i].rxMs - m->in[oldest].rxMs) < 0)
            oldest = i;
    }

    aprs_in_msg_t *e = &m->in[oldest];
    if (e->read == false && m->unread > 0)
        m->unread--;

    memset(e, 0, sizeof(*e));
    copyStr(e->from, sizeof(e->from), from);
    copyStr(e->text, sizeof(e->text), text);
    copyStr(e->msgNo, sizeof(e->msgNo), msgNo);
    e->rxMs  = nowMs;
    e->inUse = true;
    m->unread++;
    return true;
}

void aprs_mbox_markRead(aprs_mbox_t *m, uint8_t index)
{
    if ((index >= APRS_INBOX_MAX) || (m->in[index].inUse == false))
        return;

    if (!m->in[index].read)
    {
        m->in[index].read = true;
        if (m->unread > 0)
            m->unread--;
    }
}

void aprs_mbox_tick(aprs_mbox_t *m, uint32_t nowMs)
{
    for (uint8_t i = 0; i < APRS_OUTBOX_MAX; i++)
    {
        aprs_out_msg_t *o = &m->out[i];

        /* Free acknowledged messages once their ACK is consumed; the caller
         * reads the flags before the slot is reused. */
        if (o->inUse && (o->acked || o->failed))
        {
            /* Keep them for one more tick so the UI can show the result */
            if ((uint32_t)(nowMs - o->nextTxMs) > 30000u)
                memset(o, 0, sizeof(*o));
        }
    }
}
