/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Handlers of the binary protocol that touch the radio's stores: the channel
 * bank, the contact book, the zones, the settings and the message store.
 *
 * The rule for every write is the same and it is the reason this file exists:
 * validate the whole request first, write, then read the record back and
 * compare it byte by byte before telling the PC that it worked.  A store that
 * accepted the call but did not keep the bytes (a flash that lied, a record
 * that did not fit) is reported as PC_ERR_IO, never as success -- a CPS that
 * believes a write that did not land leaves the operator with a codeplug that
 * disagrees with the radio.
 *
 * The stores themselves are the second half of the guarantee: the channel bank
 * and the contact book are written with the ping-pong of two slots (see
 * docs/CHANNELS.md), so a power cut in the middle of a save costs the save and
 * never the previous bank.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "core/pc_proto.h"
#include "pc_proto_internal.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/crc.h"
#include "core/messaging.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/zones.h"
#ifdef CONFIG_C62_MODE_STATS
#include "core/mode_stats.h"
#endif

/* The channel and contact records travel as the store keeps them: both are
 * packed and both have a version of their own (CHSTORE_VERSION, and the contact
 * bank in slot_store.h), so a client that speaks the same version parses them
 * without a second description to drift. */
_Static_assert(sizeof(chstore_channel_t) == 37, "channel wire record");
_Static_assert(sizeof(cstore_contact_t) == CSTORE_RECORD_SIZE,
               "contact wire record");

/**
 * One channel of a zone, packed for the wire: zone_channel_t as the firmware
 * holds it has alignment padding before the 16-bit fields, which would differ
 * between the two ends of the cable. 28 bytes.
 */
typedef struct
{
    uint8_t  zone;
    uint16_t index;
    char     name[ZONE_NAME_LEN];
    uint32_t rx;
    uint32_t tx;
    int32_t  shift;
    uint8_t  mode;
    uint8_t  bandwidth;
    uint16_t power;
    uint8_t  flags;
}
__attribute__((packed)) pcWireZoneChannel_t;

_Static_assert(sizeof(pcWireZoneChannel_t) == 28, "zone channel wire record");

/** Channels one CHAN_LIST response may carry: (payload - TLVs) / record. */
#define CHAN_BLOCK_MAX ((PC_PROTO_MAX_PAYLOAD - 24) / sizeof(chstore_channel_t))

/** Contacts one CONTACT_LIST response may carry. */
#define CT_BLOCK_MAX ((PC_PROTO_MAX_PAYLOAD - 24) / sizeof(cstore_contact_t))

/*
 * ----------------------------------------------------------------------------
 * Channels
 * ----------------------------------------------------------------------------
 */

/** Copy a record out of the bank into the reply. */
static int chanReply(struct pcReq *r, uint16_t idx)
{
    chstore_channel_t ch;

    if (chstore_get(idx, &ch) != 0) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_INDEX, idx);
    off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_CHANNEL, &ch, sizeof(ch));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/**
 * Write one record and prove it: the store's own write, then a read of the
 * same slot compared with what was sent.
 *
 * @return PC_OK, PC_ERR_INVALID_VALUE or PC_ERR_IO.
 */
static int chanStoreVerified(uint16_t idx, const chstore_channel_t *ch)
{
    if (!chstore_valid(ch))
        return PC_ERR_INVALID_VALUE;
    if (chstore_set(idx, ch) != 0)
        return PC_ERR_IO;

    chstore_channel_t back;
    if (chstore_get(idx, &back) != 0)
        return PC_ERR_IO;
    if (memcmp(&back, ch, sizeof(back)) != 0)
        return PC_ERR_IO; /* the store did not keep what it accepted */
    return PC_OK;
}

int pc_ops_chanCount(struct pcReq *r)
{
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_CHCOUNT,
                            (uint16_t)chstore_count());
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_CHSTORE_VER,
                     CHSTORE_VERSION);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_chanGet(struct pcReq *r)
{
    uint16_t idx;
    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    return chanReply(r, idx);
}

