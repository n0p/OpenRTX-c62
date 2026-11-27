/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_MESSAGING_H
#define OPENRTX_MESSAGING_H

/**
 * @file messaging.h
 * @brief Text messages for every transport the radio speaks: the queue the UI
 * writes to, the inbox it reads, and the dispatch to APRS, DMR SMS and TETRA
 * SDS.
 *
 * One message model for all three transports, so the messaging screen never
 * needs to know which one carried the text:
 *
 *   - APRS, on the analog FM path (aprs_service, AX.25 UI frames);
 *   - DMR short messages, in a data call (protocols/DMR/dmr_sms.h);
 *   - TETRA SDS, the uplink PDU of EN 300 392-2 (protocols/TETRA/SdsEnc.h).
 *
 * The outbox is a small ring of pending messages; the transport that can send
 * takes the oldest one for it, reports the outcome and the entry moves to a
 * terminal state.  Retries, the ACK of a delivery response and the sequence
 * numbers of each transport are the transport's business, not the UI's.
 *
 * Nothing allocates.  Outbox and inbox live in RAM and are written to a pair
 * of flash sectors (messaging_store.h) so a power cycle does not lose queued
 * messages or the last few received ones.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** UTF-8 bytes kept per message, terminator excluded. */
#define MSG_TEXT_MAX 160

/*
 * Sizes are a RAM budget, not a wish: every entry is 196 B of .bss and there
 * are two queues, so eight and eight cost 3,1 kB.  Six pending messages is
 * more than a DMR data call (about 0.4 s each) or the FM mailbox will ever
 * hold, and six received ones fill the screen the messaging inbox is drawn on.
 */
/** Pending messages the outbox holds. */
#define MSG_OUTBOX_LEN 6

/** Received messages kept for the UI, newest first. */
#define MSG_INBOX_LEN 6

/** Reached after this many attempts a message is given up on. */
#define MSG_MAX_ATTEMPTS 3

/** Which radio path carries the message. */
enum msgTransport {
    MSG_TRANSPORT_APRS = 0, ///< APRS on the FM path
    MSG_TRANSPORT_DMR = 1,  ///< DMR short message (data call)
    MSG_TRANSPORT_TETRA = 2,///< TETRA SDS
    MSG_TRANSPORT_DSTAR = 3,///< D-STAR DV slow-data text (20 characters)
    MSG_TRANSPORT_COUNT
};

/** Life cycle of an outbound message. */
enum msgState {
    MSG_STATE_FREE = 0,   ///< Slot not in use
    MSG_STATE_QUEUED,     ///< Waiting for its transport
    MSG_STATE_SENDING,    ///< Handed to the transport, not confirmed yet
    MSG_STATE_SENT,       ///< On the air, no confirmation asked for
    MSG_STATE_DELIVERED,  ///< The peer confirmed it
    MSG_STATE_FAILED,     ///< Given up after MSG_MAX_ATTEMPTS
    MSG_STATE_CANCELLED,  ///< Dropped by the operator before it finished
};

/** Flags of a message. */
enum msgFlags {
    MSG_FLAG_UNREAD = 0x01,   ///< Inbound, not shown to the user yet
    MSG_FLAG_CONFIRM = 0x02,  ///< The sender wants a delivery confirmation
    MSG_FLAG_INBOUND = 0x04,  ///< Received, not sent
    MSG_FLAG_TRUNCATED = 0x08,///< Text was cut to fit
    MSG_FLAG_REPLY_READY = 0x10, ///< A response is waiting for the transport
};

