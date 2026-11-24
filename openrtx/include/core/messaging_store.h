/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file messaging_store.h
 * @brief Where the message store is kept between power cycles.
 *
 * The store is a packed record (messaging_serialise()) with its own magic and
 * CRC-16; this layer only has to put it somewhere that survives a power cut
 * without ever leaving a half-written record readable.  The firmware backend
 * (messaging_store_zephyr.c) uses the same pair of alternating 4 KiB sectors
 * as the TETRA SDS history: a sequence number decides which copy is newer and
 * a write that was cut short only damages the sector being written.  The host
 * backend in the tests keeps it in RAM.
 */

#ifndef OPENRTX_MESSAGING_STORE_H
#define OPENRTX_MESSAGING_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest record the store has to hold: eight queued and eight received
 *  entries of at most 30 + MSG_TEXT_MAX octets, plus the header and the CRC. */
#define MSG_STORE_MAX 3100

/**
 * @return the number of bytes of the stored record, 0 if there is none.
 */
size_t messaging_store_size(void);

/**
 * Read the stored record.
 *
 * @param buf: destination.
 * @param cap: capacity, at least MSG_STORE_MAX.
 * @return bytes read, 0 if there is no usable record.
 */
size_t messaging_store_read(uint8_t *buf, size_t cap);

/**
 * Replace the stored record.
 *
 * @param buf: record packed by messaging_serialise().
 * @param len: its length.
 * @return true on success.
 */
bool messaging_store_write(const uint8_t *buf, size_t len);

/**
 * Drop the stored record (the next write starts from a clean sector pair).
 */
void messaging_store_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_MESSAGING_STORE_H */
