/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/nvmem_access.h"
#include "core/nvmem_device.h"
#include <stdint.h>
#include <errno.h>

const struct nvmDescriptor *nvm_getDesc(const uint32_t index)
{
    if (index >= nvmTab.nbAreas)
        return NULL;

    return &nvmTab.areas[index];
}

int nvm_getPart(const uint32_t idx, const uint32_t part,
                struct nvmPartition *pInfo)
{
    const struct nvmDescriptor *desc = nvm_getDesc(idx);
    if (desc == NULL)
        return -EINVAL;

    if (part == 0) {
        pInfo->offset = 0;
        pInfo->size = desc->size;
    } else {
        if (part > desc->nbPart)
            return -EINVAL;

        *pInfo = desc->partitions[part - 1];
    }

    return 0;
}

int nvm_read(const uint32_t idx, const uint32_t part, uint32_t offset,
             void *data, size_t len)
{
    const struct nvmDescriptor *nvm;
    struct nvmPartition np;
    int ret;

    ret = nvm_getPart(idx, part, &np);
    if (ret < 0)
        return ret;

    /*
     * Accesses exceeding the partition are caught by this check, provided that
     * the partition table is well defined.  It is written as a subtraction
     * because offset and len are 32 bit and the caller chooses the offset: a
     * large one made offset+len wrap round and pass, and the access that
     * followed went to whatever the same flash holds at baseAddr+offset --
     * another partition, the DSP image, the factory area.
     */
    if ((offset > np.size) || (len > (np.size - offset)))
        return -EINVAL;

    // Index already validated by nvm_getPart
    nvm = &nvmTab.areas[idx];
    offset += np.offset;

    return nvm_devRead(nvm->dev, nvm->baseAddr + offset, data, len);
}

int nvm_write(const uint32_t idx, const uint32_t part, uint32_t offset,
              const void *data, size_t len)
{
    const struct nvmDescriptor *nvm;
    struct nvmPartition np;
    int ret;

    ret = nvm_getPart(idx, part, &np);
    if (ret < 0)
        return ret;

    if ((offset > np.size) || (len > (np.size - offset)))
        return -EINVAL;

    nvm = &nvmTab.areas[idx];
    offset += np.offset;

    return nvm_devWrite(nvm->dev, nvm->baseAddr + offset, data, len);
}

int nvm_erase(const uint32_t idx, const uint32_t part, uint32_t offset,
              size_t size)
{
    const struct nvmDescriptor *nvm;
    struct nvmPartition np;
    int ret;

    ret = nvm_getPart(idx, part, &np);
    if (ret < 0)
        return ret;

    if ((offset > np.size) || (size > (np.size - offset)))
        return -EINVAL;

    nvm = &nvmTab.areas[idx];
    offset += np.offset;

    return nvm_devErase(nvm->dev, nvm->baseAddr + offset, size);
}