int pc_ops_chanSet(struct pcReq *r)
{
    uint16_t idx;
    const uint8_t *rec;
    uint16_t recLen;

    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx) ||
        !pc_tlv_find(r->in, r->inLen, PC_TAG_CHANNEL, &rec, &recLen) ||
        recLen != sizeof(chstore_channel_t)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    if (idx >= (uint16_t)chstore_count()) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }

    chstore_channel_t ch;
    memcpy(&ch, rec, sizeof(ch));

    int err = chanStoreVerified(idx, &ch);
    if (err != PC_OK) {
        r->err = err;
        return -1;
    }
    /* Answer with the record as it now reads in the bank, not with the copy
     * that was sent: the client gets what the radio really holds. */
    return chanReply(r, idx);
}

int pc_ops_chanAdd(struct pcReq *r)
{
    const uint8_t *rec;
    uint16_t recLen;

    if (!pc_tlv_find(r->in, r->inLen, PC_TAG_CHANNEL, &rec, &recLen) ||
        recLen != sizeof(chstore_channel_t)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    chstore_channel_t ch;
    memcpy(&ch, rec, sizeof(ch));

    if (!chstore_valid(&ch)) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    int idx = chstore_add(&ch);
    if (idx < 0) {
        r->err = (idx == -EINVAL) ? PC_ERR_INVALID_VALUE : PC_ERR_IO;
        return -1;
    }
    if (chstore_get((uint16_t)idx, &ch) != 0 || chstore_get((uint16_t)idx, &ch) != 0) {
        r->err = PC_ERR_IO;
        return -1;
    }
    return chanReply(r, (uint16_t)idx);
}

int pc_ops_chanDelete(struct pcReq *r)
{
    uint16_t idx;
    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    int rc = chstore_delete(idx);
    if (rc != 0) {
        r->err = (rc == -EINVAL) ? PC_ERR_BAD_INDEX : PC_ERR_IO;
        return -1;
    }
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_CHCOUNT,
                            (uint16_t)chstore_count());
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_chanList(struct pcReq *r)
{
    uint16_t first = 0;
    uint16_t count = CHAN_BLOCK_MAX;

    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_FIRST, &first);
    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_COUNT, &count);

    int total = chstore_count();
    if (first >= (uint16_t)total) {
        /* An empty bank and a request for the first block is not an error: the
         * client asked "what do you have" and the answer is nothing. */
        if (total == 0 && first == 0)
            count = 0;
        else {
            r->err = PC_ERR_BAD_INDEX;
            return -1;
        }
    }
    if (count > CHAN_BLOCK_MAX)
        count = CHAN_BLOCK_MAX;
    if (count > (uint16_t)total - first)
        count = (uint16_t)total - first;

    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_FIRST, first);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_COUNT, count);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_CHCOUNT, (uint16_t)total);
    /* One container with count records back to back: the client reads a
     * single array, not one tag per channel. */
    size_t arr = pc_tlv_open(r->out, r->outCap, off, PC_TAG_CHANNEL_LIST);
    if (arr == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    for (uint16_t i = 0; i < count; i++) {
        chstore_channel_t ch;
        if (chstore_get((uint16_t)(first + i), &ch) != 0) {
            r->err = PC_ERR_BAD_INDEX;
            return -1;
        }
        if (arr + sizeof(ch) > r->outCap) {
            r->err = PC_ERR_TOO_LARGE;
            return -1;
        }
        memcpy(r->out + arr, &ch, sizeof(ch));
        arr += sizeof(ch);
    }
    off = pc_tlv_close(r->out, arr, arr - (size_t)count * sizeof(chstore_channel_t));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;

    /* More after this block: the client does not have to guess whether it
     * reached the end, it reads the flag. */
    if (first + count < (uint16_t)total)
        r->flags |= PC_PROTO_FLAG_MORE;
    return 0;
}

