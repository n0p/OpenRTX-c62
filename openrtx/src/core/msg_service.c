/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/msg_service.h"
#include "core/state.h"
#include "rtx/rtx.h"
#include "interfaces/delays.h"
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_APRS
#include "core/aprs_service.h"
#endif

/*
 * Transport table. The UI never talks to a protocol: it asks for what is
 * available and queues text.
 *
 * The APRS backend is complete: aprs_sendMessage() puts the text in the
 * service outbox, which owns the channel gap and the retry schedule, and
 * poll() reads the result back into the persistent store. The DMR and TETRA
 * backends are the hooks the SMS/SDS ports fill in.
 */

#define APRS_TEXT_MAX 67
#define DMR_SMS_TEXT_MAX 160
#define TETRA_SDS_TEXT_MAX 140

__attribute__((weak)) bool msg_dmr_sms_available(void)
{
    return false;
}

__attribute__((weak)) bool msg_dmr_sms_send(const char *peer, const char *text,
                                            char *ref, size_t refLen)
{
    (void)peer; (void)text; (void)ref; (void)refLen;
    return false;
}

__attribute__((weak)) bool msg_dmr_sms_cancel(const char *ref)
{
    (void)ref;
    return false;
}

__attribute__((weak)) bool msg_tetra_sds_available(void)
{
    return false;
}

__attribute__((weak)) bool msg_tetra_sds_send(const char *peer,
                                              const char *text, char *ref,
                                              size_t refLen)
{
    (void)peer; (void)text; (void)ref; (void)refLen;
    return false;
}

__attribute__((weak)) bool msg_tetra_sds_cancel(const char *ref)
{
    (void)ref;
    return false;
}

const char *msg_service_name(uint8_t tp)
{
    switch (tp) {
        case MSG_TP_APRS:      return "APRS";
        case MSG_TP_DMR_SMS:   return "DMR SMS";
        case MSG_TP_TETRA_SDS: return "TETRA SDS";
        default:               return "?";
    }
}

uint8_t msg_service_maxLen(uint8_t tp)
{
    switch (tp) {
        case MSG_TP_APRS:      return APRS_TEXT_MAX;
        case MSG_TP_DMR_SMS:   return DMR_SMS_TEXT_MAX;
        case MSG_TP_TETRA_SDS: return TETRA_SDS_TEXT_MAX;
        default:               return 0;
    }
}

bool msg_service_available(uint8_t tp)
{
    switch (tp) {
        case MSG_TP_APRS:
#ifdef CONFIG_APRS
            return state.channel.mode == OPMODE_FM;
#else
            return false;
#endif

        case MSG_TP_DMR_SMS:
#ifdef CONFIG_DMR
            return (state.channel.mode == OPMODE_DMR) && msg_dmr_sms_available();
#else
            return false;
#endif

        case MSG_TP_TETRA_SDS:
#ifdef CONFIG_TETRA
            return (state.channel.mode == OPMODE_TETRA)
                && msg_tetra_sds_available();
#else
            return false;
#endif

        default:
            return false;
    }
}

uint8_t msg_service_default(void)
{
    /* The current mode first, then anything else that works */
    uint8_t first = state.channel.mode == OPMODE_FM      ? MSG_TP_APRS
                  : state.channel.mode == OPMODE_DMR     ? MSG_TP_DMR_SMS
                  : state.channel.mode == OPMODE_TETRA   ? MSG_TP_TETRA_SDS
                                                         : MSG_TP_NUM;

    if (first != MSG_TP_NUM && msg_service_available(first))
        return first;

    for (uint8_t tp = 0; tp < MSG_TP_NUM; tp++) {
        if (msg_service_available(tp))
            return tp;
    }
    return MSG_TP_NUM;
}

const char *msg_service_statusText(uint8_t status)
{
    switch (status) {
        case MSG_ST_QUEUED:    return "QUEUED";
        case MSG_ST_TX:        return "TX";
        case MSG_ST_SENT:      return "SENT";
        case MSG_ST_ACKED:     return "ACK";
        case MSG_ST_FAILED:    return "FAIL";
        case MSG_ST_CANCELLED: return "CANC";
        default:               return "-";
    }
}

void msg_service_init(void)
{
    msg_init();
}

#ifdef CONFIG_APRS

/* The APRS outbox entry of a stored message, by message number. */
static aprs_out_msg_t *aprsEntry(const char *ref)
{
    if (ref[0] == '\0')
        return NULL;

    aprs_mbox_t *m = aprs_mailbox();
    for (uint8_t i = 0; i < APRS_OUTBOX_MAX; i++) {
        if (m->out[i].inUse && strncmp(m->out[i].msgNo, ref, MSG_REF_LEN) == 0)
            return &m->out[i];
    }
    return NULL;
}

