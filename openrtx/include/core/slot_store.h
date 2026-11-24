/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SLOT_STORE_H
#define SLOT_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Two-slot record container with CRC, shared by the contact book and the
 * message store.
 *
 * Both slots live in the same flash partition, at an offset chosen by the
 * caller: a save always goes to the slot that is *not* holding the current
 * data, and only becomes visible after the header and payload are read back
 * and verified. A power cut in the middle of a save therefore costs at most
 * the save itself, never the last good copy. The newest valid slot, by
 * sequence number, is the one loaded.
 *
 * Layout of one slot, little endian:
 *
 *   0  uint32  magic
 *   4  uint16  version
 *   6  uint16  payload length
 *   8  uint32  sequence (grows on every save)
 *  12  uint16  CRC16-CCITT of the payload
 *  14  uint16  reserved, zero
 *  16  payload
 */

#define SLOT_HDR_SIZE 16U
/** Flash access, provided by the platform (or by a host test). */
typedef struct
{
    int (*read)(void *ctx, uint32_t off, void *buf, size_t len);
    int (*erase)(void *ctx, uint32_t off, size_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    void *ctx;          /**< Passed to the operations, holds the base offset */
    uint32_t slot_size; /**< Bytes per slot, multiple of sector, >= hdr+data */
    uint32_t sector;    /**< Erase unit, must divide slot_size              */
} slot_ops_t;

/** Handle of one container. */
typedef struct
{
    const slot_ops_t *ops;
    uint32_t magic;
    uint16_t version;
    uint32_t slot;   /**< Slot holding the current payload (0 or 1) */
    uint32_t seq;    /**< Sequence number of that payload           */
} slot_store_t;

/**
 * Find and load the newest valid payload.
 *
 * @return 0 on success, -ENOENT when both slots are blank or invalid (the
 *         caller keeps its defaults and the next save goes to slot 0), or a
 *         negative errno on a read error.
 */
int slot_store_load(slot_store_t *st, const slot_ops_t *ops, uint32_t magic,
                    uint16_t version, void *payload, size_t len);

/**
 * Write a payload to the inactive slot, verify it, and make it current.
 *
 * @return 0 on success, -EIO if the write or the read-back check failed (the
 *         previous payload is still the valid one).
 */
int slot_store_save(slot_store_t *st, const void *payload, size_t len);

/** True when both slots are blank, i.e. nothing was ever written. */
bool slot_store_blank(const slot_store_t *st);

#ifdef __cplusplus
}
#endif

#endif /* SLOT_STORE_H */
