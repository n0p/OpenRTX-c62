/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BACKUP_IMAGE_H
#define BACKUP_IMAGE_H

/**
 * @file backup_image.h
 * @brief Internal backup of the radio's own stores, in its own flash.
 *
 * The screens have been in the firmware since the generic branch
 * (MENU_BACKUP_RESTORE, MENU_BACKUP, MENU_RESTORE) but the only thing behind
 * them was the MD-3x0's xmodem dump of the *external* flash to a computer. This
 * module is the radio's own copy: settings, VFO, channel bank, contact book,
 * message store and zone plan, in a record of its own in the free part of the
 * storage partition (see docs/BACKUP.md).
 *
 * Two properties are the whole point of it:
 *
 *  - **a backup never costs the previous one.** There are two areas of the same
 *    size and a save goes to the one that does not hold the newest valid
 *    record. The header is written last, after the payload, so a power cut in
 *    the middle of a save leaves an area with no usable header: the previous
 *    record is still the newest valid one.
 *  - **a restore verifies before it writes.** The magic, the format version,
 *    the directory and the CRC-16 of the whole record are checked first; a
 *    record that does not check out is refused and not one byte of the live
 *    stores is touched.
 *
 * The engine is portable C: it talks to flash through bkpPort_t, to the stores
 * through their own public APIs and to the clock through platform_getCurrentTime().
 * The platform provides the flash port (platform/targets/c62/backup_c62.c on the
 * radio, platform/targets/linux/emulator/backup_host.c on the host) or binds one
 * in a test with bkp_bind().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/datetime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Magic of a record header: "BKUP". */
#define BKP_MAGIC 0x50554B42U

/**
 * Version of the record format written by this build.
 *
 *  - 1: header with magic, version, sequence, timestamp, sizes and CRC-16, a
 *       directory of the six stores and the packed payloads as the stores
 *       already write them (settings_recordPack, messaging_serialise, ...).
 *
 * A record of another version is refused as a whole, before anything is
 * written back: the payloads are the stores' own formats, so reinterpreting one
 * written by another firmware would put fields at the wrong offset.
 */
#define BKP_VERSION 1

/** Stores a record carries, in the order of the directory. */
enum bkpEntry
{
    BKP_E_SETTINGS = 0, /**< settings_t, as settings_recordPack() writes it */
    BKP_E_VFO,          /**< the VFO channel_t                           */
    BKP_E_CHANNELS,     /**< the channel bank, as chstore_export() writes */
    BKP_E_CONTACTS,     /**< the contact book, cstore_export()           */
    BKP_E_MESSAGES,     /**< outbox, inbox and draft, messaging_serialise() */
    BKP_E_ZONES,        /**< the zone plan of this build and its direction */
    BKP_E_COUNT
};

/** Size of one area in the port's address space. */
#define BKP_AREA_SIZE 0x4000U

/**
 * Write unit of the medium the format targets, bytes.
 *
 * The C62's NOR flash only accepts writes whose offset and length are multiples
 * of it, so every entry is stored padded to it (with 0xFF, the erased value)
 * and every offset in the directory is a multiple of it. A port whose medium
 * does not care simply receives aligned writes, which is never wrong.
 */
#define BKP_ALIGN 4U

/** Largest entry a record can carry (a full channel bank). */
#define BKP_MAX_ENTRY 4864U

/** Result codes. Negative, so that `if (rc != BKP_OK)` reads the same as errno. */
enum bkpResult
{
    BKP_OK = 0,
    BKP_ERR_NO_PORT = -1,   /**< the platform has no backup storage         */
    BKP_ERR_NO_BACKUP = -2, /**< no area holds a valid record               */
    BKP_ERR_CORRUPT = -3,   /**< magic, header, directory or CRC do not match */
    BKP_ERR_VERSION = -4,   /**< written by another firmware version        */
    BKP_ERR_TOO_BIG = -5,   /**< the stores do not fit an area              */
    BKP_ERR_IO = -6,        /**< erase, write or read failed                */
    BKP_ERR_STORE = -7,     /**< a store refused the data it was handed     */
    BKP_ERR_PARAM = -8      /**< bad argument                               */
};

/**
 * Flash access, provided by the platform or by a test.
 *
 * Offsets are relative to the port's own base (the first of the two areas), the
 * way chstorePort and slot_ops_t do it, so the same engine runs on the radio's
 * partition and on a file of the host.
 */
typedef struct
{
    int (*read)(void *ctx, uint32_t off, void *buf, size_t len);
    int (*erase)(void *ctx, uint32_t off, size_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    void    *ctx;
    uint32_t size;   /**< bytes, at least 2 * BKP_AREA_SIZE               */
    uint32_t sector; /**< erase unit of the medium                        */
} bkpPort_t;

/** What a valid record holds: read it before offering the restore. */
typedef struct
{
    bool     valid;
    uint16_t version;            /**< format version of the record           */
    uint32_t sequence;           /**< write counter, newest wins             */
    uint32_t size;               /**< bytes of the whole record              */
    datetime_t when;             /**< clock when it was taken, zero if none  */
    uint8_t  area;               /**< 0 or 1: which area holds it            */
    uint16_t count;              /**< entries in the directory               */
    uint16_t lengths[BKP_E_COUNT]; /**< bytes per entry, 0 when absent       */
} bkpInfo_t;

/**
 * Platform hook: the flash port of this device, or NULL when it has none (the
 * screens then say NO STORAGE and writes are refused).
 */
const bkpPort_t *bkp_platformPort(void);

/** Bind a port explicitly. Mostly for tests; NULL means "no storage". */
void bkp_bind(const bkpPort_t *port);

/**
 * Take a backup: pack the six stores into a record and write it to the area
 * that does not hold the current one, then read it back and verify it.
 *
 * On failure the previous record is untouched and still the valid one.
 *
 * @return BKP_OK or a negative bkpResult.
 */
int bkp_backup(void);

/**
 * Read the header of the newest valid record and check its CRC.
 *
 * @param out: filled in; `valid` is false and the return value is
 *             BKP_ERR_NO_BACKUP when there is nothing to restore.
 * @return BKP_OK or a negative bkpResult.
 */
int bkp_inspect(bkpInfo_t *out);

/**
 * Restore the newest valid record.
 *
 * The whole record is verified (magic, version, directory, CRC-16) before the
 * first byte of any store is written, and the zone plan is checked against the
 * one this build has. Then each store is replaced through its own write path,
 * so every store keeps its read-back check.
 *
 * @return BKP_OK, or the first failure: BKP_ERR_CORRUPT / BKP_ERR_VERSION leave
 *         the radio untouched, BKP_ERR_STORE means one store refused the data
 *         (the others were restored, the log names the one that failed).
 */
int bkp_restore(void);

/** Text for a bkpResult: "OK", "NO BACKUP", "CRC BAD", ... */
const char *bkp_resultText(int result);

/** Text of the last backup or restore, for the screen and the log. */
const char *bkp_lastResult(void);

#ifdef __cplusplus
}
#endif

#endif /* BACKUP_IMAGE_H */