/** One message, the same for both directions and all transports. */
struct message {
    uint32_t src;    ///< Source address: radio ID, SSI or callsign hash
    uint32_t dst;    ///< Destination address
    uint32_t time;   ///< Uptime in ms when queued or received
    uint16_t ref;    ///< Transport reference (TMS sequence, SDS reference)
    uint8_t id;      ///< Identifier, 1..255, unique while the entry lives
    uint8_t transport; ///< enum msgTransport
    uint8_t state;   ///< enum msgState
    uint8_t flags;   ///< enum msgFlags
    uint8_t attempts;///< Transmissions tried so far
    uint8_t textLen; ///< UTF-8 bytes in text
    char peer[12];   ///< Address as text (APRS callsign), "" when unused
    char text[MSG_TEXT_MAX + 1]; ///< UTF-8, NUL terminated
};

/**
 * Build the model.  Call once at start-up, before the UI or the RTX thread
 * touch it; it also loads the stored queue.
 */
void messaging_init(void);

/**
 * @return true once messaging_init() has run.  A screen or a caller that finds
 * the store built must not build it again: initialising reloads the record and
 * would drop what the radio received since.
 */
bool messaging_ready(void);

/**
 * Queue a message for a transport.
 *
 * @param transport: which path carries it.
 * @param dst: destination address in the transport's own numbering.
 * @param text: UTF-8 text, NUL terminated; longer texts are cut.
 * @param confirm: ask the peer for a delivery confirmation.
 * @param now: uptime in ms.
 * @return the message id, or -1 if the outbox is full or the text is empty.
 */
int messaging_send(enum msgTransport transport, uint32_t dst, const char *text,
                   bool confirm, uint32_t now);

/**
 * Queue a message with both a numeric address and the name the operator typed.
 *
 * The numeric address is what the transport puts on the air (a DMR radio ID, a
 * TETRA SSI); the name is what the messaging screen shows -- a callsign from
 * the contact book or a manual entry.  messaging_send() is this with no name.
 *
 * @return the message id, or -1.
 */
int messaging_sendAddr(enum msgTransport transport, uint32_t dst,
                       const char *peer, const char *text, bool confirm,
                       uint32_t now);

/**
 * Queue a message addressed by name, for the transports whose addresses are
 * text (an APRS callsign).  The numeric address keeps a hash of the name, so
 * the UI can group and sort by it.
 *
 * @return the message id, or -1.
 */
int messaging_sendToText(enum msgTransport transport, const char *peer,
                         const char *text, bool confirm, uint32_t now);

/** @return the number of messages waiting for a transport. */
int messaging_pending(enum msgTransport transport);

/** @return the oldest message queued for a transport, NULL if none. */
const struct message *messaging_head(enum msgTransport transport);

/**
 * Mark a queued message as being transmitted, so a second call does not pick
 * it again.  The caller keeps the entry valid until it reports the outcome.
 *
 * @return the message id, -1 if there is nothing to send.
 */
int messaging_begin(enum msgTransport transport);

/**
 * Report the outcome of a transmission.
 *
 * @param id: message id returned by messaging_begin().
 * @param delivered: true when the peer confirmed it (or no confirmation was
 *                   asked for), false to retry or give up.
 * @param ref: transport reference of the transmission, 0 if none.
 * @param now: uptime in ms.
 */
void messaging_done(int id, bool delivered, uint16_t ref, uint32_t now);

/**
 * Note that the peer confirmed a message, for the transports whose
 * confirmation arrives later than the transmission (a DMR delivery response,
 * a TMS acknowledgement).
 *
 * @param id: message id, or 0 to match the oldest message sending to `peer`.
 * @param peer: the address the confirmation came from.
 */
void messaging_confirmed(int id, uint32_t peer, uint32_t now);

/**
 * Give a message up for good, whatever its retry count (an address the
 * transport cannot use, for instance).
 *
 * @param id: message id.
 * @param now: uptime in ms.
 */
void messaging_fail(int id, uint32_t now);

/** @return the message with that id, NULL if it is gone. */
struct message *messaging_find(int id);