int pc_ops_chanWriteBlock(struct pcReq *r)
{
    uint16_t first = 0, count = 0;
    const uint8_t *recs;
    uint16_t recLen;

    if (!pc_tlv_find(r->in, r->inLen, PC_TAG_CHANNEL_LIST, &recs, &recLen)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    if (recLen % sizeof(chstore_channel_t) != 0) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_FIRST, &first);
    count = (uint16_t)(recLen / sizeof(chstore_channel_t));

    int total = chstore_count();
    if (count == 0 || first + count > (uint16_t)total) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }

    /* Every record is checked before anything is written: a block with one bad
     * entry leaves the bank exactly as it was, which is what a CPS wants when
     * it pastes a list. */
    for (uint16_t i = 0; i < count; i++) {
        chstore_channel_t ch;
        memcpy(&ch, recs + (size_t)i * sizeof(ch), sizeof(ch));
        if (!chstore_valid(&ch)) {
            r->err = PC_ERR_INVALID_VALUE;
            r->flags = PC_PROTO_FLAG_MORE; /* reuse: slot in PC_TAG_INDEX */
            size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_INDEX,
                                    (uint16_t)(first + i));
            r->outLen = off;
            return -1;
        }
    }

    uint16_t written = 0;
    for (uint16_t i = 0; i < count; i++) {
        chstore_channel_t ch;
        memcpy(&ch, recs + (size_t)i * sizeof(ch), sizeof(ch));
        int err = chanStoreVerified((uint16_t)(first + i), &ch);
        if (err != PC_OK) {
            /* Partial: report how many landed and which one did not, so the
             * client can retry from there instead of rewriting the block. */
            size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_INDEX,
                                    (uint16_t)(first + i));
            off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_COUNT, written);
            r->outLen = off;
            r->err = err;
            return -1;
        }
        written++;
    }

    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_FIRST, first);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_COUNT, written);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_VERIFY, 1);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_chanLastUsed(struct pcReq *r)
{
    uint16_t idx;
    if (pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx)) {
        if (idx >= (uint16_t)chstore_count() && idx != CHSTORE_NONE) {
            r->err = PC_ERR_BAD_INDEX;
            return -1;
        }
        chstore_setLastUsed(idx);
        if (chstore_commit() != 0) {
            r->err = PC_ERR_IO;
            return -1;
        }
    }
    int last = chstore_lastUsed();
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_LASTUSED,
                            (uint16_t)(last < 0 ? CHSTORE_NONE : last));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Contacts
 * ----------------------------------------------------------------------------
 */

static int contactReply(struct pcReq *r, uint16_t idx)
{
    cstore_contact_t c;

    if (cstore_get(idx, &c) != 0) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_INDEX, idx);
    off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_CONTACT, &c, sizeof(c));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

static int contactStoreVerified(uint16_t idx, const cstore_contact_t *c)
{
    if (!cstore_valid(c))
        return PC_ERR_INVALID_VALUE;
    if (cstore_set(idx, c) != 0)
        return PC_ERR_IO;
    cstore_contact_t back;
    if (cstore_get(idx, &back) != 0 || memcmp(&back, c, sizeof(back)) != 0)
        return PC_ERR_IO;
    return PC_OK;
}

int pc_ops_contactCount(struct pcReq *r)
{
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_CTCOUNT,
                            (uint16_t)cstore_count());
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_contactGet(struct pcReq *r)
{
    uint16_t idx;
    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    return contactReply(r, idx);
}

int pc_ops_contactSet(struct pcReq *r)
{
    uint16_t idx;
    const uint8_t *rec;
    uint16_t recLen;

    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx) ||
        !pc_tlv_find(r->in, r->inLen, PC_TAG_CONTACT, &rec, &recLen) ||
        recLen != sizeof(cstore_contact_t)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    if (idx >= (uint16_t)cstore_count()) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }
    cstore_contact_t c;
    memcpy(&c, rec, sizeof(c));

    int err = contactStoreVerified(idx, &c);
    if (err != PC_OK) {
        r->err = err;
        return -1;
    }
    return contactReply(r, idx);
}

