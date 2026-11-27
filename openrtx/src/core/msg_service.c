/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <string.h>

#include "core/msg_service.h"
#include "core/state.h"
#include "core/contact_store.h"
#include "interfaces/delays.h"

#ifdef CONFIG_APRS
#include "core/aprs_service.h"
#endif

/*
 * Transport table and address translation.  The store, the queue and the
 * states are core/messaging.c; nothing here keeps a second copy of them.
 */

#define APRS_TEXT_MAX 67
#define DMR_SMS_TEXT_MAX 160
#define TETRA_SDS_TEXT_MAX 140
/* A D-STAR DV transmission carries a 20-character label, not a message */
#define DSTAR_TEXT_MAX 20

const char *msg_service_name(enum msgTransport tp)
{
    switch (tp) {
        case MSG_TRANSPORT_APRS:  return "APRS";
        case MSG_TRANSPORT_DMR:   return "DMR SMS";
        case MSG_TRANSPORT_TETRA: return "TETRA SDS";
        case MSG_TRANSPORT_DSTAR: return "D-STAR";
        default:                  return "?";
    }
}

uint8_t msg_service_maxLen(enum msgTransport tp)
{
    switch (tp) {
        case MSG_TRANSPORT_APRS:  return APRS_TEXT_MAX;
        case MSG_TRANSPORT_DMR:   return DMR_SMS_TEXT_MAX;
        case MSG_TRANSPORT_TETRA: return TETRA_SDS_TEXT_MAX;
        case MSG_TRANSPORT_DSTAR: return DSTAR_TEXT_MAX;
        default:                  return 0;
    }
}

bool msg_service_available(enum msgTransport tp)
{
    switch (tp) {
        case MSG_TRANSPORT_APRS:
#ifdef CONFIG_APRS
            return state.channel.mode == OPMODE_FM;
#else
            return false;
#endif

        case MSG_TRANSPORT_DMR:
            /*
             * Without the transmit chain a message would sit in the outbox for
             * ever: SMS reception alone is not something to send with.
             */
#if defined(CONFIG_DMR_SMS) && defined(CONFIG_DMR_TX)
            return state.channel.mode == OPMODE_DMR;
#else
            return false;
#endif

        case MSG_TRANSPORT_TETRA:
            /*
             * There is no TETRA transmit chain in this image: the SDS is
             * encoded and left queued (tetraSds_prepareQueued), which is what
             * the mode documents.  The screen still lets it be written.
             */
#ifdef CONFIG_TETRA
            return state.channel.mode == OPMODE_TETRA;
#else
            return false;
#endif

        case MSG_TRANSPORT_DSTAR:
            /*
             * D-STAR has no addressed messaging: the text is a 20-character
             * label carried in the slow data of the operator's own DV
             * transmission, so it can only be queued while that mode is
             * selected and its transmitter is built in.
             */
#if defined(CONFIG_DSTAR) && defined(CONFIG_DSTAR_TX)
            return state.channel.mode == OPMODE_DSTAR;
#else
            return false;
#endif

        default:
            return false;
    }
}

enum msgTransport msg_service_default(void)
{
    /* The current mode first, then anything else that works */
    enum msgTransport first = MSG_TRANSPORT_COUNT;

    if (state.channel.mode == OPMODE_FM)
        first = MSG_TRANSPORT_APRS;
    else if (state.channel.mode == OPMODE_DMR)
        first = MSG_TRANSPORT_DMR;
    else if (state.channel.mode == OPMODE_TETRA)
        first = MSG_TRANSPORT_TETRA;
    else if (state.channel.mode == OPMODE_DSTAR)
        first = MSG_TRANSPORT_DSTAR;

    if ((first != MSG_TRANSPORT_COUNT) && msg_service_available(first))
        return first;

    for (uint8_t tp = 0; tp < MSG_TRANSPORT_COUNT; tp++) {
        if (msg_service_available((enum msgTransport)tp))
            return (enum msgTransport)tp;
    }
    return MSG_TRANSPORT_COUNT;
}

const char *msg_service_statusText(uint8_t state)
{
    switch (state) {
        case MSG_STATE_QUEUED:    return "QUEUED";
        case MSG_STATE_SENDING:   return "TX";
        case MSG_STATE_SENT:      return "SENT";
        case MSG_STATE_DELIVERED: return "ACK";
        case MSG_STATE_FAILED:    return "FAIL";
        case MSG_STATE_CANCELLED: return "CANC";
        default:                  return "-";
    }
}

void msg_service_init(void)
{
    /*
     * Opening the screen must never reload the record over live messages: the
     * store is built once, by whoever gets there first (state_init() on the
     * radio, the test on the host).
     */
    if (!messaging_ready())
        messaging_init();
}

/* -------------------------------------------------------- addresses --- */

/** A decimal number the operator typed, 0 if the string is not one. */
static uint32_t parseNumber(const char *s)
{
    uint32_t v = 0;

    if ((s == NULL) || (s[0] == '\0'))
        return 0;

    for (const char *p = s; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p))
            return 0;
        v = (v * 10u) + (uint32_t)(*p - '0');
        if (v > 0xFFFFFFu)   /* a DMR ID and an SSI are 24 bits at most */
            return 0;
    }
    return v;
}

