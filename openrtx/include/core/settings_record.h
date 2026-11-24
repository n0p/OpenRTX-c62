/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SETTINGS_RECORD_H
#define SETTINGS_RECORD_H

/**
 * @file settings_record.h
 * @brief On-flash container of settings_t: magic, payload version and CRC.
 *
 * The NVM driver owns the flash (erase sector, alignment, read-back), this
 * owns the bytes: the layout below and the rules that decide whether a record
 * may be interpreted at all.
 *
 *   offset  size  field
 *   0       4     magic "ORTX" (little endian 0x4F525458)
 *   4       2     CRC-CCITT of the payload
 *   6       2     payload version (SETTINGS_VERSION)
 *   8       n     settings_t, packed
 *
 * A record is usable only when the magic matches, the version is the one this
 * firmware writes and the CRC matches.  Anything else is discarded as a whole
 * and the caller falls back to default_settings: a settings record that is
 * reinterpreted after the structure grew is worse than a reset, because the
 * fields land at the wrong offset and the radio boots with a plausible but
 * wrong configuration (a squelch level read from a callsign, for instance).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Magic value: "ORTX". */
#define SETTINGS_RECORD_MAGIC 0x4F525458U

/** Size of the record header in front of the payload. */
#define SETTINGS_RECORD_HDR 8U

/** Size of a complete record: header plus a packed settings_t. */
#define SETTINGS_RECORD_SIZE (SETTINGS_RECORD_HDR + sizeof(settings_t))

/** Value returned when the record is intact but written by another version. */
#define SETTINGS_RECORD_ERR_VERSION (-34) /* -ERANGE */

/** Value returned when the record is damaged (magic or CRC). */
#define SETTINGS_RECORD_ERR_CORRUPT (-22) /* -EINVAL */

/**
 * Build a record from a settings structure.
 *
 * @param buf: destination, at least SETTINGS_RECORD_SIZE bytes.
 * @param cap: capacity of buf.
 * @param s: settings to store.
 * @return the number of bytes written, -1 if buf is too small.
 */
int settings_recordPack(uint8_t *buf, size_t cap, const settings_t *s);

/**
 * Parse a record.
 *
 * @param buf: the record as read back from flash.
 * @param len: bytes available, at least SETTINGS_RECORD_SIZE.
 * @param out: settings to fill; untouched when the record is rejected.
 * @return 0 on success, SETTINGS_RECORD_ERR_VERSION if the payload version is
 *         not SETTINGS_VERSION (the record is from an older or newer
 *         firmware), SETTINGS_RECORD_ERR_CORRUPT on bad magic, bad CRC or a
 *         short buffer.
 */
int settings_recordRead(const uint8_t *buf, size_t len, settings_t *out);

/**
 * Same as settings_recordRead() but reporting why a record was dropped, in the
 * terms the operator interface uses.
 *
 * @return 0 when the record loaded, -SETTINGS_RECORD_ERR_* otherwise, and
 *         `cause` is set to the enum settingsReset value that explains it.
 */
int settings_recordLoad(const uint8_t *buf, size_t len, settings_t *out,
                        uint8_t *cause);

/** Payload version of a record, without validating anything else. */
uint16_t settings_recordVersion(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_RECORD_H */