int pc_ops_contactAdd(struct pcReq *r)
{
    const uint8_t *rec;
    uint16_t recLen;

    if (!pc_tlv_find(r->in, r->inLen, PC_TAG_CONTACT, &rec, &recLen) ||
        recLen != sizeof(cstore_contact_t)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    cstore_contact_t c;
    memcpy(&c, rec, sizeof(c));

    if (!cstore_valid(&c)) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    int idx = cstore_add(&c);
    if (idx < 0) {
        r->err = (idx == -EINVAL) ? PC_ERR_INVALID_VALUE : PC_ERR_IO;
        return -1;
    }
    return contactReply(r, (uint16_t)idx);
}

int pc_ops_contactDelete(struct pcReq *r)
{
    uint16_t idx;
    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    int rc = cstore_delete(idx);
    if (rc != 0) {
        r->err = (rc == -EINVAL) ? PC_ERR_BAD_INDEX : PC_ERR_IO;
        return -1;
    }
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_CTCOUNT,
                            (uint16_t)cstore_count());
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_contactList(struct pcReq *r)
{
    uint16_t first = 0;
    uint16_t count = CT_BLOCK_MAX;

    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_FIRST, &first);
    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_COUNT, &count);

    int total = cstore_count();
    if (first >= (uint16_t)total) {
        if (total == 0 && first == 0)
            count = 0;
        else {
            r->err = PC_ERR_BAD_INDEX;
            return -1;
        }
    }
    if (count > CT_BLOCK_MAX)
        count = CT_BLOCK_MAX;
    if (count > (uint16_t)total - first)
        count = (uint16_t)total - first;

    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_FIRST, first);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_COUNT, count);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_CTCOUNT, (uint16_t)total);
    size_t arr = pc_tlv_open(r->out, r->outCap, off, PC_TAG_CONTACT_LIST);
    if (arr == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    for (uint16_t i = 0; i < count; i++) {
        cstore_contact_t c;
        if (cstore_get((uint16_t)(first + i), &c) != 0) {
            r->err = PC_ERR_BAD_INDEX;
            return -1;
        }
        if (arr + sizeof(c) > r->outCap) {
            r->err = PC_ERR_TOO_LARGE;
            return -1;
        }
        memcpy(r->out + arr, &c, sizeof(c));
        arr += sizeof(c);
    }
    off = pc_tlv_close(r->out, arr,
                       arr - (size_t)count * sizeof(cstore_contact_t));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    if (first + count < (uint16_t)total)
        r->flags |= PC_PROTO_FLAG_MORE;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Zones
 * ----------------------------------------------------------------------------
 */

int pc_ops_zoneCount(struct pcReq *r)
{
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_ZONE_COUNT,
                            (uint16_t)zones_count());
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_zoneGet(struct pcReq *r)
{
    uint8_t id;
    if (!pc_tlv_get_u8(r->in, r->inLen, PC_TAG_ZONE, &id)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    const zone_t *z = zones_get(id);
    if (z == NULL) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }
    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_ZONE, z->id);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_ZONE_KIND, z->id);
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_ZONE_NAME,
                     z->descr != NULL ? z->descr : "");
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_ZONE_PREFIX,
                     z->prefix != NULL ? z->prefix : "");
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_ZONE_COUNT, z->count);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_MODE, z->mode);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_zoneChannel(struct pcReq *r)
{
    uint8_t id;
    uint16_t index;

    if (!pc_tlv_get_u8(r->in, r->inLen, PC_TAG_ZONE, &id) ||
        !pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &index)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    zone_channel_t zc;
    if (zones_channel(id, index, &zc) != 0) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }

    pcWireZoneChannel_t w;
    memset(&w, 0, sizeof(w));
    w.zone = zc.zone;
    w.index = zc.index;
    memcpy(w.name, zc.name, sizeof(w.name));
    w.rx = zc.rx;
    w.tx = zc.tx;
    w.shift = zc.shift;
    w.mode = zc.mode;
    w.bandwidth = zc.bandwidth;
    w.power = zc.power;
    w.flags = zc.flags;

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_ZONE, id);
    off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_ZONE_CHANNEL, &w,
                       sizeof(w));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_zoneImport(struct pcReq *r)
{
    uint8_t id;
    uint16_t index;

    if (!pc_tlv_get_u8(r->in, r->inLen, PC_TAG_ZONE, &id) ||
        !pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &index)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    chstore_channel_t ch;
    if (zones_toStore(id, index, &ch) != 0) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }
    int slot = chstore_add(&ch);
    if (slot < 0) {
        r->err = (slot == -EINVAL) ? PC_ERR_INVALID_VALUE : PC_ERR_IO;
        return -1;
    }
    return chanReply(r, (uint16_t)slot);
}

/*
 * ----------------------------------------------------------------------------
 * Settings
 * ----------------------------------------------------------------------------
 */

enum settingsKind
{
    SK_U8,
    SK_I8,
    SK_U32,
    SK_STR,
    SK_U16,
    SK_M17CAN /**< the low nibble of a byte that also holds the timer    */
};

