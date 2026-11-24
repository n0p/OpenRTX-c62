/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/slot_store.h"
#include "core/crc.h"
#include <string.h>
#include <errno.h>

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
    uint16_t crc;
    uint16_t reserved;
} __attribute__((packed)) slot_hdr_t;

_Static_assert(sizeof(slot_hdr_t) == SLOT_HDR_SIZE, "slot header size");

/* Read-back verification chunk. Small on purpose: the payload can be a few
   kilobytes and this runs on the UI thread stack. */
#define VERIFY_CHUNK 64U

static bool opsValid(const slot_ops_t *ops)
{
    if ((ops == NULL) || (ops->read == NULL) || (ops->write == NULL)
        || (ops->erase == NULL))
        return false;
    if (ops->slot_size < SLOT_HDR_SIZE || ops->sector == 0)
        return false;
    if ((ops->slot_size % ops->sector) != 0)
        return false;
    return true;
}

/* Header only: the payload CRC is checked once the slot is a candidate, so
   that a broken newest slot still lets the older one load. */
static int slotHeader(const slot_store_t *st, uint32_t slot, slot_hdr_t *hdr)
{
    uint32_t off = slot * st->ops->slot_size;

    if (st->ops->read(st->ops->ctx, off, hdr, sizeof(*hdr)) != 0)
        return -EIO;

    if (hdr->magic != st->magic || hdr->version != st->version)
        return -EINVAL;

    return 0;
}

int slot_store_load(slot_store_t *st, const slot_ops_t *ops, uint32_t magic,
                    uint16_t version, void *payload, size_t len)
{
    if ((st == NULL) || (payload == NULL) || !opsValid(ops))
        return -EINVAL;
    if (len == 0 || len + SLOT_HDR_SIZE > ops->slot_size)
        return -EINVAL;

    memset(st, 0, sizeof(*st));
    st->ops = ops;
    st->magic = magic;
    st->version = version;
    /* A blank store saves into slot 0 first */
    st->slot = 1;
    st->seq = 0;

    slot_hdr_t hdr[2];
    bool valid[2] = { false, false };

    for (uint32_t slot = 0; slot < 2; slot++) {
        if (slotHeader(st, slot, &hdr[slot]) != 0)
            continue;
        if (hdr[slot].length != (uint16_t)len)
            continue;
        valid[slot] = true;
    }

    /* Newest first: if its payload does not verify, try the other one. */
    int order[2];
    if (valid[0] && valid[1]) {
        order[0] = (hdr[0].sequence >= hdr[1].sequence) ? 0 : 1;
        order[1] = 1 - order[0];
    } else {
        order[0] = valid[0] ? 0 : 1;
        order[1] = 1 - order[0];
    }

    for (uint8_t i = 0; i < 2; i++) {
        uint32_t slot = (uint32_t)order[i];
        if (!valid[slot])
            continue;

        uint32_t off = slot * st->ops->slot_size;
        if (st->ops->read(st->ops->ctx, off + SLOT_HDR_SIZE, payload, len) != 0)
            continue;
        if (hdr[slot].crc != crc_ccitt(payload, len))
            continue;

        st->slot = slot;
        st->seq = hdr[slot].sequence;
        return 0;
    }

    return -ENOENT;
}

int slot_store_save(slot_store_t *st, const void *payload, size_t len)
{
    if ((st == NULL) || (payload == NULL) || !opsValid(st->ops))
        return -EINVAL;
    if (len == 0 || len + SLOT_HDR_SIZE > st->ops->slot_size)
        return -EINVAL;

    uint32_t slot = 1 - st->slot;
    uint32_t off = slot * st->ops->slot_size;

    slot_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = st->magic;
    hdr.version = st->version;
    hdr.length = (uint16_t)len;
    hdr.sequence = st->seq + 1;
    hdr.crc = crc_ccitt(payload, len);

    if (st->ops->erase(st->ops->ctx, off, st->ops->slot_size) != 0)
        return -EIO;
    if (st->ops->write(st->ops->ctx, off, &hdr, sizeof(hdr)) != 0)
        return -EIO;
    if (st->ops->write(st->ops->ctx, off + SLOT_HDR_SIZE, payload, len) != 0)
        return -EIO;

    /*
     * Read the header and the payload back through the same path the loader
     * uses: a save that cannot be loaded is not a save. The payload is
     * compared chunk by chunk, so nothing else goes on the stack.
     */
    slot_hdr_t check;
    if (slotHeader(st, slot, &check) != 0 || check.length != hdr.length
        || check.sequence != hdr.sequence || check.crc != hdr.crc)
        return -EIO;

    const uint8_t *src = (const uint8_t *)payload;
    uint8_t chunk[VERIFY_CHUNK];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > sizeof(chunk))
            n = sizeof(chunk);
        if (st->ops->read(st->ops->ctx, off + SLOT_HDR_SIZE + done, chunk,
                          n) != 0)
            return -EIO;
        if (memcmp(chunk, src + done, n) != 0)
            return -EIO;
        done += n;
    }

    st->slot = slot;
    st->seq = hdr.sequence;
    return 0;
}

bool slot_store_blank(const slot_store_t *st)
{
    return (st != NULL) && (st->ops != NULL) && (st->seq == 0)
        && (st->slot == 1);
}
