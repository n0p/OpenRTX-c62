/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * SDS messages for the TETRA mode: decode, keep the last few, page them on
 * the main screen and persist them across power cycles.
 *
 * Nonvolatile storage: two 4 KiB sectors of the storage partition, written
 * alternately so that the erase wear is shared and a power cut during a
 * write damages at most the sector being written -- the other one still
 * holds the previous set. Each sector starts with a small header holding a
 * magic, a sequence number (the newest wins) and the record length; the
 * record itself is the packed message list of SdsHist, which carries its
 * own magic and CRC16, so a torn write is rejected when reading back.
 *
 * The C62 storage partition is 640 KiB and only uses its first 8 KiB
 * (settings at +0x0000, VFO at +0x1000), leaving +0x2000 and +0x3000 free.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/storage/flash_map.h>

#include "core/crc.h"
#ifdef CONFIG_MESSAGING
#include "core/messaging.h"
#include "core/psram_state.h"
#endif
#include "interfaces/delays.h"
#include "protocols/TETRA/TetraSds.h"
#include "protocols/TETRA/TetraSdsEnc.h"

#define SDS_AREA_A 0x2000
#define SDS_AREA_B 0x3000
#define SDS_SECTOR_SIZE 0x1000

#define SDS_RECORD_MAGIC 0x5344534aU /* "SDSJ" */
#define SDS_RECORD_VERSION 1
#define SDS_RECORD_MAX 700 /* 6 + 8 * (11 + 64) + 2, see SdsHist.c */
#define SDS_WRITE_DEBOUNCE_MS 5000
#define TETRASDS_PAGE_MS 4000

/* Header in front of the packed messages. */
struct sdsHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t length; /* bytes of the packed message record that follow */
    uint32_t sequence;
};

static bool dirty;
static long long lastChange;
static int pageIndex;
static long long pageTick;

/* Static, not on a thread stack: the packed record is up to 608 bytes. */
static uint8_t recBuf[SDS_RECORD_MAX] PSRAM_STATE;

int tetraSds_onEvent(const struct tetra_sds *s)
{
    struct sds_msg m;

    if (sds_msg_decode(s, &m) != 0 || !sds_msg_has_text(&m))
        return -1;
    if (sds_hist_push(&m, (uint32_t)getTick()) != 0)
        return -1;

    dirty = true;
    lastChange = getTick();
    pageIndex = 0;
    pageTick = lastChange;
    return 0;
}

int tetraSds_count(void)
{
    return sds_hist_count();
}

const struct sds_hist_entry *tetraSds_get(int i)
{
    return sds_hist_get(i);
}

int tetraSds_unread(void)
{
    return sds_hist_unread();
}

void tetraSds_markRead(void)
{
    sds_hist_mark_read();
}

const char *tetraSds_line(char *out, size_t len, long long tick)
{
    int count = sds_hist_count();
    int unread = sds_hist_unread();
    const struct sds_hist_entry *e;

    if (out == NULL || len == 0)
        return "";
    out[0] = '\0';
    if (count == 0)
        return out;

    /* Page through the unread ones, one every TETRASDS_PAGE_MS; once they
     * have all been shown, stay on the newest. */
    if (tick - pageTick >= TETRASDS_PAGE_MS) {
        pageTick = tick;
        if (unread > 0) {
            if (pageIndex + 1 < unread) {
                pageIndex++;
            } else {
                sds_hist_mark_read();
                pageIndex = 0;
            }
        } else {
            pageIndex = 0;
        }
    }
    if (pageIndex >= count)
        pageIndex = 0;

    e = sds_hist_get(pageIndex);
    if (e == NULL)
        return out;

    if (count > 1)
        snprintf(out, len, "%lu%s %d/%d: %s", (unsigned long)e->src_ssi,
                 (e->flags & SDS_HIST_UNREAD) != 0 ? "*" : "", pageIndex + 1,
                 count, e->text);
    else
        snprintf(out, len, "%lu: %s", (unsigned long)e->src_ssi, e->text);
    return out;
}

bool tetraSds_dirty(void)
{
    return dirty;
}

void tetraSds_clear(void)
{
    sds_hist_reset();
    dirty = true;
    lastChange = getTick();
}

/* ---- nonvolatile record ---- */

/* Read and check a sector header. */
static bool readHeader(const struct flash_area *area, uint32_t off,
                       struct sdsHeader *hdr)
{
    if (flash_area_read(area, off, hdr, sizeof(*hdr)) < 0)
        return false;
    if (hdr->magic != SDS_RECORD_MAGIC || hdr->version != SDS_RECORD_VERSION)
        return false;
    if (hdr->length == 0 || hdr->length > SDS_RECORD_MAX)
        return false;
    return true;
}

