/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSG_SERVICE_H
#define MSG_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "core/messaging.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The messaging screen's door into the shared message store.
 *
 * There is one store and one queue in the radio -- core/messaging.h: outbox,
 * inbox, states, retries, the compose draft and the flash record -- and this
 * file adds only what a screen needs on top of it:
 *
 *   - which transport the build and the current mode can send with, its name
 *     and its text limit;
 *   - the address translation: a callsign from the contact book becomes the
 *     DMR radio ID or the TETRA SSI the transport puts on the air;
 *   - the APRS leg of the store: the FM path owns its own mailbox and retry
 *     schedule (aprs_service), so queued APRS text is handed over there and its
 *     outcome comes back as a message state.
 *
 * DMR short messages and TETRA SDS are wired directly: they pump the store
 * themselves (OpMode_DMR, tetraSds_prepareQueued()) and report through
 * messaging_done() / messaging_confirmed().
 */

/** Longest recipient string, NUL included: "N0CALL-15" is the longest form. */
#define MSG_PEER_LEN 12

/** Name shown in the transport row. */
const char *msg_service_name(enum msgTransport tp);

/** Longest text the transport accepts, characters. */
uint8_t msg_service_maxLen(enum msgTransport tp);

/**
 * Can this transport send right now? Checks the build option and the current
 * operating mode: APRS needs FM, DMR SMS needs DMR, SDS needs TETRA.
 */
bool msg_service_available(enum msgTransport tp);

/** First available transport for the current mode, or MSG_TRANSPORT_COUNT. */
enum msgTransport msg_service_default(void);

/**
 * Queue a message: the text and the name are copied into the shared store.
 *
 * APRS sends to the callsign as typed.  DMR and TETRA send to the number the
 * name stands for: the DMR ID or the SSI of a contact in the book, or the
 * digits the operator typed.
 *
 * @return the message id, or -1 if the transport is unavailable, the peer does
 *         not resolve to an address, the text is empty or too long, or the
 *         outbox is full.
 */
int msg_service_queue(enum msgTransport tp, const char *peer, const char *text,
                      bool confirm, uint32_t now);

/**
 * Cancel a message the operator does not want any more.  A queued entry is
 * never picked again; one already on the air finishes and its outcome is
 * ignored.
 */
bool msg_service_cancel(int id, uint32_t now);

/**
 * Housekeeping: move queued APRS text into the FM mailbox and bring its
 * outcome back.  Call once per UI pass; cheap when nothing changed.
 */
void msg_service_poll(uint32_t now);

/** Bind the store.  Idempotent. */
void msg_service_init(void);

/** Status as the UI spells it: "QUEUED", "TX", "SENT", "ACK", "FAIL", "CANC". */
const char *msg_service_statusText(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* MSG_SERVICE_H */
