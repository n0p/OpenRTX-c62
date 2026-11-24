/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CONTACT_STORE_H
#define CONTACT_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "core/slot_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Contact book kept on the radio.
 *
 * The CPS codeplug (cps_readContact) is empty on the C62, so the contact list
 * was read only and, in practice, blank. This store is the editable one: it
 * holds what the radio itself needs to address a message (callsign, display
 * name and the preferred transport), lives in the same flash partition as the
 * channel bank (see channel_store_c62.c for the map) and is written through
 * slot_store, i.e. two ping-pong slots with a CRC and a read-back check.
 *
 * The record is deliberately smaller than contact_t: no geolocation, no
 * 32-byte description, nothing the radio cannot edit.
 */

/** Size of one record on flash: 10 + 17 + 4 + 1 + 1. */
#define CSTORE_RECORD_SIZE 33

/** Maximum number of contacts. */
#define CSTORE_MAX_CONTACTS 64

/** Callsign field size including the NUL: "N0CALL-15" is the longest form. */
#define CSTORE_CALL_LEN 10

/** Display name size including the NUL. */
#define CSTORE_NAME_LEN 17

/** Value meaning "no contact" where an index is expected. */
#define CSTORE_NONE 0xFFFF

/** Contact flags. */
enum cstoreFlags
{
    CSTORE_FLAG_GROUP = 0x01  /**< DMR group (talkgroup) instead of private */
};

typedef struct
{
    char     call[CSTORE_CALL_LEN]; /**< Callsign or DMR id, NUL terminated */
    char     name[CSTORE_NAME_LEN]; /**< Display name, NUL terminated      */
    uint32_t dmr_id;                /**< DMR subscriber id, 0 if unknown   */
    uint8_t  mode;                  /**< Preferred transport, OPMODE_*     */
    uint8_t  flags;                 /**< enum cstoreFlags                  */
} __attribute__((packed)) cstore_contact_t;

/** Whole book as it is written to flash. */
typedef struct
{
    uint16_t        count;
    uint16_t        reserved;
    cstore_contact_t list[CSTORE_MAX_CONTACTS];
} __attribute__((packed)) cstore_bank_t;

/**
 * Platform hook: flash access for the contact book, or NULL on a device
 * without one (the book is then RAM only and reports empty after a reset).
 */
const slot_ops_t *cstore_platformOps(void);

/** Bind a port explicitly. Mostly for tests. */
void cstore_bind(const slot_ops_t *ops);

/** Load the book. Idempotent; a blank or corrupt store loads empty. */
int cstore_init(void);

/** Number of contacts, 0 if not initialised. */
int cstore_count(void);

/** Copy contact idx. @return 0 on success, -EINVAL on a bad index. */
int cstore_get(uint16_t idx, cstore_contact_t *out);

/** Validate: callsign not empty and inside the field. */
bool cstore_valid(const cstore_contact_t *c);

/** Append a contact and write the book. @return the index, or a negative errno. */
int cstore_add(const cstore_contact_t *c);

/** Replace contact idx and write the book. */
int cstore_set(uint16_t idx, const cstore_contact_t *c);

/** Delete contact idx, shifting the rest down, and write the book. */
int cstore_delete(uint16_t idx);

/**
 * Index of the first contact whose callsign matches (case sensitive, as the
 * radio stores it), or CSTORE_NONE.
 */
uint16_t cstore_findCall(const char *call);

/**
 * Index of the first contact at or after `from` whose preferred transport is
 * `mode`, or CSTORE_NONE. Used to pre-select a recipient.
 */
uint16_t cstore_findForMode(uint8_t mode, uint16_t from);

/** Write the book if it changed. 0 if it is on flash or nothing was pending. */
int cstore_commit(void);

/**
 * Drop the in-RAM copy so that the next cstore_init() reloads from flash.
 * For tests, and for a shutdown path that wants the next boot to re-read.
 */
void cstore_forget(void);

/** Direct access to the in-RAM book, for tests and for enumeration. */
cstore_bank_t *cstore_bank(void);

#ifdef __cplusplus
}
#endif

#endif /* CONTACT_STORE_H */