/* Read the record of a sector whose header has been validated into recBuf. */
static bool readPayload(const struct flash_area *area, uint32_t off,
                        const struct sdsHeader *hdr)
{
    return flash_area_read(area, off + sizeof(*hdr), recBuf, hdr->length) == 0;
}

/* Read one sector and restore the store from it; -1 if it is not usable. */
static int trySector(const struct flash_area *area, uint32_t off,
                     struct sdsHeader *hdr)
{
    if (!readHeader(area, off, hdr) || !readPayload(area, off, hdr))
        return -1;
    return sds_hist_deserialise(recBuf, hdr->length);
}

void tetraSds_load(void)
{
    const struct flash_area *area;
    struct sdsHeader a, b;
    bool haveA, haveB, aNewer;

    sds_hist_reset();
    dirty = false;
    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &area) < 0)
        return;

    haveA = readHeader(area, SDS_AREA_A, &a);
    haveB = readHeader(area, SDS_AREA_B, &b);
    aNewer = haveA && (!haveB || (int32_t)(a.sequence - b.sequence) > 0);

    /*
     * Newest first, but a record whose own CRC does not check out (a write
     * cut short) falls back to the other sector, which still holds the
     * previous set: an interrupted update costs the last message, never all
     * of them.
     */
    if (aNewer) {
        if (trySector(area, SDS_AREA_A, &a) < 0)
            trySector(area, SDS_AREA_B, &b);
    } else if (haveB) {
        if (trySector(area, SDS_AREA_B, &b) < 0)
            trySector(area, SDS_AREA_A, &a);
    }

    flash_area_close(area);
}

void tetraSds_task(void)
{
    const struct flash_area *area;
    struct sdsHeader a, b, hdr;
    bool haveA, haveB;
    uint32_t off = SDS_AREA_A, seq = 1;
    int len;

    if (!dirty || (getTick() - lastChange) < SDS_WRITE_DEBOUNCE_MS)
        return;

    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &area) < 0)
        return;

    /* Headers first: the record is packed into recBuf afterwards, and
     * reading a sector would overwrite it. */
    haveA = readHeader(area, SDS_AREA_A, &a);
    haveB = readHeader(area, SDS_AREA_B, &b);

    /* Write the sector with the older record, numbered one above the newest
     * sequence seen: a sector whose write was cut short still has a valid
     * header, so its number must not be reused. */
    if (haveA && haveB) {
        uint32_t newest = ((int32_t)(b.sequence - a.sequence) > 0) ? b.sequence
                                                                  : a.sequence;

        off = ((int32_t)(a.sequence - b.sequence) < 0) ? SDS_AREA_A : SDS_AREA_B;
        seq = newest + 1;
    } else if (haveA) {
        off = SDS_AREA_B;
        seq = a.sequence + 1;
    } else if (haveB) {
        off = SDS_AREA_A;
        seq = b.sequence + 1;
    }

    len = sds_hist_serialise(recBuf, sizeof(recBuf));
    if (len < 0) {
        flash_area_close(area);
        return;
    }

    hdr.magic = SDS_RECORD_MAGIC;
    hdr.version = SDS_RECORD_VERSION;
    hdr.length = (uint16_t)len;
    hdr.sequence = seq;

    bool ok = flash_area_erase(area, off, SDS_SECTOR_SIZE) == 0 &&
              flash_area_write(area, off, &hdr, sizeof(hdr)) == 0 &&
              flash_area_write(area, off + sizeof(hdr), recBuf,
                               (size_t)len) == 0;

    dirty = !ok; /* a failed write is retried on the next call */
    flash_area_close(area);
}

/* ---- uplink: the SDS of the shared message store ---- */

#ifdef CONFIG_MESSAGING

/** The last PDU encoded, for the transmitter of a future build and for the
 *  diagnostics of this one. */
static struct sds_tx prepared PSRAM_STATE;
static int preparedId = -1;

int tetraSds_prepareQueued(void)
{
    const struct message *msg = messaging_head(MSG_TRANSPORT_TETRA);

    if (msg == NULL) {
        preparedId = -1;
        return -1;
    }
    if (msg->id == preparedId)
        return (int)prepared.nbits; /* already encoded, nothing to do */

    /*
     * Individual call (SSI) with no SDS-TL wrapper: what a radio sends for a
     * plain text.  A text outside the GSM 7-bit alphabet goes as UCS-2.
     */
    if (tetraSds_encodeText(&prepared, msg->dst, msg->text, false) != 0)
        return -1;

    preparedId = msg->id;
    return (int)prepared.nbits;
}

unsigned tetraSds_preparedBits(void)
{
    return (preparedId >= 0) ? prepared.nbits : 0;
}

const uint8_t *tetraSds_preparedPdu(void)
{
    return prepared.buf;
}
#endif /* CONFIG_MESSAGING */
