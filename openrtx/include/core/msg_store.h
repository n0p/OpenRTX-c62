/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSG_STORE_H
#define MSG_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "core/slot_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent message store: inbox, sent folder and the compose draft.
 *
 * This is the radio's own record of what it sent and received, independent of
 * the transport. The transport only contributes the text and the peer; the
 * delivery state (queued, transmitting, sent, acknowledged, failed, cancelled)
 * lives here, so APRS, DMR SMS and TETRA SDS all show the same screens and
 * survive a power cycle the same way.
 *
 * Written through slot_store: two ping-pong slots, CRC, read-back check. A
 * message is never half written: either the new bank verifies or the old one
 * stays.
 */

/** Longest text stored. APRS allows 67, DMR SMS 160, TETRA SDS 140. */
#define MSG_TEXT_MAX 160

/** Peer field: an APRS callsign with SSID is the longest form ("N0CALL-15"). */
#define MSG_PEER_LEN 12

/** Transport reference (APRS message number, DMR/SDS id), NUL terminated. */
#define MSG_REF_LEN 8

#define MSG_INBOX_MAX 16
#define MSG_SENT_MAX  8
#define MSG_DRAFT_MAX 2

/** Direction of a record. */
typedef enum
{
    MSG_DIR_OUT = 0,  /**< Sent folder, or the draft while it is one */
    MSG_DIR_IN        /**< Inbox                                     */
} msg_dir_t;

/** Transport of a message. Must match msg_service.h's msg_transport_t. */
typedef enum
{
    MSG_TP_APRS = 0,
    MSG_TP_DMR_SMS,
    MSG_TP_TETRA_SDS,
    MSG_TP_NUM
} msg_tp_t;

/** Delivery state of an outgoing message. */
typedef enum
{
    MSG_ST_QUEUED = 0, /**< Accepted, waiting for the channel        */
    MSG_ST_TX,         /**< Being transmitted right now              */
    MSG_ST_SENT,       /**< Left the radio, no acknowledgement yet   */
    MSG_ST_ACKED,      /**< Peer acknowledged it                     */
    MSG_ST_FAILED,     /**< Out of retries, or the transport refused */
    MSG_ST_CANCELLED   /**< Cancelled by the user                    */
} msg_status_t;

typedef struct
{
    char     peer[MSG_PEER_LEN];
    char     text[MSG_TEXT_MAX + 1];
    char     ref[MSG_REF_LEN];  /**< Transport reference                  */
    uint32_t t_ms;              /**< Queue/receive time, uptime ms        */
    uint16_t seq;               /**< Monotonic id inside the bank         */
    uint8_t  tp;                /**< enum msg_tp_t                        */
    uint8_t  status;            /**< enum msg_status_t                    */
    uint8_t  attempts;          /**< Transmissions attempted              */
    uint8_t  unread;            /**< Inbox: not read yet                  */
} __attribute__((packed)) msg_rec_t;

typedef struct
{
    uint16_t nextSeq;
    uint16_t reserved[3];
    msg_rec_t in[MSG_INBOX_MAX];
    msg_rec_t out[MSG_SENT_MAX];
    msg_rec_t draft[MSG_DRAFT_MAX];
} __attribute__((packed)) msg_bank_t;

/** Platform hook: flash access for the message store, NULL if none. */
const slot_ops_t *msg_platformOps(void);

/** Bind a port explicitly. Mostly for tests. */
void msg_bind(const slot_ops_t *ops);

/** Load the store. Idempotent; blank or corrupt loads empty. */
int msg_init(void);

/** Write the store back. 0 if it was already on flash. */
int msg_save(void);

/** Drop the in-RAM copy so that the next msg_init() reloads from flash. */
void msg_forget(void);

/** Direct access to the in-RAM bank. */
msg_bank_t *msg_bank(void);

/** Number of unread inbox messages. */
uint8_t msg_unread(void);

/** Number of used entries in the inbox / sent folder. */
uint8_t msg_inbox_count(void);
uint8_t msg_sent_count(void);

/** Pointer to the n-th used inbox / sent entry, NULL past the end. */
msg_rec_t *msg_inbox_at(uint8_t n);
msg_rec_t *msg_sent_at(uint8_t n);

/** Queue an outgoing message. Returns the record (already saved) or NULL. */
msg_rec_t *msg_addOut(uint8_t tp, const char *peer, const char *text,
                      uint32_t now);

/** Store an incoming message as unread. Returns the record or NULL. */
msg_rec_t *msg_addIn(uint8_t tp, const char *peer, const char *text,
                     uint32_t now);

/** Find an outgoing message by its sequence number, NULL if gone. */
msg_rec_t *msg_findSeq(uint16_t seq);

/** Mark every inbox entry as read (called when the inbox is opened). */
void msg_markAllRead(void);

/** Delete inbox entry n (dense index) and save. */
int msg_deleteIn(uint8_t n);

/** Delete sent entry n (dense index) and save. */
int msg_deleteOut(uint8_t n);

/** The compose draft, kept across power cycles. */
msg_rec_t *msg_draft(void);
msg_rec_t *msg_draftAlt(void);

/** Clear (zero) the visible draft. */
void msg_draftClear(void);

#ifdef __cplusplus
}
#endif

#endif /* MSG_STORE_H */
