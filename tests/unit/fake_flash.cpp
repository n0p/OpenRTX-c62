/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The RAM flash partition behind tests/unit/fake_zephyr/zephyr/storage/
 * flash_map.h.  24 KiB, which is the two 4 KiB sectors the message store uses
 * at 0x4000 and 0x5000 plus room for the ones before them, and no locking: the
 * tests drive it from one thread on purpose.
 */

#include <stdbool.h>
#include <string.h>

#include "fake_zephyr/zephyr/storage/flash_map.h"

#define FAKE_SIZE (24u * 1024u)

static uint8_t s_bytes[FAKE_SIZE];
static int s_cutAfter = -1;
static int s_readFails = 0;
static int s_readPass = 0;


int flash_area_open(uint8_t id, const struct flash_area **out)
{
    static struct flash_area area = {0, FAKE_SIZE};

    if (id != storage_partition || out == NULL)
        return -1;

    *out = &area;
    return 0;
}

void flash_area_close(const struct flash_area *area)
{
    (void)area;
}

int flash_area_read(const struct flash_area *area, uint32_t off, void *dst,
                    size_t len)
{
    if (s_readPass > 0) {
        s_readPass--;
    } else if (s_readFails > 0) {
        s_readFails--;
        return -5; /* -EIO */
    }
    if (area == NULL || dst == NULL || off > area->size
        || len > (size_t)(area->size - off))
        return -22;

    memcpy(dst, &s_bytes[off], len);
    return 0;
}

int flash_area_write(const struct flash_area *area, uint32_t off,
                     const void *src, size_t len)
{
    if (area == NULL || src == NULL || off > area->size
        || len > (size_t)(area->size - off))
        return -22;

    if (s_cutAfter >= 0) {
        /*
         * A power cut in the middle of the write: the bytes that made it are
         * on the flash, the rest is not, and the caller is told so.
         */
        size_t part = (size_t)s_cutAfter;

        if (part > len)
            part = len;
        memcpy(&s_bytes[off], src, part);
        s_cutAfter = -1;
        return -5;
    }

    memcpy(&s_bytes[off], src, len);
    return 0;
}

int flash_area_erase(const struct flash_area *area, uint32_t off, size_t len)
{
    if (area == NULL || off > area->size || len > (size_t)(area->size - off))
        return -22;

    memset(&s_bytes[off], 0xFF, len);
    return 0;
}

void fake_flash_reset(void)
{
    memset(s_bytes, 0xFF, sizeof(s_bytes));
    s_cutAfter = -1;
    s_readFails = 0;
    s_readPass = 0;
}

uint8_t *fake_flash_bytes(size_t *size)
{
    if (size != NULL)
        *size = sizeof(s_bytes);
    return s_bytes;
}

void fake_flash_cut_write_after(size_t bytes)
{
    s_cutAfter = (int)bytes;
}

void fake_flash_fail_reads(int count)
{
    s_readFails = count;
    s_readPass = 0;
}

void fake_flash_fail_reads_after(int pass, int count)
{
    s_readPass = pass;
    s_readFails = count;
}