/**
 * The address a name stands for: the number of a contact in the book, or the
 * digits themselves.  0 when the name is neither.
 */
static uint32_t resolveNumber(const char *peer)
{
    uint16_t idx = cstore_findCall(peer);

    if (idx != CSTORE_NONE) {
        cstore_contact_t c;

        if ((cstore_get(idx, &c) == 0) && (c.dmr_id != 0))
            return c.dmr_id;
    }
    return parseNumber(peer);
}

/* ------------------------------------------------------------- queue --- */

int msg_service_queue(enum msgTransport tp, const char *peer, const char *text,
                      bool confirm, uint32_t now)
{
    if ((peer == NULL) || (text == NULL) || (text[0] == '\0'))
        return -1;
    if (strlen(peer) >= MSG_PEER_LEN)
        return -1;
    if (!msg_service_available(tp))
        return -1;
    if (strlen(text) > msg_service_maxLen(tp))
        return -1;

    switch (tp) {
        case MSG_TRANSPORT_APRS:
            /* A callsign is the address: the store keeps it as text */
            return messaging_sendToText(MSG_TRANSPORT_APRS, peer, text, confirm,
                                        now);

        case MSG_TRANSPORT_DMR:
        case MSG_TRANSPORT_TETRA: {
            uint32_t dst = resolveNumber(peer);

            if (dst == 0)
                return -1;   /* no contact and not a number: nothing to dial */
            return messaging_sendAddr(tp, dst, peer, text, confirm, now);
        }

        default:
            return -1;
    }
}

bool msg_service_cancel(int id, uint32_t now)
{
    /*
     * Everything this needs from the message is taken by value before the
     * cancel: the entry's slot can be reused the moment messaging_cancel()
     * returns, and reading the text through the pointer afterwards cancelled
     * messages by a text nobody had written yet.
     */
    struct message m;

    if (messaging_copy(id, &m) != 0)
        return false;
    if ((m.state == MSG_STATE_DELIVERED) || (m.state == MSG_STATE_SENT)
        || (m.state == MSG_STATE_CANCELLED))
        return false;

    messaging_cancel(id, now);

#ifdef CONFIG_APRS
    /*
     * APRS keeps its own retry schedule in the FM mailbox: drop the entry
     * there too, or the service would send it anyway.
     */
    if (m.transport == MSG_TRANSPORT_APRS) {
        aprs_mbox_t *box = aprs_mailbox();

        for (uint8_t i = 0; i < APRS_OUTBOX_MAX; i++) {
            aprs_out_msg_t *o = &box->out[i];

            if (!o->inUse)
                continue;
            if ((strncmp(o->peer, m.peer, sizeof(o->peer)) == 0)
                && (strncmp(o->text, m.text, sizeof(o->text)) == 0))
                o->inUse = false;
        }
    }
#endif

    return true;
}

/* --------------------------------------------------------------- APRS --- */

#ifdef CONFIG_APRS

/**
 * The APRS mailbox is the transport's own buffer: this keeps the store the one
 * place the screen reads.
 *
 *  - inbound: an unread mailbox entry becomes a received message in the store
 *    and is marked read there, so it is shown once;
 *  - outbound: the mailbox knows whether the peer acknowledged the packet or
 *    the retries ran out, and that becomes the message state.
 */
static void aprsSync(uint32_t now)
{
    aprs_mbox_t *box = aprs_mailbox();

    aprs_mbox_tick(box, now);

    for (uint8_t i = 0; i < APRS_INBOX_MAX; i++) {
        aprs_in_msg_t *e = &box->in[i];

        if ((!e->inUse) || e->read)
            continue;

        if (messaging_received(MSG_TRANSPORT_APRS, 0, 0, e->text, false, now) >= 0)
            aprs_mbox_markRead(box, i);
    }

    const int n = messaging_outCount();

    for (int i = 0; i < n; i++) {
        const struct message *m = messaging_outbox(i);

        if ((m == NULL) || (m->transport != MSG_TRANSPORT_APRS))
            continue;
        if ((m->state != MSG_STATE_QUEUED) && (m->state != MSG_STATE_SENDING)
            && (m->state != MSG_STATE_SENT))
            continue;

        for (uint8_t k = 0; k < APRS_OUTBOX_MAX; k++) {
            aprs_out_msg_t *o = &box->out[k];

            if (!o->inUse)
                continue;
            if (strncmp(o->peer, m->peer, sizeof(o->peer)) != 0)
                continue;
            if (strncmp(o->text, m->text, sizeof(o->text)) != 0)
                continue;

            if (o->acked)
                messaging_confirmed(m->id, 0, now);
            else if (o->failed)
                messaging_fail(m->id, now);
            break;
        }
    }
}

#endif /* CONFIG_APRS */

void msg_service_poll(uint32_t now)
{
#ifdef CONFIG_APRS
    aprsSync(now);
#else
    (void)now;
#endif
}
