/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SDS message ring and its flash record.
 *
 * Record layout, all fields big endian, text is 7-bit ASCII:
 *
 *   0  "SDJ1"      4  magic
 *   4  version     1  1
 *   5  count       1  0..SDS_HIST_LEN, newest first
 *   6  entries     1+1+3+3+2+1+len each:
 *                    kind, flags, src (24), dst (24), status (16), len, text
 *   n  crc16       2  CRC-16/CCITT-FALSE over bytes 0..n-1
 *
 * Worst case 6 + 8 * (11 + SDS_TEXT_MAX) + 2 = 608 bytes, one 4 KiB sector
 * with room to spare; the firmware writes it to the second half of a pair of
 * sectors to level the erase wear.
 */

#include <string.h>

#include "protocols/TETRA/SdsHist.h"
#include "core/psram_state.h"

static struct sds_hist_entry hist[SDS_HIST_LEN] PSRAM_STATE;
static int histCount;
static int histUnread;

void sds_hist_reset(void)
{
    memset(hist, 0, sizeof(hist));
    histCount = 0;
    histUnread = 0;
}

int sds_hist_push(const struct sds_msg *m, uint32_t tick)
{
    struct sds_hist_entry *e;

    if (m == NULL || !sds_msg_has_text(m))
        return -1;

    /* Newest at index 0: shift the older ones up. */
    if (histCount < SDS_HIST_LEN)
        histCount++;
    memmove(&hist[1], &hist[0], (size_t)(histCount - 1) * sizeof(hist[0]));
    e = &hist[0];
    memset(e, 0, sizeof(*e));
    e->src_ssi = m->src_ssi;
    e->dst_ssi = m->dst_ssi;
    e->tick = tick;
    e->kind = (uint8_t)m->kind;
    e->status = m->status;
    e->flags = SDS_HIST_UNREAD;
    if (!m->complete || m->truncated)
        e->flags |= SDS_HIST_INCOMPLETE;
    e->text_len = (uint8_t)((m->text_len > SDS_TEXT_MAX) ? SDS_TEXT_MAX
                                                         : m->text_len);
    memcpy(e->text, m->text, e->text_len);
    e->text[e->text_len] = '\0';
    /* A full ring drops the oldest, so the count can never exceed it. */
    if (histUnread < histCount)
        histUnread++;
    return 0;
}

int sds_hist_count(void)
{
    return histCount;
}

const struct sds_hist_entry *sds_hist_get(int i)
{
    if (i < 0 || i >= histCount)
        return NULL;
    return &hist[i];
}

int sds_hist_unread(void)
{
    return histUnread;
}

void sds_hist_mark_read(void)
{
    for (int i = 0; i < histCount; i++)
        hist[i].flags &= (uint8_t)~SDS_HIST_UNREAD;
    histUnread = 0;
}

static uint16_t crc16_ccitt(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xffff;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)buf[i] << 8);
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t sds_hist_record_size(void)
{
    size_t n = 6;

    for (int i = 0; i < histCount; i++)
        n += 11 + hist[i].text_len;
    return n + 2;
}

int sds_hist_serialise(uint8_t *buf, size_t cap)
{
    size_t n = 6;

    if (buf == NULL || sds_hist_record_size() > cap)
        return -1;

    memcpy(buf, "SDJ1", 4);
    buf[4] = 1;
    buf[5] = (uint8_t)histCount;
    for (int i = 0; i < histCount; i++) {
        const struct sds_hist_entry *e = &hist[i];

        buf[n++] = e->kind;
        buf[n++] = e->flags;
        buf[n++] = (uint8_t)(e->src_ssi >> 16);
        buf[n++] = (uint8_t)(e->src_ssi >> 8);
        buf[n++] = (uint8_t)e->src_ssi;
        buf[n++] = (uint8_t)(e->dst_ssi >> 16);
        buf[n++] = (uint8_t)(e->dst_ssi >> 8);
        buf[n++] = (uint8_t)e->dst_ssi;
        buf[n++] = (uint8_t)(e->status >> 8);
        buf[n++] = (uint8_t)e->status;
        buf[n++] = e->text_len;
        memcpy(&buf[n], e->text, e->text_len);
        n += e->text_len;
    }
    uint16_t crc = crc16_ccitt(buf, n);

    buf[n++] = (uint8_t)(crc >> 8);
    buf[n++] = (uint8_t)crc;
    return (int)n;
}

int sds_hist_deserialise(const uint8_t *buf, size_t len)
{
    size_t n = 6;
    unsigned count;

    sds_hist_reset();
    if (buf == NULL || len < 8 || memcmp(buf, "SDJ1", 4) != 0 || buf[4] != 1)
        return -1;

    count = buf[5];
    if (count > SDS_HIST_LEN)
        return -1;

    uint16_t want = (uint16_t)((uint16_t)buf[len - 2] << 8 | buf[len - 1]);

    if (crc16_ccitt(buf, len - 2) != want)
        return -1;

    for (unsigned i = 0; i < count; i++) {
        struct sds_hist_entry *e = &hist[i];
        uint8_t tlen;

        if (n + 11 > len - 2)
            return -1;
        memset(e, 0, sizeof(*e));
        e->kind = buf[n++];
        e->flags = (uint8_t)(buf[n++] & (uint8_t)~SDS_HIST_UNREAD);
        e->src_ssi = (uint32_t)buf[n] << 16 | (uint32_t)buf[n + 1] << 8 | buf[n + 2];
        n += 3;
        e->dst_ssi = (uint32_t)buf[n] << 16 | (uint32_t)buf[n + 1] << 8 | buf[n + 2];
        n += 3;
        e->status = (uint16_t)((uint16_t)buf[n] << 8 | buf[n + 1]);
        n += 2;
        tlen = buf[n++];
        if (tlen > SDS_TEXT_MAX || n + tlen > len - 2)
            return -1;
        memcpy(e->text, &buf[n], tlen);
        e->text[tlen] = '\0';
        e->text_len = tlen;
        n += tlen;
    }
    histCount = (int)count;
    histUnread = 0;
    return histCount;
}
