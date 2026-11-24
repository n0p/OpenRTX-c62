/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrSmsApp.h
 * @brief Private interface between DmrSms.c (PDUs) and DmrSmsApp.c
 * (payload codecs and application formats).
 */

#ifndef OPENRTX_DMR_SMS_APP_H
#define OPENRTX_DMR_SMS_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/DMR/DmrSms.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build the user payload of a message, the octets that go into the data
 * blocks behind the header.
 *
 * @param app: flavour to build.
 * @param src: source radio ID (IPv4 addresses, TMS fields).
 * @param dst: destination radio ID.
 * @param group: destination is a talkgroup.
 * @param text: UTF-8 text, may be NULL for a TMS acknowledgement.
 * @param tmsSeq: TMS sequence number.
 * @param ackReq: the sender asked for an acknowledgement (TMS header bit).
 * @param tmsAck: build a TMS acknowledgement record instead of text.
 * @param out: destination buffer.
 * @param cap: capacity of the buffer.
 * @param truncated: set to true if the text did not fit.
 * @return octets written, -1 on error (unknown app, nothing to send).
 */
int dmrSms_buildPayload(enum dmrSmsApp app, uint32_t src, uint32_t dst,
                        bool group, const char *text, uint8_t tmsSeq,
                        bool ackReq, bool tmsAck, uint8_t *out, size_t cap,
                        bool *truncated);

/**
 * Interpret the user payload of a packet as a short message.
 *
 * @param dpf: data packet format of the header.
 * @param sap: service access point of the header.
 * @param dd: defined data format (-1 when the header has none).
 * @param p: user payload octets.
 * @param n: number of octets.
 * @param m: message to fill; app, text, ports and TMS fields are set.
 * @return true if a text handler recognised the payload.
 */
bool dmrSms_parsePayload(int dpf, int sap, int dd, const uint8_t *p, size_t n,
                         struct dmrSmsMessage *m);

/** UTF-8 to UTF-16 code units, truncated on a unit boundary. */
size_t dmrSms_utf8ToUnits(const char *utf8, uint16_t *units, size_t maxUnits);

/** UTF-8 to UTF-16BE (be) or UTF-16LE octets, truncated on a unit boundary. */
size_t dmrSms_utf8ToUtf16(const char *utf8, uint8_t *out, size_t cap, bool be,
                          bool *truncated);

/** UTF-16 octets to UTF-8, stops at a NUL unit, truncated at `cap`. */
size_t dmrSms_utf16ToUtf8(const uint8_t *p, size_t n, bool be, char *out,
                          size_t cap, bool *truncated);

/** ISO-8859-1 octets to UTF-8. */
size_t dmrSms_latin1ToUtf8(const uint8_t *p, size_t n, char *out, size_t cap,
                           bool *truncated);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_SMS_APP_H */
