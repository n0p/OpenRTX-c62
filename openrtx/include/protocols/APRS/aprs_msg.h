/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * APRS messaging: outbox with retries and acknowledgements, inbox with the
 * one-per-peer deduplication an APRS messaging station needs (a sender repeats
 * a message until it is acknowledged, so duplicates are the norm, not the
 * exception).
 *
 * APRS101 chapter 12: a message is ":ADDRESSEE:text{msgno". The receiver
 * answers ":SENDER:ackmsgno" (or "rejmsgno"). Message numbers are per
 * sender-receiver pair and typically run 1..99.
 *
 * The outbox never floods the channel: one transmission at a time, a minimum
 * gap between packets, and exponential backoff between attempts.
 */
#ifndef APRS_MSG_H
#define APRS_MSG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APRS_OUTBOX_MAX      8
#define APRS_INBOX_MAX       16
#define APRS_MSG_TEXT_MAX    67   /* APRS101: 67 characters, no protocol   */
#define APRS_MSG_ID_MAX       6

/**
 * Minimum gap between two APRS transmissions, ms. APRS good practice: never
 * more than one packet per few seconds, and beacons not more often than the
 * configured interval (see aprs_beacon_due()).
 */
#define APRS_TX_MIN_GAP_MS  10000u
/**
 * Retry schedule: attempt n happens 12 s * 2^(n-1) after the previous one,
 * capped at a minute. The base is above the channel gap on purpose: a retry
 * must never make the station transmit faster than the gap allows.
 */
#define APRS_RETRY_BASE_MS  12000u
#define APRS_RETRY_MAX_MS   60000u
#define APRS_MAX_ATTEMPTS       3u

typedef struct
{
    char     peer[10];
    char     text[APRS_MSG_TEXT_MAX + 1];
    char     msgNo[APRS_MSG_ID_MAX];
    uint32_t createdMs;
    uint32_t nextTxMs;
    uint8_t  attempts;
    bool     inUse;
    bool     acked;
    bool     failed;
} aprs_out_msg_t;

typedef struct
{
    char     from[10];
    char     text[APRS_MSG_TEXT_MAX + 1];
    char     msgNo[APRS_MSG_ID_MAX];
    uint32_t rxMs;
    bool     acked;
    bool     read;
    bool     inUse;
} aprs_in_msg_t;

typedef struct
{
    aprs_out_msg_t out[APRS_OUTBOX_MAX];
    aprs_in_msg_t  in[APRS_INBOX_MAX];
    uint8_t  nextId;
    uint32_t lastTxMs;
    bool     lastTxValid;
    uint16_t unread;          /**< unread inbox entries, for the icon */
} aprs_mbox_t;

void aprs_mbox_init(aprs_mbox_t *m);

/**
 * Queue a message for a peer. Fails if the peer is unknown-ly long, the text
 * is empty or too long, or the outbox is full.
 */
bool aprs_mbox_queue(aprs_mbox_t *m, const char *peer, const char *text,
                     uint32_t nowMs);

/**
 * Pick the next message that needs a transmission, honouring the channel gap
 * and the per-message retry schedule. Marks it as transmitted.
 *
 * @param nowMs: uptime.
 * @return a pointer to the message to send, or NULL if nothing is due.
 */
aprs_out_msg_t *aprs_mbox_nextTx(aprs_mbox_t *m, uint32_t nowMs);

/** Mark the message as sent (called after the packet leaves the radio). */
void aprs_mbox_markTx(aprs_mbox_t *m, aprs_out_msg_t *msg, uint32_t nowMs);

/** Process an incoming ack/rej. Returns the acknowledged message or NULL. */
aprs_out_msg_t *aprs_mbox_onAck(aprs_mbox_t *m, const char *from,
                                const char *msgNo, bool rejected);

/**
 * Store an incoming message. Duplicates (same peer, same message number) are
 * dropped, which is what lets the sender retry as much as it wants.
 *
 * @return true if the message is new and must be acknowledged.
 */
bool aprs_mbox_onMessage(aprs_mbox_t *m, const char *from, const char *text,
                         const char *msgNo, uint32_t nowMs);

/** Mark an inbox entry as read. */
void aprs_mbox_markRead(aprs_mbox_t *m, uint8_t index);

/** Keepalive: expire messages that ran out of attempts. */
void aprs_mbox_tick(aprs_mbox_t *m, uint32_t nowMs);

/** Number of unread messages. */
static inline uint16_t aprs_mbox_unread(const aprs_mbox_t *m)
{
    return m->unread;
}

#ifdef __cplusplus
}
#endif

#endif /* APRS_MSG_H */
