/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * RAM-backed flash for the host device: the channel bank and the contact book
 * with the same two-slot discipline as the radio, plus a write budget so a test
 * can cut a save in the middle and check that the previous bank survives.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/slot_store.h"

static uint8_t *flash;
static uint32_t flashSize = CHSTORE_PARTITION_SIZE;
static int64_t budget = -1;

static void ensure(void)
{
    if (flash == NULL) {
        flash = malloc(flashSize);
        memset(flash, 0xFF, flashSize);
    }
}

static int memRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;
    ensure();
    if (off + len > flashSize)
        return -1;
    memcpy(buf, flash + off, len);
    return 0;
}

static int memErase(void *ctx, uint32_t off, size_t len)
{
    (void)ctx;
    ensure();
    if (off + len > flashSize)
        return -1;
    memset(flash + off, 0xFF, len);
    return 0;
}

static int memWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    (void)ctx;
    ensure();
    if (off + len > flashSize)
        return -1;
    if (budget >= 0) {
        if ((int64_t)len > budget) {
            memcpy(flash + off, buf, (size_t)budget);
            budget = 0;
            return -1;
        }
        budget -= (int64_t)len;
    }
    memcpy(flash + off, buf, len);
    return 0;
}

static const struct chstorePort chanPort = {
    .read = memRead, .erase = memErase, .write = memWrite,
    .ctx = NULL, .size = CHSTORE_PARTITION_SIZE,
    .sector = CHSTORE_SECTOR_SIZE
};

/* The contact book lives in its own partition, after the channel bank: two
 * 8 KiB slots, the same geometry the radio uses. */
static const slot_ops_t contactOps = {
    .read = memRead, .erase = memErase, .write = memWrite,
    .ctx = NULL, .slot_size = 0x2000, .sector = 0x1000
};

void ram_store_bind(void)
{
    ensure();
    chstore_bind(&chanPort);
    cstore_bind(&contactOps);
}

void ram_store_cut(int64_t bytes)
{
    budget = bytes;
}

void ram_store_heal(void)
{
    budget = -1;
}
