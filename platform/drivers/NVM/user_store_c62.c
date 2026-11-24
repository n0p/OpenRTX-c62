/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <zephyr/storage/flash_map.h>
#include <errno.h>
#include "core/contact_store.h"
#include "core/msg_store.h"

/*
 * Flash backend for the contact book and the message store.
 *
 * Both live in the same partition as the channel bank (the OEM channel area,
 * 256 KiB at 0x3C0000, see platform/targets/c62/c62.dts), which the channel
 * store only uses up to 0x4000. The map is:
 *
 *   0x00000  channel store, two 8 KiB slots
 *   0x08000  contact book,   two 8 KiB slots
 *   0x10000  message store,  two 8 KiB slots
 *   0x18000+ free
 *
 * Keeping the three stores in one partition means no devicetree change and a
 * single place to look when something does not survive a power cycle.
 */

#define CSTORE_BASE 0x08000U
#define MSTORE_BASE 0x10000U
#define USLOT_SIZE  0x2000U
#define USECTOR     0x1000U

static const struct flash_area *area = NULL;

static int openArea(void)
{
    if (area != NULL)
        return 0;
    if (flash_area_open(FIXED_PARTITION_ID(channels_partition), &area) < 0) {
        area = NULL;
        return -ENODEV;
    }
    return 0;
}

static int uRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    if (openArea() != 0)
        return -ENODEV;
    return flash_area_read(area, (uint32_t)(uintptr_t)ctx + off, buf, len);
}

static int uErase(void *ctx, uint32_t off, size_t len)
{
    if (openArea() != 0)
        return -ENODEV;
    return flash_area_erase(area, (uint32_t)(uintptr_t)ctx + off, len);
}

static int uWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    if (openArea() != 0)
        return -ENODEV;
    return flash_area_write(area, (uint32_t)(uintptr_t)ctx + off, buf, len);
}

static const slot_ops_t contactOps = {
    .read = uRead,
    .erase = uErase,
    .write = uWrite,
    .ctx = (void *)(uintptr_t)CSTORE_BASE,
    .slot_size = USLOT_SIZE,
    .sector = USECTOR
};

static const slot_ops_t msgOps = {
    .read = uRead,
    .erase = uErase,
    .write = uWrite,
    .ctx = (void *)(uintptr_t)MSTORE_BASE,
    .slot_size = USLOT_SIZE,
    .sector = USECTOR
};

const slot_ops_t *cstore_platformOps(void)
{
    return &contactOps;
}

const slot_ops_t *msg_platformOps(void)
{
    return &msgOps;
}