static bool aprsQueue(const char *peer, const char *text, char *ref,
                      size_t refLen)
{
    if (!aprs_sendMessage(peer, text, (uint32_t)getTick()))
        return false;

    /* Recover the message number so that poll() can follow this very entry */
    aprs_mbox_t *m = aprs_mailbox();
    aprs_out_msg_t *found = NULL;
    for (uint8_t i = 0; i < APRS_OUTBOX_MAX; i++) {
        aprs_out_msg_t *o = &m->out[i];
        if (!o->inUse || o->acked || o->failed)
            continue;
        if (strncmp(o->peer, peer, sizeof(o->peer)) == 0
            && strncmp(o->text, text, sizeof(o->text)) == 0)
            found = o;
    }

    if (found == NULL)
        return true; /* queued, we just cannot follow the retries */

    snprintf(ref, refLen, "%s", found->msgNo);
    return true;
}

/* Refresh the state of every APRS message we have stored. */
static void aprsPoll(uint32_t now)
{
    aprs_mbox_t *m = aprs_mailbox();
    aprs_mbox_tick(m, now);

    bool changed = false;

    for (uint8_t i = 0; i < MSG_SENT_MAX; i++) {
        msg_rec_t *r = msg_sent_at(i);
        if ((r == NULL) || (r->tp != MSG_TP_APRS))
            continue;
        if (r->status == MSG_ST_ACKED || r->status == MSG_ST_FAILED
            || r->status == MSG_ST_CANCELLED)
            continue;

        aprs_out_msg_t *o = aprsEntry(r->ref);
        uint8_t st = r->status;
        uint8_t attempts = r->attempts;

        if (o != NULL) {
            attempts = o->attempts;
            if (o->acked)
                st = MSG_ST_ACKED;
            else if (o->failed)
                st = MSG_ST_FAILED;
            else if (o->attempts == 0)
                st = MSG_ST_QUEUED;
            else if (m->lastTxValid && (uint32_t)(now - m->lastTxMs) < 1500)
                st = MSG_ST_TX;
            else
                st = MSG_ST_SENT;
        } else if (r->status != MSG_ST_QUEUED && r->ref[0] != '\0') {
            /* The service dropped it: no more retries */
            st = MSG_ST_FAILED;
        }

        if (st != r->status || attempts != r->attempts) {
            r->status = st;
            r->attempts = attempts;
            changed = true;
        }
    }

    /* Mirror what the receive path stored into the persistent inbox */
    for (uint8_t i = 0; i < APRS_INBOX_MAX; i++) {
        aprs_in_msg_t *e = &m->in[i];
        if (!e->inUse)
            continue;

        bool seen = false;
        for (uint8_t k = 0; k < MSG_INBOX_MAX && !seen; k++) {
            msg_bank_t *b = msg_bank();
            seen = (b->in[k].seq != 0)
                && (strncmp(b->in[k].peer, e->from, MSG_PEER_LEN) == 0)
                && (strncmp(b->in[k].ref, e->msgNo, MSG_REF_LEN) == 0);
        }
        if (seen)
            continue;

        msg_rec_t *r = msg_addIn(MSG_TP_APRS, e->from, e->text, e->rxMs);
        if (r != NULL) {
            snprintf(r->ref, sizeof(r->ref), "%s", e->msgNo);
            msg_save();
        }
    }

    /* Delivery states changed: one write for the whole pass */
    if (changed)
        msg_save();
}

#endif /* CONFIG_APRS */

msg_rec_t *msg_service_queue(uint8_t tp, const char *peer, const char *text,
                             uint32_t now)
{
    if ((peer == NULL) || (text == NULL) || (text[0] == '\0')
        || !msg_service_available(tp))
        return NULL;

    if (strlen(text) > msg_service_maxLen(tp))
        return NULL;

    char ref[MSG_REF_LEN] = "";
    bool queued = false;

    switch (tp) {
        case MSG_TP_APRS:
#ifdef CONFIG_APRS
            queued = aprsQueue(peer, text, ref, sizeof(ref));
#endif
            break;

        case MSG_TP_DMR_SMS:
            queued = msg_dmr_sms_send(peer, text, ref, sizeof(ref));
            break;

        case MSG_TP_TETRA_SDS:
            queued = msg_tetra_sds_send(peer, text, ref, sizeof(ref));
            break;

        default:
            break;
    }

    if (!queued)
        return NULL;

    msg_rec_t *r = msg_addOut(tp, peer, text, now);
    if (r == NULL)
        return NULL;

    snprintf(r->ref, sizeof(r->ref), "%s", ref);
    r->status = MSG_ST_QUEUED;
    msg_save();
    return r;
}

bool msg_service_cancel(msg_rec_t *rec)
{
    if (rec == NULL || rec->seq == 0)
        return false;
    if (rec->status == MSG_ST_CANCELLED || rec->status == MSG_ST_ACKED)
        return false;

    switch (rec->tp) {
        case MSG_TP_APRS:
#ifdef CONFIG_APRS
        {
            aprs_out_msg_t *o = aprsEntry(rec->ref);
            if (o != NULL)
                o->inUse = false;
        }
#endif
            break;

        case MSG_TP_DMR_SMS:
            msg_dmr_sms_cancel(rec->ref);
            break;

        case MSG_TP_TETRA_SDS:
            msg_tetra_sds_cancel(rec->ref);
            break;

        default:
            break;
    }

    rec->status = MSG_ST_CANCELLED;
    msg_save();
    return true;
}

void msg_service_poll(uint32_t now)
{
#ifdef CONFIG_APRS
    aprsPoll(now);
#else
    (void)now;
#endif
}
