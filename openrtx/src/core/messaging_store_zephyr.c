/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file messaging_store_zephyr.c
 * @brief Message store in the C62 storage partition, for the Zephyr build.
 *
 * Same scheme as the TETRA SDS history (protocols/TETRA/TetraSds.c): two
 * 4 KiB sectors written alternately, each with a small header holding a magic,
 * a sequence number (the newest wins) and the record length.  A write that a
 * power cut interrupts damages only the sector being written, and the other
 * one still holds the previous set; the record carries its own CRC, so a torn
 * write is rejected when reading back.
 *
 * Storage partition layout (platform/targets/c62/c62.dts, 640 KiB):
 *
 *   0x0000  settings            (nvmem_C62.c)
 *   0x1000  VFO
 *   0x2000  TETRA SDS A         (TetraSds.c)
 *   0x3000  TETRA SDS B
 *   0x4000  messages A          <- this file
 *   0x5000  messages B
 */

#include <string.h>

#include <zephyr/storage/flash_map.h>

#include "core/messaging_store.h"

#define MSG_AREA_A 0x4000
#define MSG_AREA_B 0x5000
#define MSG_SECTOR_SIZE 0x1000

#define MSG_STORE_MAGIC 0x3147534dU /* "MSG1", same as the record */
#define MSG_STORE_VERSION 1

/** Header in front of the packed record. */
struct storeHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
};

/** Static: the record is up to 3 kB and must not sit on a thread stack. */
static uint8_t record[MSG_STORE_MAX];

static bool readHeader(const struct flash_area *area, uint32_t off,
                       struct storeHeader *hdr)
{
    if (flash_area_read(area, off, hdr, sizeof(*hdr)) < 0)
        return false;
    if ((hdr->magic != MSG_STORE_MAGIC) || (hdr->version != MSG_STORE_VERSION))
        return false;
    if ((hdr->length == 0) || (hdr->length > MSG_STORE_MAX))
        return false;
    return true;
}

static size_t trySector(const struct flash_area *area, uint32_t off,
                        const struct storeHeader *hdr)
{
    if (hdr->length > sizeof(record))
        return 0;
    if (flash_area_read(area, off + sizeof(*hdr), record, hdr->length) < 0)
        return 0;
    return hdr->length;
}

size_t messaging_store_size(void)
{
    const struct flash_area *area;
    struct storeHeader a;
    struct storeHeader b;
    bool haveA;
    bool haveB;
    bool aNewer;

    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &area) < 0)
        return 0;

    haveA = readHeader(area, MSG_AREA_A, &a);
    haveB = readHeader(area, MSG_AREA_B, &b);
    aNewer = haveA && (!haveB || ((int32_t)(a.sequence - b.sequence) > 0));

    size_t len = 0;

    if (aNewer) {
        len = trySector(area, MSG_AREA_A, &a);
        if ((len == 0) && haveB)
            len = trySector(area, MSG_AREA_B, &b);
    } else if (haveB) {
        len = trySector(area, MSG_AREA_B, &b);
        if ((len == 0) && haveA)
            len = trySector(area, MSG_AREA_A, &a);
    }
    flash_area_close(area);
    return len;
}

size_t messaging_store_read(uint8_t *buf, size_t cap)
{
    size_t len = messaging_store_size();

    if ((len == 0) || (len > cap))
        return 0;
    memcpy(buf, record, len);
    return len;
}

bool messaging_store_write(const uint8_t *buf, size_t len)
{
    const struct flash_area *area;
    struct storeHeader a;
    struct storeHeader b;
    struct storeHeader hdr;
    bool haveA;
    bool haveB;
    uint32_t off = MSG_AREA_A;
    uint32_t seq = 1;

    if ((buf == NULL) || (len == 0) || (len > MSG_STORE_MAX))
        return false;
    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &area) < 0)
        return false;

    haveA = readHeader(area, MSG_AREA_A, &a);
    haveB = readHeader(area, MSG_AREA_B, &b);

    /*
     * Write the sector that holds the older record, numbered one above the
     * newest sequence seen: a sector whose write was cut short still has a
     * valid header, so its number must not be reused.
     */
    if (haveA && haveB) {
        uint32_t newest = ((int32_t)(b.sequence - a.sequence) > 0) ? b.sequence
                                                                   : a.sequence;

        off = ((int32_t)(a.sequence - b.sequence) < 0) ? MSG_AREA_A : MSG_AREA_B;
        seq = newest + 1;
    } else if (haveA) {
        off = MSG_AREA_B;
        seq = a.sequence + 1;
    } else if (haveB) {
        off = MSG_AREA_A;
        seq = b.sequence + 1;
    }

    hdr.magic = MSG_STORE_MAGIC;
    hdr.version = MSG_STORE_VERSION;
    hdr.length = (uint16_t)len;
    hdr.sequence = seq;

    bool ok = (flash_area_erase(area, off, MSG_SECTOR_SIZE) == 0)
              && (flash_area_write(area, off, &hdr, sizeof(hdr)) == 0)
              && (flash_area_write(area, off + sizeof(hdr), buf, len) == 0);

    flash_area_close(area);
    return ok;
}

void messaging_store_clear(void)
{
    /*
     * Nothing to erase here: an emptied store serialises to a record with no
     * entries, and messaging_task() writes it to the older sector, whose
     * sequence number makes the read prefer it.  Erasing a sector on clear
     * would only add a way to lose both copies at once.
     */
}