/** The settings a CPS may read and write by name, one row per field. */
static const struct settingsField
{
    uint16_t       tag;
    uint8_t        kind;
    uint16_t       off;   /**< offsetof(settings_t, field)               */
    uint8_t        len;   /**< bytes for SK_STR, ignored otherwise       */
}
settingsFields[] =
{
    { PC_TAG_SET_CALLSIGN,   SK_STR, offsetof(settings_t, callsign),
      sizeof(((settings_t *)0)->callsign) },
    { PC_TAG_SET_BRIGHTNESS, SK_U8,  offsetof(settings_t, brightness), 0 },
    { PC_TAG_SET_CONTRAST,   SK_U8,  offsetof(settings_t, contrast), 0 },
    { PC_TAG_SET_SQLLEVEL,   SK_U8,  offsetof(settings_t, sqlLevel), 0 },
    { PC_TAG_SET_VOXLEVEL,   SK_U8,  offsetof(settings_t, voxLevel), 0 },
    { PC_TAG_SET_TZ,         SK_I8,  offsetof(settings_t, utc_timezone), 0 },
    { PC_TAG_SET_DMR_ID,     SK_U32, offsetof(settings_t, dmr_id), 0 },
    { PC_TAG_SET_DMR_DEST,   SK_U32, offsetof(settings_t, dmr_dest_id), 0 },
    { PC_TAG_SET_DMR_TYPE,   SK_U8,  offsetof(settings_t, dmr_dest_type), 0 },
    { PC_TAG_SET_DMR_TS,     SK_U8,  offsetof(settings_t, dmr_timeslot), 0 },
    { PC_TAG_SET_M17_CAN,    SK_M17CAN, 0, 0 },
    { PC_TAG_SET_M17_DEST,   SK_STR, offsetof(settings_t, m17_dest),
      sizeof(((settings_t *)0)->m17_dest) },
};

#define SETTINGS_FIELDS (sizeof(settingsFields) / sizeof(settingsFields[0]))

static const struct settingsField *settingsField(uint16_t tag)
{
    for (size_t i = 0; i < SETTINGS_FIELDS; i++) {
        if (settingsFields[i].tag == tag)
            return &settingsFields[i];
    }
    return NULL;
}

/** Append the current value of a field to the reply. */
static size_t settingsPut(const struct settingsField *f,
                          const settings_t *s, uint8_t *buf, size_t cap,
                          size_t off)
{
    const uint8_t *base = (const uint8_t *)s + f->off;

    switch (f->kind) {
        case SK_U8:
            return pc_tlv_u8(buf, cap, off, f->tag, *base);
        case SK_I8:
            return pc_tlv_i8(buf, cap, off, f->tag, (int8_t)*base);
        case SK_U16:
            return pc_tlv_u16(buf, cap, off, f->tag,
                              (uint16_t)(base[0] | (base[1] << 8)));
        case SK_U32:
            return pc_tlv_u32(buf, cap, off, f->tag,
                              (uint32_t)base[0] | ((uint32_t)base[1] << 8) |
                              ((uint32_t)base[2] << 16) |
                              ((uint32_t)base[3] << 24));
        case SK_M17CAN:
            /* m17_can shares its byte with display_timer, so it cannot be a
             * byte field: read the member itself. */
            return pc_tlv_u8(buf, cap, off, f->tag, (uint8_t)s->m17_can);
        case SK_STR: {
            char text[32];
            size_t n = 0;
            while (n < f->len && base[n] != 0)
                n++;
            if (n >= sizeof(text))
                n = sizeof(text) - 1;
            memcpy(text, base, n);
            text[n] = '\0';
            return pc_tlv_str(buf, cap, off, f->tag, text);
        }
        default:
            return 0;
    }
}

