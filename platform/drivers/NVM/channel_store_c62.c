/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include "core/channel_store.h"

/*
 * Channel bank backend for the C62: the channels_partition area of the SPI NOR
 * flash, declared in platform/targets/c62/c62.dts. It is the OEM channel bank
 * at 0x3C0000 (256 KiB): the factory memories are not preserved, reverting to
 * the stock firmware means reflashing the whole flash anyway. Only the first
 * CHSTORE_PARTITION_SIZE bytes of the partition are used by the store.
 */

static const struct flash_area *area = NULL;

static int c62Open(void)
{
    if (area != NULL)
        return 0;

    if (flash_area_open(FIXED_PARTITION_ID(channels_partition), &area) < 0) {
        area = NULL;
        return -ENODEV;
    }

    return 0;
}

static int c62Read(void *ctx, uint32_t offset, void *buf, size_t len)
{
    (void)ctx;

    if (c62Open() != 0)
        return -ENODEV;

    return flash_area_read(area, offset, buf, len);
}

static int c62Erase(void *ctx, uint32_t offset, size_t len)
{
    (void)ctx;

    if (c62Open() != 0)
        return -ENODEV;

    return flash_area_erase(area, offset, len);
}

static int c62Write(void *ctx, uint32_t offset, const void *buf, size_t len)
{
    (void)ctx;

    if (c62Open() != 0)
        return -ENODEV;

    return flash_area_write(area, offset, buf, len);
}

static const struct chstorePort c62Port = {
    .read = c62Read,
    .erase = c62Erase,
    .write = c62Write,
    .ctx = NULL,
    .size = CHSTORE_PARTITION_SIZE,
    .sector = CHSTORE_SECTOR_SIZE
};

const struct chstorePort *chstore_platformPort(void)
{
    return &c62Port;
}
