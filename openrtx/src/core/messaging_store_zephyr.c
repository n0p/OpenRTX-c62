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

#include "core/messaging.h"
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

/*
 * There is no buffer of the record here: reading goes straight into the
 * caller's, which is messaging.c's own recordBuf.  The two 3,4 kB copies this
 * file used to keep -- one per sector plus one in messaging.c -- were 6,8 kB of
 * the 256 kB of SRAM for the same bytes.
 */

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

/**
 * Where the newest valid record starts and how long it is.
 *
 * @return false when neither sector holds a header that checks out.
 */
static bool newestRecord(const struct flash_area *area, uint32_t *off,
                         size_t *len)
{
    struct storeHeader a;
    struct storeHeader b;
    bool haveA = readHeader(area, MSG_AREA_A, &a);
    bool haveB = readHeader(area, MSG_AREA_B, &b);
    bool aNewer = haveA && (!haveB || ((int32_t)(a.sequence - b.sequence) > 0));

    if (aNewer) {
        *off = MSG_AREA_A;
        *len = a.length;
    } else if (haveB) {
        *off = MSG_AREA_B;
        *len = b.length;
    } else {
        return false;
    }
    return true;
}

size_t messaging_store_size(void)
{
    const struct flash_area *area;
    uint32_t off;
    size_t len;

    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &area) < 0)
        return 0;

    bool ok = newestRecord(area, &off, &len);

    flash_area_close(area);
    (void)off;
    return ok ? len : 0;
}

/**
 * Read one sector's record and tell whether it is a record at all.
 *
 * A sector whose write a power cut interrupted has a header that may well
 * check out -- the header went first -- and a payload that does not: the CRC
 * at the end of the record is the only thing that separates the two, so it is
 * asked here and not three layers up, where answering "no" means throwing the
 * whole store away instead of reading the other sector.
 */
static bool loadSector(const struct flash_area *area, uint32_t off,
                       const struct storeHeader *hdr, uint8_t *buf, size_t cap,
                       size_t *got)
{
    if ((hdr->length == 0) || (hdr->length > cap))
        return false;
    if (flash_area_read(area, off + sizeof(*hdr), buf, hdr->length) < 0)
        return false;
    if (!messaging_recordValid(buf, hdr->length))
        return false;

    *got = hdr->length;
    return true;
}

size_t messaging_store_read(uint8_t *buf, size_t cap)
{
    const struct flash_area *area;
    uint32_t off = 0;
    size_t len = 0;

    if ((buf == NULL) || (cap == 0))
        return 0;
    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &area) < 0)
        return 0;

    /*
     * The newest sector wins only if it holds a record that verifies; a torn
     * one -- the power cut off in the middle of the payload, which is the way
     * this radio is switched off -- is skipped and the other sector answers.
     * That is the whole point of keeping two: without this check the alternation
     * protected nothing, because the torn header was newer and the record it
     * pointed at was rejected far above, where rejection means wiping the store.
     */
    struct storeHeader a;
    struct storeHeader b;
    bool haveA = readHeader(area, MSG_AREA_A, &a);
    bool haveB = readHeader(area, MSG_AREA_B, &b);
    bool aNewer = haveA && (!haveB || ((int32_t)(a.sequence - b.sequence) > 0));

    size_t got = 0;

    if (aNewer) {
        if (!loadSector(area, MSG_AREA_A, &a, buf, cap, &got) && haveB)
            (void)loadSector(area, MSG_AREA_B, &b, buf, cap, &got);
    } else if (haveB) {
        if (!loadSector(area, MSG_AREA_B, &b, buf, cap, &got) && haveA)
            (void)loadSector(area, MSG_AREA_A, &a, buf, cap, &got);
    }

    (void)off;
    (void)len;
    flash_area_close(area);
    return got;
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

    /*
     * Payload first, header last: the header is what makes a sector count, so
     * it goes into the flash once the bytes it describes are there.  A write
     * cut short leaves a sector with no header at all -- the erase took it --
     * which the reader skips, instead of a header pointing at half a record.
     */
    bool ok = (flash_area_erase(area, off, MSG_SECTOR_SIZE) == 0)
              && (flash_area_write(area, off + sizeof(hdr), buf, len) == 0)
              && (flash_area_write(area, off, &hdr, sizeof(hdr)) == 0);

    /*
     * And read it back: a flash that answered ok and did not keep the bytes is
     * the one failure the alternation cannot catch by itself, because the next
     * write would go to the other sector believing this one is good.
     */
    if (ok) {
        static uint8_t check[64];
        size_t done = 0;

        while (ok && (done < len)) {
            size_t n = (len - done < sizeof(check)) ? (len - done) : sizeof(check);

            ok = (flash_area_read(area, off + sizeof(hdr) + done, check, n) == 0)
                 && (memcmp(check, buf + done, n) == 0);
            done += n;
        }
    }

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