/** Range-check and apply one field to a settings copy. */
static int settingsApply(const struct settingsField *f, settings_t *s,
                         const uint8_t *tlv, size_t tlvLen)
{
    uint8_t *base = (uint8_t *)s + f->off;
    uint8_t u8;
    int8_t i8;
    uint32_t u32;
    const uint8_t *str;
    uint16_t strLen;

    switch (f->kind) {
        case SK_U8:
            if (!pc_tlv_get_u8(tlv, tlvLen, f->tag, &u8))
                return PC_ERR_BAD_REQUEST;
            switch (f->tag) {
                case PC_TAG_SET_BRIGHTNESS:
                    if (u8 > 100)
                        return PC_ERR_INVALID_VALUE;
                    break;
                case PC_TAG_SET_DMR_TYPE:
                    if (u8 > DMR_DEST_GROUP)
                        return PC_ERR_INVALID_VALUE;
                    break;
                case PC_TAG_SET_DMR_TS:
                    if (u8 < 1 || u8 > 2)
                        return PC_ERR_INVALID_VALUE;
                    break;
                case PC_TAG_SET_M17_CAN:
                    if (u8 > 15)
                        return PC_ERR_INVALID_VALUE;
                    break;
                default:
                    break;
            }
            *base = u8;
            return PC_OK;

        case SK_I8:
            if (!pc_tlv_get_i8(tlv, tlvLen, f->tag, &i8))
                return PC_ERR_BAD_REQUEST;
            if (i8 < -24 || i8 > 28)
                return PC_ERR_INVALID_VALUE;
            *base = (uint8_t)i8;
            return PC_OK;

        case SK_U32:
            if (!pc_tlv_get_u32(tlv, tlvLen, f->tag, &u32))
                return PC_ERR_BAD_REQUEST;
            if (u32 > 16777215u) /* 24 bits on the air */
                return PC_ERR_INVALID_VALUE;
            base[0] = (uint8_t)(u32 & 0xFF);
            base[1] = (uint8_t)((u32 >> 8) & 0xFF);
            base[2] = (uint8_t)((u32 >> 16) & 0xFF);
            base[3] = (uint8_t)(u32 >> 24);
            return PC_OK;

        case SK_STR: {
            if (!pc_tlv_find(tlv, tlvLen, f->tag, &str, &strLen))
                return PC_ERR_BAD_REQUEST;
            if (strLen >= f->len)
                return PC_ERR_TOO_LARGE;
            memset(base, 0, f->len);
            memcpy(base, str, strLen);
            return PC_OK;
        }

        case SK_M17CAN:
            if (!pc_tlv_get_u8(tlv, tlvLen, f->tag, &u8))
                return PC_ERR_BAD_REQUEST;
            if (u8 > 15)
                return PC_ERR_INVALID_VALUE;
            s->m17_can = u8; /* the bitfield, next to display_timer */
            return PC_OK;

        default:
            return PC_ERR_NOT_SUPPORTED;
    }
}

int pc_ops_settingsGet(struct pcReq *r)
{
    settings_t s;

    pthread_mutex_lock(&state_mutex);
    memcpy(&s, &state.settings, sizeof(s));
    pthread_mutex_unlock(&state_mutex);

    size_t off = 0;
    bool anyField = false;

    /* One pass over the request: a tag present means "this field".  No tags at
     * all means the whole record, which is what a CPS that wants to keep a
     * backup asks for. */
    size_t scan = 0;
    while (scan + 4 <= r->inLen) {
        uint16_t tag = (uint16_t)(r->in[scan] | (r->in[scan + 1] << 8));
        const struct settingsField *f = settingsField(tag);
        if (f == NULL) {
            r->err = PC_ERR_INVALID_VALUE;
            return -1;
        }
        off = settingsPut(f, &s, r->out, r->outCap, off);
        if (off == 0) {
            r->err = PC_ERR_TOO_LARGE;
            return -1;
        }
        anyField = true;
        uint16_t l = (uint16_t)(r->in[scan + 2] | (r->in[scan + 3] << 8));
        scan += 4 + l;
    }

    if (!anyField) {
        off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_SETTINGS_VER,
                         SETTINGS_VERSION);
        off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_SETTINGS_SIZE,
                         (uint16_t)sizeof(s));
        off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_SETTINGS_CRC,
                         crc_ccitt_update(0xFFFF, &s, sizeof(s)));
        off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_SETTINGS, &s,
                           sizeof(s));
    }
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_settingsSet(struct pcReq *r)
{
    settings_t work, back;

    if (r->inLen == 0) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }

    pthread_mutex_lock(&state_mutex);
    memcpy(&work, &state.settings, sizeof(work));
    pthread_mutex_unlock(&state_mutex);

    /* Apply to a copy first: one bad field must not leave the radio with half
     * of the new settings. */
    size_t scan = 0;
    while (scan + 4 <= r->inLen) {
        uint16_t tag = (uint16_t)(r->in[scan] | (r->in[scan + 1] << 8));
        uint16_t l = (uint16_t)(r->in[scan + 2] | (r->in[scan + 3] << 8));
        if (scan + 4 + l > r->inLen) {
            r->err = PC_ERR_BAD_REQUEST;
            return -1;
        }
        const struct settingsField *f = settingsField(tag);
        if (f == NULL) {
            r->err = PC_ERR_INVALID_VALUE;
            return -1;
        }
        int err = settingsApply(f, &work, r->in, r->inLen);
        if (err != PC_OK) {
            r->err = err;
            return -1;
        }
        scan += 4 + l;
    }

    pthread_mutex_lock(&state_mutex);
    memcpy(&state.settings, &work, sizeof(work));
    memcpy(&back, &state.settings, sizeof(back));
    pthread_mutex_unlock(&state_mutex);

    /* The debouncer in state_task() sees the new CRC and writes the record to
     * flash a couple of seconds later, so a CPS edit lands the same way an
     * edit on the keypad does.  What is verified here is the read-back of the
     * RAM copy, which is what the rest of the firmware will use. */
    if (memcmp(&back, &work, sizeof(back)) != 0) {
        r->err = PC_ERR_IO;
        return -1;
    }

    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_SETTINGS_VER,
                            SETTINGS_VERSION);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_SETTINGS_CRC,
                     crc_ccitt_update(0xFFFF, &back, sizeof(back)));
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_VERIFY, 1);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_settingsInfo(struct pcReq *r)
{
    settings_t s;

    pthread_mutex_lock(&state_mutex);
    memcpy(&s, &state.settings, sizeof(s));
    pthread_mutex_unlock(&state_mutex);

    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_SETTINGS_VER,
                            SETTINGS_VERSION);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_SETTINGS_SIZE,
                     (uint16_t)sizeof(s));
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_SETTINGS_CRC,
                     crc_ccitt_update(0xFFFF, &s, sizeof(s)));
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_UPTIME_MS, r->p->nowMs);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Live status
 * ----------------------------------------------------------------------------
 */

