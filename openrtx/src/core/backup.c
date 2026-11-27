/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/backup.h"
#include "core/xmodem.h"
#include <string.h>
#include "drivers/NVM/W25Qx.h"

#if defined(PLATFORM_GD77) || defined(PLATFORM_DM1801)
static const size_t EFLASH_SIZE = 1024*1024;    // 1 MB
#else
static const size_t EFLASH_SIZE = 16*1024*1024; // 16 MB
#endif

size_t  memAddr = 0;

static int getDataCallback(uint8_t *ptr, size_t size)
{
    if((memAddr + size) > EFLASH_SIZE) return -1;
    W25Qx_readData(memAddr, ptr, size);
    memAddr += size;
    return 0;
}

void eflash_dump()
{
    memAddr = 0;
    W25Qx_wakeup();
    xmodem_sendData(EFLASH_SIZE, getDataCallback);
}

/*
 * There is no eflash_restore() any more: it wrote the whole 16 MB back over
 * the programming interface -- bootloader included, sectors erased only when a
 * block happened to start on a 4 KiB boundary, pages written without respecting
 * the chip's page limit -- and nothing called it.  A restore is
 * core/backup_image.c now: the radio's own stores, in the radio's own
 * partition, with the stores' read-back checks (docs/BACKUP.md).
 */
