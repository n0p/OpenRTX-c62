/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file backup_c62.c
 * @brief Flash backend of the internal backup on the Retevis C62.
 *
 * The record lives in the storage partition (0x2B0000, 640 KiB), in the free
 * part of it: settings at +0x0000, VFO at +0x1000, the TETRA SDS history at
 * +0x2000/+0x3000, the message store at +0x4000/+0x5000 and the post-mortem
 * recorder at +0x6000, so the backup takes +0x7000 and up (docs/BACKUP.md).
 *
 * Two areas of 16 KiB, one after the other, is all it needs: the six stores
 * take about 8 KiB together, and the pair is what makes a save never cost the
 * previous record. The remaining 568 KiB of the partition stay free.
 */

#include <errno.h>
#include <zephyr/storage/flash_map.h>

#include "core/backup_image.h"

/** First byte of the backup area pair inside the storage partition. */
#define BKP_BASE 0x7000U

static const struct flash_area *area = NULL;

static int c62Open(void)
{
    if (area != NULL)
        return 0;

    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &area) < 0) {
        area = NULL;
        return -ENODEV;
    }

    return 0;
}

static int c62Read(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;

    if (c62Open() != 0)
        return -ENODEV;

    if ((off + len) > (2U * BKP_AREA_SIZE))
        return -EINVAL;

    return flash_area_read(area, BKP_BASE + off, buf, len);
}

static int c62Erase(void *ctx, uint32_t off, size_t len)
{
    (void)ctx;

    if (c62Open() != 0)
        return -ENODEV;

    if ((off + len) > (2U * BKP_AREA_SIZE))
        return -EINVAL;

    return flash_area_erase(area, BKP_BASE + off, len);
}

static int c62Write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    (void)ctx;

    if (c62Open() != 0)
        return -ENODEV;

    if ((off + len) > (2U * BKP_AREA_SIZE))
        return -EINVAL;

    return flash_area_write(area, BKP_BASE + off, buf, len);
}

static const bkpPort_t c62Port = {
    .read = c62Read,
    .erase = c62Erase,
    .write = c62Write,
    .ctx = NULL,
    .size = 2U * BKP_AREA_SIZE,
    .sector = 0x1000
};

const bkpPort_t *bkp_platformPort(void)
{
    return &c62Port;
}