int pc_ops_statusGet(struct pcReq *r)
{
    pcProtoStatus_t s;
    pc_proto_status(&s);

    size_t off = pc_proto_statusTlv(&s, r->out, r->outCap, 0);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_UPTIME_MS, r->p->nowMs);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_statusSubscribe(struct pcReq *r)
{
    uint16_t period = 0;

    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_PERIOD_MS, &period);

    /* The floor keeps the UART usable: at 100 ms a status frame of about 60
     * bytes is 600 B/s of a 11.5 kB/s cable. */
    if (period != 0 && period < 50)
        period = 50;

    r->p->subPeriodMs = period;
    r->p->subNextMs = r->p->nowMs + period;

    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_PERIOD_MS, period);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_statsGet(struct pcReq *r)
{
#ifdef CONFIG_C62_MODE_STATS
    char text[128];
    size_t off = 0;

    for (int mode = 0; mode < MS_MODE_COUNT; mode++) {
        int n = modeStats_format((enum modeStatsMode)mode, text,
                                 sizeof(text));
        if (n <= 0)
            continue;
        off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_STATS, text,
                           (size_t)n);
        if (off == 0) {
            r->err = PC_ERR_TOO_LARGE;
            return -1;
        }
    }
    if (off == 0) {
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }
    r->outLen = off;
    return 0;
#else
    r->err = PC_ERR_NOT_SUPPORTED;
    return -1;
#endif
}

/*
 * ----------------------------------------------------------------------------
 * Messaging
 * ----------------------------------------------------------------------------
 */

static size_t messagePut(const struct message *m, uint8_t *buf, size_t cap,
                         size_t off)
{
    off = pc_tlv_u8(buf, cap, off, PC_TAG_MSG_STORE_ID, m->id);
    off = pc_tlv_u8(buf, cap, off, PC_TAG_MSG_TRANSPORT, m->transport);
    off = pc_tlv_u8(buf, cap, off, PC_TAG_MSG_STATE, m->state);
    off = pc_tlv_u8(buf, cap, off, PC_TAG_MSG_FLAGS, m->flags);
    off = pc_tlv_u32(buf, cap, off, PC_TAG_MSG_PEER_ID,
                     m->flags & MSG_FLAG_INBOUND ? m->src : m->dst);
    off = pc_tlv_str(buf, cap, off, PC_TAG_MSG_PEER, m->peer);
    off = pc_tlv_str(buf, cap, off, PC_TAG_MSG_TEXT, m->text);
    off = pc_tlv_u32(buf, cap, off, PC_TAG_UPTIME_MS, m->time);
    return off;
}

