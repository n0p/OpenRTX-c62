/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Received SDS messages: a small ring for the UI plus a packed record for
 * nonvolatile storage.
 *
 * The ring is what the screen lists; the record is what survives a power
 * cycle. Both live in RAM and are written by the receiver thread, so the
 * accessors are lock-free readers of an array that is only ever modified
 * under the caller's own lock (the firmware uses the TETRA status mutex).
 */

#ifndef TETRA_SDS_HIST_H
#define TETRA_SDS_HIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/TETRA/SdsMsg.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Messages kept in RAM (the flash record holds the same number). */
#define SDS_HIST_LEN 8

/** Flags of an entry. */
#define SDS_HIST_UNREAD 0x01 /**< Not shown to the user yet. */
#define SDS_HIST_INCOMPLETE 0x02 /**< Reassembled from a lost fragment. */

/** One stored message, compact enough to keep eight of them in RAM. */
struct sds_hist_entry {
    uint32_t src_ssi;  /**< Sender SSI, 0 if the PDU had none. */
    uint32_t dst_ssi;  /**< Addressee SSI (group or individual). */
    uint32_t tick;     /**< Uptime ms when received, 0 after a reload. */
    uint16_t status;   /**< Pre-coded status, if kind is status. */
    uint8_t kind;      /**< enum sds_kind. */
    uint8_t flags;     /**< SDS_HIST_*. */
    uint8_t text_len;
    char text[SDS_TEXT_MAX + 1];
};

/** Drop every message. */
void sds_hist_reset(void);

/**
 * \brief Store a decoded message as the newest entry.
 *
 * @param m: decoded message.
 * @param tick: uptime in ms, or 0 if unknown.
 * @return index of the new entry (0 is the newest), -1 if not storable.
 */
int sds_hist_push(const struct sds_msg *m, uint32_t tick);

/** @return number of stored messages, 0..SDS_HIST_LEN. */
int sds_hist_count(void);

/**
 * \brief Get a stored message.
 *
 * @param i: 0 is the newest.
 * @return the entry, or NULL if i is out of range.
 */
const struct sds_hist_entry *sds_hist_get(int i);

/** @return number of entries with the unread flag set. */
int sds_hist_unread(void);

/** Clear the unread flag of every entry. */
void sds_hist_mark_read(void);

/** Size of the packed record for the current contents. */
size_t sds_hist_record_size(void);

/**
 * \brief Pack the ring into the flash record ("SDJ1", version, count, CRCs).
 *
 * @param buf: destination buffer.
 * @param cap: size of the buffer.
 * @return bytes written, -1 if the buffer is too small.
 */
int sds_hist_serialise(uint8_t *buf, size_t cap);

/**
 * \brief Restore the ring from a flash record.
 *
 * Entries come back with their unread flag cleared and tick 0. A record
 * with a bad magic, version or CRC leaves the ring empty.
 *
 * @param buf: record bytes as stored.
 * @param len: number of bytes read from flash.
 * @return number of entries restored, -1 if the record is not valid.
 */
int sds_hist_deserialise(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_SDS_HIST_H */
