/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/msg_store.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define MSGSTORE_MAGIC   0x4D534754U /* "TGSM" */
#define MSGSTORE_VERSION 1

static msg_bank_t bank;
static slot_store_t store;
static const slot_ops_t *port = NULL;
static bool ready = false;

__attribute__((weak)) const slot_ops_t *msg_platformOps(void)
{
    return NULL;
}

void msg_bind(const slot_ops_t *ops)
{
    port = ops;
}

static void sanitize(msg_rec_t *r)
{
    r->peer[MSG_PEER_LEN - 1] = '\0';
    r->text[MSG_TEXT_MAX] = '\0';
    r->ref[MSG_REF_LEN - 1] = '\0';
    if (r->tp >= MSG_TP_NUM)
        r->tp = MSG_TP_APRS;
    if (r->status > MSG_ST_CANCELLED)
        r->status = MSG_ST_FAILED;
}

int msg_init(void)
{
    if (ready)
        return 0;

    if (port == NULL)
        port = msg_platformOps();

    memset(&bank, 0, sizeof(bank));
    bank.nextSeq = 1;

    if (port == NULL) {
        ready = true;
        return -ENODEV;
    }

    if (slot_store_load(&store, port, MSGSTORE_MAGIC, MSGSTORE_VERSION, &bank,
                        sizeof(bank)) != 0)
        memset(&bank, 0, sizeof(bank));
    if (bank.nextSeq == 0)
        bank.nextSeq = 1;

    for (uint8_t i = 0; i < MSG_INBOX_MAX; i++)
        sanitize(&bank.in[i]);
    for (uint8_t i = 0; i < MSG_SENT_MAX; i++)
        sanitize(&bank.out[i]);
    for (uint8_t i = 0; i < MSG_DRAFT_MAX; i++)
        sanitize(&bank.draft[i]);

    ready = true;
    return 0;
}

int msg_save(void)
{
    if (!ready)
        return -EINVAL;
    if (port == NULL)
        return -ENODEV;

    return slot_store_save(&store, &bank, sizeof(bank));
}

void msg_forget(void)
{
    ready = false;
    memset(&bank, 0, sizeof(bank));
}

msg_bank_t *msg_bank(void)
{
    return &bank;
}

uint8_t msg_unread(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < MSG_INBOX_MAX; i++) {
        if (bank.in[i].seq != 0 && bank.in[i].unread)
            n++;
    }
    return n;
}

uint8_t msg_inbox_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < MSG_INBOX_MAX; i++) {
        if (bank.in[i].seq != 0)
            n++;
    }
    return n;
}

uint8_t msg_sent_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < MSG_SENT_MAX; i++) {
        if (bank.out[i].seq != 0)
            n++;
    }
    return n;
}

static msg_rec_t *entryAt(msg_rec_t *list, uint8_t max, uint8_t n)
{
    uint8_t seen = 0;
    for (uint8_t i = 0; i < max; i++) {
        if (list[i].seq == 0)
            continue;
        if (seen == n)
            return &list[i];
        seen++;
    }
    return NULL;
}

msg_rec_t *msg_inbox_at(uint8_t n)
{
    return entryAt(bank.in, MSG_INBOX_MAX, n);
}

msg_rec_t *msg_sent_at(uint8_t n)
{
    return entryAt(bank.out, MSG_SENT_MAX, n);
}

/* Oldest entry of a full list, so that a new message always fits. */
static msg_rec_t *oldest(msg_rec_t *list, uint8_t max)
{
    msg_rec_t *best = NULL;
    for (uint8_t i = 0; i < max; i++) {
        if (list[i].seq == 0)
            return &list[i];
        if (best == NULL || list[i].t_ms < best->t_ms)
            best = &list[i];
    }
    return best;
}

static msg_rec_t *add(msg_rec_t *list, uint8_t max, uint8_t tp, const char *peer,
                      const char *text, uint32_t now, uint8_t status,
                      uint8_t unread)
{
    if ((peer == NULL) || (text == NULL))
        return NULL;

    msg_rec_t *r = oldest(list, max);
    if (r == NULL)
        return NULL;

    memset(r, 0, sizeof(*r));
    snprintf(r->peer, sizeof(r->peer), "%s", peer);
    snprintf(r->text, sizeof(r->text), "%s", text);
    r->tp = tp;
    r->status = status;
    r->unread = unread;
    r->t_ms = now;
    r->seq = bank.nextSeq++;
    if (bank.nextSeq == 0)
        bank.nextSeq = 1;
    return r;
}

msg_rec_t *msg_addOut(uint8_t tp, const char *peer, const char *text,
                      uint32_t now)
{
    if (!ready)
        return NULL;

    msg_rec_t *r = add(bank.out, MSG_SENT_MAX, tp, peer, text, now,
                       MSG_ST_QUEUED, 0);
    if (r != NULL)
        msg_save();
    return r;
}

msg_rec_t *msg_addIn(uint8_t tp, const char *peer, const char *text,
                     uint32_t now)
{
    if (!ready)
        return NULL;

    msg_rec_t *r = add(bank.in, MSG_INBOX_MAX, tp, peer, text, now,
                       MSG_ST_SENT, 1);
    if (r != NULL)
        msg_save();
    return r;
}

msg_rec_t *msg_findSeq(uint16_t seq)
{
    if (seq == 0)
        return NULL;

    for (uint8_t i = 0; i < MSG_SENT_MAX; i++) {
        if (bank.out[i].seq == seq)
            return &bank.out[i];
    }
    for (uint8_t i = 0; i < MSG_INBOX_MAX; i++) {
        if (bank.in[i].seq == seq)
            return &bank.in[i];
    }
    return NULL;
}

void msg_markAllRead(void)
{
    bool changed = false;
    for (uint8_t i = 0; i < MSG_INBOX_MAX; i++) {
        if (bank.in[i].seq != 0 && bank.in[i].unread) {
            bank.in[i].unread = 0;
            changed = true;
        }
    }
    if (changed)
        msg_save();
}

static int deleteAt(msg_rec_t *list, uint8_t max, uint8_t n)
{
    msg_rec_t *r = entryAt(list, max, n);
    if (r == NULL)
        return -EINVAL;
    memset(r, 0, sizeof(*r));
    return msg_save();
}

int msg_deleteIn(uint8_t n)
{
    return deleteAt(bank.in, MSG_INBOX_MAX, n);
}

int msg_deleteOut(uint8_t n)
{
    return deleteAt(bank.out, MSG_SENT_MAX, n);
}

msg_rec_t *msg_draft(void)
{
    return &bank.draft[0];
}

msg_rec_t *msg_draftAlt(void)
{
    return &bank.draft[1];
}

void msg_draftClear(void)
{
    memset(&bank.draft[0], 0, sizeof(bank.draft[0]));
    msg_save();
}