int pc_ops_msgCount(struct pcReq *r)
{
    int inbox = 0;
    while (messaging_inbox(inbox) != NULL && inbox < 64)
        inbox++;

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_MSG_INBOX,
                           (uint8_t)inbox);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_MSG_OUTBOX,
                    (uint8_t)messaging_outCount());
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_MSG_UNREAD,
                    (uint8_t)messaging_unread());
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_msgInbox(struct pcReq *r)
{
    uint16_t idx;
    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    const struct message *m = messaging_inbox(idx);
    if (m == NULL) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_INDEX, idx);
    off = messagePut(m, r->out, r->outCap, off);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_msgOutbox(struct pcReq *r)
{
    uint16_t idx;
    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_INDEX, &idx)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    const struct message *m = messaging_outbox(idx);
    if (m == NULL) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_INDEX, idx);
    off = messagePut(m, r->out, r->outCap, off);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_msgSend(struct pcReq *r)
{
    uint8_t transport;
    uint32_t peerId = 0;
    const uint8_t *peerP;
    const uint8_t *textP;
    uint16_t peerLen = 0, textLen = 0;
    char peer[24];
    char text[MSG_TEXT_MAX + 1];

    if (!pc_tlv_get_u8(r->in, r->inLen, PC_TAG_MSG_TRANSPORT, &transport) ||
        transport >= MSG_TRANSPORT_COUNT) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    if (!pc_tlv_find(r->in, r->inLen, PC_TAG_MSG_TEXT, &textP, &textLen) ||
        textLen == 0 || textLen > MSG_TEXT_MAX) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    pc_tlv_get_u32(r->in, r->inLen, PC_TAG_MSG_PEER_ID, &peerId);

    peer[0] = '\0';
    if (pc_tlv_find(r->in, r->inLen, PC_TAG_MSG_PEER, &peerP, &peerLen)) {
        if (peerLen >= sizeof(peer)) {
            r->err = PC_ERR_TOO_LARGE;
            return -1;
        }
        memcpy(peer, peerP, peerLen);
        peer[peerLen] = '\0';
    }
    if (peer[0] == '\0' && peerId == 0) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    memcpy(text, textP, textLen);
    text[textLen] = '\0';

    bool confirm = false;
    uint8_t flags = 0;
    if (pc_tlv_get_u8(r->in, r->inLen, PC_TAG_MSG_FLAGS, &flags))
        confirm = (flags & MSG_FLAG_CONFIRM) != 0;

    int id;
    if (peer[0] != '\0' && peerId != 0)
        id = messaging_sendAddr((enum msgTransport)transport, peerId, peer,
                                text, confirm, r->p->nowMs);
    else if (peer[0] != '\0')
        id = messaging_sendToText((enum msgTransport)transport, peer, text,
                                  confirm, r->p->nowMs);
    else
        id = messaging_send((enum msgTransport)transport, peerId, text,
                           confirm, r->p->nowMs);
    if (id < 0) {
        r->err = PC_ERR_BUSY; /* outbox full or the text was refused */
        return -1;
    }

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_MSG_STORE_ID,
                           (uint8_t)id);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_MSG_STATE,
                    MSG_STATE_QUEUED);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_msgDelete(struct pcReq *r)
{
    uint8_t id;
    if (!pc_tlv_get_u8(r->in, r->inLen, PC_TAG_MSG_STORE_ID, &id)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    if (messaging_find(id) == NULL) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }
    /* Cancel, not erase: a message already on the air finishes on the air and
     * its late outcome is ignored, which is the store's own rule. */
    messaging_cancel(id, r->p->nowMs);
    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_MSG_STORE_ID, id);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_ops_msgMarkRead(struct pcReq *r)
{
    uint8_t id;
    if (pc_tlv_get_u8(r->in, r->inLen, PC_TAG_MSG_STORE_ID, &id))
        messaging_markReadId(id);
    else
        messaging_markRead();

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_MSG_UNREAD,
                           (uint8_t)messaging_unread());
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Reserved blocks
 * ----------------------------------------------------------------------------
 */

int pc_ops_reserved(struct pcReq *r)
{
    r->err = PC_ERR_NOT_SUPPORTED;
    return -1;
}
