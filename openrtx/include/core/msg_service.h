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
#include "core/msg_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Transport independent message sending.
 *
 * The UI knows only this interface: recipient + text -> enqueue, plus a poll
 * that refreshes the delivery state and mirrors received messages into the
 * persistent store. Which transport is used is a row of the compose screen.
 *
 * APRS over FM is wired to the existing aprs_service. DMR SMS and TETRA SDS
 * are thin hooks with weak defaults (unavailable / refuses to send): porting
 * those protocols only has to provide strong definitions of the four
 * msg_dmr_* / msg_tetra_* functions below, nothing in the UI changes.
 */

/** Name shown in the transport row. */
const char *msg_service_name(uint8_t tp);

/** Longest text the transport accepts, characters. */
uint8_t msg_service_maxLen(uint8_t tp);

/**
 * Can this transport send right now? Checks the build option and the current
 * operating mode: APRS needs FM, DMR SMS needs DMR, SDS needs TETRA.
 */
bool msg_service_available(uint8_t tp);

/** First available transport for the current mode, or MSG_TP_NUM if none. */
uint8_t msg_service_default(void);

/**
 * Queue a message: the text is copied into the persistent store and handed to
 * the transport.
 *
 * @return the stored record, or NULL if the transport is unavailable, the
 *         text is empty or too long, or the store refused it.
 */
msg_rec_t *msg_service_queue(uint8_t tp, const char *peer, const char *text,
                             uint32_t now);

/** Cancel a queued message: it stops retrying and shows as cancelled. */
bool msg_service_cancel(msg_rec_t *rec);

/**
 * Refresh delivery states and mirror received messages into the store.
 * Call once per UI pass; cheap when nothing changed.
 */
void msg_service_poll(uint32_t now);

/** Initialise the store and bind the transports. */
void msg_service_init(void);

/** Status as the UI spells it: "QUEUED", "TX", "SENT", "ACK", "FAIL", "CANC". */
const char *msg_service_statusText(uint8_t status);

/*
 * Transport hooks. Weak defaults in msg_service.c: the DMR and TETRA ones are
 * for the SMS/SDS ports, they override these by defining strong symbols.
 */
bool msg_dmr_sms_available(void);
bool msg_dmr_sms_send(const char *peer, const char *text, char *ref,
                      size_t refLen);
bool msg_dmr_sms_cancel(const char *ref);

bool msg_tetra_sds_available(void);
bool msg_tetra_sds_send(const char *peer, const char *text, char *ref,
                        size_t refLen);
bool msg_tetra_sds_cancel(const char *ref);

#ifdef __cplusplus
}
#endif

#endif /* MSG_SERVICE_H */