/**
 * Copy a message by value.
 *
 * messaging_find() and messaging_head() hand out a pointer into the outbox and
 * release the lock: the entry stays alive but it can change owner (a new
 * message reuses a finished slot), so anything read through the pointer after
 * another call into this module is a guess.  A caller that needs more than one
 * field, or that acts on the message after calling something else, takes a
 * copy with this and works from it.  That is the only way the text it reads is
 * the text of the message it looked up.
 *
 * @param id: message id.
 * @param out: destination, filled only when the message exists.
 * @return 0 on success, -1 if there is no live message with that id.
 */
int messaging_copy(int id, struct message *out);

/* ------------------------------------------------------------- inbound --- */

/**
 * Store a received message: newest first in the inbox, unread.
 *
 * @param transport: which path carried it.
 * @param src: sender address.
 * @param dst: destination address as received.
 * @param text: UTF-8 text, may be empty for a status or a raw payload.
 * @param confirm: the sender asked for a confirmation.
 * @param now: uptime in ms.
 * @return the new message id, -1 if the text is empty and there is nothing to
 *         show, or the inbox cannot take it.
 */
int messaging_received(enum msgTransport transport, uint32_t src, uint32_t dst,
                       const char *text, bool confirm, uint32_t now);

/** @return the i-th received message, 0 is the newest, NULL out of range. */
const struct message *messaging_inbox(int i);

/**
 * @return the i-th outbound entry in use, 0 is the oldest, NULL out of range.
 * Terminal states are kept until their slot is reused by a new message.
 */
const struct message *messaging_outbox(int i);

/** @return how many outbound entries are in use (queued and finished). */
int messaging_outCount(void);

/** @return how many received messages are still unread. */
int messaging_unread(void);

/** Clear the unread flag of every received message. */
void messaging_markRead(void);

/** Clear the unread flag of one message, by id (a message opened on screen). */
void messaging_markReadId(int id);

/**
 * Drop a message the operator cancelled.  Its transport stops sending it (a
 * queued entry is never picked again; one on the air finishes, and the late
 * outcome is ignored) and the screen shows it as cancelled.
 *
 * @param id: message id.
 * @param now: uptime in ms.
 */
void messaging_cancel(int id, uint32_t now);

/* --------------------------------------------------------------- draft --- */

/**
 * The compose draft: transport, recipient and text of a message that was not
 * sent yet.  It is part of the store, so it survives a power cycle with the
 * same two-sector discipline as the queues.
 *
 * Edit the fields in place and call messaging_draftTouch(); the write happens
 * on the next messaging_task() pass, debounced like any other change.
 *
 * @return the draft, never NULL.  An empty draft has an empty text.
 */
struct message *messaging_draft(void);

/** Note that the draft changed (the store writes it on the next pass). */
void messaging_draftTouch(void);

/** Empty the draft. */
void messaging_draftClear(void);

/** Drop every message, in RAM and in the pending record. */
void messaging_clear(void);

/* --------------------------------------------------------- persistence --- */

/**
 * Write the store to nonvolatile memory if it changed and the debounce time
 * has passed.  Call from a non real-time task.
 *
 * @param now: uptime in ms.
 */
void messaging_task(uint32_t now);

/** @return true if a write is pending (diagnostics). */
bool messaging_dirty(void);

/**
 * Pack the store into a record: magic, version, entries, CRC-16.  Exposed for
 * the storage backends and the host tests.
 *
 * @param buf: destination.
 * @param cap: capacity.
 * @return bytes written, -1 if it does not fit.
 */
int messaging_serialise(uint8_t *buf, size_t cap);

/**
 * Restore the store from a record written by messaging_serialise().
 *
 * @return the number of messages restored, -1 if the record is not usable.
 */
int messaging_deserialise(const uint8_t *buf, size_t len);

/** @return a short name of the transport, for logs and the UI. */
const char *messaging_transportName(enum msgTransport transport);

/** @return a short name of the state, for logs and the UI. */
const char *messaging_stateName(enum msgState state);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_MESSAGING_H */
