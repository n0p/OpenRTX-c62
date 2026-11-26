/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file backup_host.c
 * @brief The internal backup of the simulator, in a file of its own.
 *
 * On the radio the record lives in the free part of the storage partition
 * (platform/targets/c62/backup_c62.c). The host has no partition, and the NVM
 * file of the emulator (platform/drivers/NVM/nvmem_linux.c) is 1 KiB of
 * settings, so the backup gets a file next to it: `backup.img` in the state
 * directory the run is using, that is `$XDG_STATE_HOME/OpenRTX/backup.img` when
 * the variable is set and `~/.local/state/OpenRTX/backup.img` otherwise.
 *
 * That is what lets the tour of the simulator (tools/simulator/tour.py) check
 * the round trip: it gives the run its own XDG_STATE_HOME, takes a backup,
 * changes the radio and restores it, and the record it restores is a file the
 * run created.
 *
 * Only the simulator links this file: the unit tests get no port at all and
 * bind their own RAM one, so a test never writes in the developer's home.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/backup_image.h"
#include "interfaces/platform.h"

/** Size of the image: the two areas, as on the radio. */
#define IMG_SIZE (2U * BKP_AREA_SIZE)

static FILE *image = NULL;

/** Build the path of the image, creating the state directory if needed. */
static bool imagePath(char *out, size_t cap)
{
    const char *state = getenv("XDG_STATE_HOME");
    char dir[256];

    if (state != NULL && state[0] != '\0') {
        if (snprintf(dir, sizeof(dir), "%s/OpenRTX", state) >=
            (int)sizeof(dir))
            return false;
    } else {
        const char *home = getenv("HOME");

        if (home == NULL || home[0] == '\0')
            return false;

        if (snprintf(dir, sizeof(dir), "%s/.local/state/OpenRTX", home) >=
            (int)sizeof(dir))
            return false;
    }

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return false;

    if (snprintf(out, cap, "%s/backup.img", dir) >= (int)cap)
        return false;

    return true;
}

static int openImage(void)
{
    if (image != NULL)
        return 0;

    char path[320];

    if (!imagePath(path, sizeof(path)))
        return -ENODEV;

    image = fopen(path, "r+b");
    if (image == NULL) {
        image = fopen(path, "w+b");
        if (image == NULL)
            return -EIO;

        /* A blank part: every byte erased, as the radio's flash comes up */
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
    (void)ctx;

    int rc = openImage();
    if (rc != 0)
        return rc;

    if ((off + len) > IMG_SIZE || fseek(image, (long)off, SEEK_SET) != 0 ||
        fread(buf, 1, len, image) != len)
        return -EIO;

    return 0;
}

static int imgErase(void *ctx, uint32_t off, size_t len)
{
    (void)ctx;

    int rc = openImage();
    if (rc != 0)
        return rc;

    if ((off + len) > IMG_SIZE || fseek(image, (long)off, SEEK_SET) != 0)
        return -EIO;

    uint8_t ff[512];
    memset(ff, 0xFF, sizeof(ff));

    for (size_t done = 0; done < len; done += sizeof(ff)) {
        size_t chunk = (len - done < sizeof(ff)) ? (len - done) : sizeof(ff);

        if (fwrite(ff, 1, chunk, image) != chunk)
            return -EIO;
    }

    fflush(image);
    return 0;
}

static int imgWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    (void)ctx;

    int rc = openImage();
    if (rc != 0)
        return rc;

    if ((off + len) > IMG_SIZE || fseek(image, (long)off, SEEK_SET) != 0 ||
        fwrite(buf, 1, len, image) != len)
        return -EIO;

    fflush(image);
    return 0;
}

static const bkpPort_t hostPort = {
    .read = imgRead,
    .erase = imgErase,
    .write = imgWrite,
    .ctx = NULL,
    .size = IMG_SIZE,
    .sector = 0x1000
};

const bkpPort_t *bkp_platformPort(void)
{
    return &hostPort;
}

bool bkp_platformTime(datetime_t *out)
{
    if (out == NULL)
        return false;

    *out = platform_getCurrentTime();
    return true;
}
