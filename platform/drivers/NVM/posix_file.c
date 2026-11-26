/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/nvmem.h"
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "posix_file.h"

int posixFile_init(struct nvmFileDevice *dev, const char *fileName,
                   const size_t size)
{
    // Test if file exists, if it doesn't create it.
    int flags = O_RDWR;
    int ret = access(fileName, F_OK);
    if (ret != 0)
        flags |= O_CREAT | O_EXCL;

    // Open file
    int fd = open(fileName, flags, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return fd;

    // Truncate to size, pad with zeroes if extending.
    ftruncate(fd, size);

    dev->fd = fd;

    return (flags & O_CREAT) ? 1 : 0;
}

int posixFile_terminate(struct nvmFileDevice *dev)
{
    if (dev->fd < 0)
        return -EBADF;

    fsync(dev->fd);
    close(dev->fd);

    dev->fd = -1;

    return 0;
}

/**
 * Read from the file.
 *
 * The result is a negative errno: the NVM API is written against "< 0 is a
 * failure" (see nvmem_access.c and every driver of this port), and returning a
 * bare `errno` here -- which is positive -- made a failed access look like a
 * successful one to every caller that tests the sign. A short read is an error
 * too: the file is truncated to the size of the part at init, so a partial
 * read means the medium is not the one the partition table describes.
 */
static int nvm_api_read(const struct nvmDevice *dev, uint32_t offset,
                        void *data, size_t len)
{
    struct nvmFileDevice *pDev = (struct nvmFileDevice *)(dev);

    if (pDev->fd < 0)
        return -EBADF;

    if (lseek(pDev->fd, (off_t)offset, SEEK_SET) != (off_t)offset)
        return -errno;

    ssize_t ret = read(pDev->fd, data, len);
    if (ret < 0)
        return -errno;
    if ((size_t)ret != len)
        return -EIO;

    return 0;
}

/**
 * Write to the file. Same sign convention as the read above, and the same
 * check for a short write: a write that did not take every byte is a failure,
 * not a partial success.
 */
static int nvm_api_write(const struct nvmDevice *dev, uint32_t offset,
                         const void *data, size_t len)
{
    struct nvmFileDevice *pDev = (struct nvmFileDevice *)(dev);

    if (pDev->fd < 0)
        return -EBADF;

    if (data == NULL)
        return -EINVAL;

    if (lseek(pDev->fd, (off_t)offset, SEEK_SET) != (off_t)offset)
        return -errno;

    ssize_t ret = write(pDev->fd, data, len);
    if (ret < 0)
        return -errno;
    if ((size_t)ret != len)
        return -EIO;

    return 0;
}

const struct nvmOps posix_file_ops = {
    .read = nvm_api_read,
    .write = nvm_api_write,
    .erase = NULL,
    .sync = NULL,
};

const struct nvmInfo posix_file_info = {
    .write_size = 1,
    .erase_size = 1,
    .erase_cycles = INT_MAX,
    .device_info = NVM_FILE | NVM_WRITE,
};
