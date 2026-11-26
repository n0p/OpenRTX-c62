/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file user_store_host.c
 * @brief The channel bank and the contact book of the simulator, in a file.
 *
 * On the radio both stores live in the OEM channel partition
 * (platform/drivers/NVM/user_store_c62.c): the channel bank in its first 16 KiB
 * and the contact book at 0x8000. The host has no flash, and the emulator's NVM
 * area (platform/drivers/NVM/nvmem_linux.c) is 1 KiB of settings, so without
 * this file every save from the channel editor answers WRITE FAILED and the
 * screens that read the bank stay empty: the simulator could not check the one
 * operation the operator does most often.
 *
 * This backend keeps the same layout in one image file, `.emulator_userstore.img`
 * in the working directory (OPENRTX_SIM_STORE overrides the path), so a tour
 * that creates a channel or a contact gets a real save, a real read-back and a
 * bank that survives the run. Erase is a write of 0xFF, as on NOR flash.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/slot_store.h"

/** Size of the image: the two stores, 16 KiB each, as on the radio. */
#define IMG_SIZE 0x10000U
#define CSTORE_BASE 0x8000U

static FILE *image = NULL;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int openImage(void)
{
    if (image != NULL)
        return 0;

    const char *path = getenv("OPENRTX_SIM_STORE");
    if (path == NULL || path[0] == '\0')
        path = ".emulator_userstore.img";

    image = fopen(path, "r+b");
    if (image == NULL) {
        image = fopen(path, "w+b");
        if (image == NULL)
            return -EIO;

        /* A blank part: every byte erased */
        uint8_t ff[512];
        memset(ff, 0xFF, sizeof(ff));
        for (uint32_t written = 0; written < IMG_SIZE; written += sizeof(ff)) {
            if (fwrite(ff, 1, sizeof(ff), image) != sizeof(ff)) {
                fclose(image);
                image = NULL;
                return -EIO;
            }
        }
        fflush(image);
    }

    return 0;
}

static int imgRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    pthread_mutex_lock(&lock);
    int rc = openImage();
    uint32_t pos = (uint32_t)(uintptr_t)ctx + off;

    if (rc == 0) {
        if ((pos + len) > IMG_SIZE || fseek(image, (long)pos, SEEK_SET) != 0
            || fread(buf, 1, len, image) != len)
            rc = -EIO;
    }

    pthread_mutex_unlock(&lock);
    return rc;
}

static int imgErase(void *ctx, uint32_t off, size_t len)
{
    pthread_mutex_lock(&lock);
    int rc = openImage();
    uint32_t pos = (uint32_t)(uintptr_t)ctx + off;

    if (rc == 0) {
        if ((pos + len) > IMG_SIZE || fseek(image, (long)pos, SEEK_SET) != 0)
            rc = -EIO;
        else {
            uint8_t ff[512];
            memset(ff, 0xFF, sizeof(ff));
            for (size_t done = 0; done < len && rc == 0; done += sizeof(ff)) {
                size_t chunk = (len - done < sizeof(ff)) ? len - done :
                                                           sizeof(ff);
                if (fwrite(ff, 1, chunk, image) != chunk)
                    rc = -EIO;
            }
            fflush(image);
        }
    }

    pthread_mutex_unlock(&lock);
    return rc;
}

static int imgWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    pthread_mutex_lock(&lock);
    int rc = openImage();
    uint32_t pos = (uint32_t)(uintptr_t)ctx + off;

    if (rc == 0) {
        if ((pos + len) > IMG_SIZE || fseek(image, (long)pos, SEEK_SET) != 0
            || fwrite(buf, 1, len, image) != len)
            rc = -EIO;
        else
            fflush(image);
    }

    pthread_mutex_unlock(&lock);
    return rc;
}

/* ---------------------------------------------------------------- channels -- */

static const struct chstorePort channelPort = { .read = imgRead,
                                                .erase = imgErase,
                                                .write = imgWrite,
                                                .ctx = (void *)(uintptr_t)0,
                                                .size = 0x4000,
                                                .sector = CHSTORE_SECTOR_SIZE };

const struct chstorePort *chstore_platformPort(void)
{
    return &channelPort;
}

/* ---------------------------------------------------------------- contacts -- */

static const slot_ops_t contactOps = { .read = imgRead,
                                       .erase = imgErase,
                                       .write = imgWrite,
                                       .ctx = (void *)(uintptr_t)CSTORE_BASE,
                                       .slot_size = 0x2000,
                                       .sector = 0x1000 };

const slot_ops_t *cstore_platformOps(void)
{
    return &contactOps;
}
