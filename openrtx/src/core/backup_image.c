/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/backup_image.h"
#include "core/blackbox.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/crc.h"
#include "core/messaging.h"
#include "core/messaging_store.h"
#include "core/psram_state.h"
#include "core/settings_record.h"
#include "core/state.h"
#include "core/zones.h"
#include "interfaces/nvmem.h"
#include "rtx/rtx.h"

#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#endif

#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The internal backup of the radio's own stores. docs/BACKUP.md is the
 * reference of the format and of the two rules that make it safe (a save never
 * costs the previous record, a restore verifies everything before it writes
 * anything).
 *
 * Layout of one record, little endian, everything packed:
 *
 *    0  32  header (bkpHeader_t): magic, version, sequence, sizes, CRC-16,
 *           the clock and the directory of the six stores
 *   32  n   the payloads, one after the other, in directory order
 *
 * The header is written *last*, after the payload, and only then is the record
 * read back and verified: an area whose save was cut short has no usable
 * header, so the other area stays the newest valid record.
 */

/** Offset of the second area: the first one starts at the port's base. */
#define BKP_AREA_B BKP_AREA_SIZE

/** Directory entry of one store. */
typedef struct
{
    uint8_t  id;       /**< enum bkpEntry                                  */
    uint8_t  flags;    /**< reserved, zero                                 */
    uint16_t length;   /**< payload bytes, as the store's format knows it   */
    uint32_t offset;   /**< from the start of the record, 4-byte aligned    */
}
__attribute__((packed)) bkpDirEntry_t;

/**
 * Header of a record, written once the payload is on flash.
 *
 * 80 bytes and a 4-byte aligned directory on purpose: the flash of the radio
 * only takes writes aligned to its 4-byte write unit, so the payload of an
 * entry is stored padded to that unit and the offsets of the entries are
 * multiples of it (BKP_ALIGN). The `length` keeps the store's own size, which
 * is what the store's parser needs; the padding is 0xFF, the erased value.
 */
typedef struct
{
    uint32_t magic;     /**< BKP_MAGIC                                    */
    uint16_t version;   /**< BKP_VERSION                                  */
    uint16_t hdrSize;   /**< sizeof(bkpHeader_t)                          */
    uint32_t sequence;  /**< write counter, the newest valid one wins     */
    uint32_t totalSize; /**< header plus padded payload, bytes            */
    uint32_t crc;       /**< CRC-16 of this header (crc field zeroed) and
                         *   of the padded payload bytes that follow     */
    uint16_t count;     /**< entries in the directory                     */
    datetime_t when;    /**< clock when it was taken, zero if unknown     */
    uint8_t  reserved;
    uint16_t pad;       /**< zero, aligns the directory to BKP_ALIGN      */
    bkpDirEntry_t dir[BKP_E_COUNT];
}
__attribute__((packed)) bkpHeader_t;

/*
 * The packed store the engine hands to flash: a full channel bank is the
 * biggest of the six (4738 B), and 4864 B leaves room for one that grows by a
 * record. PSRAM on the radio: it is touched twice per backup and twice per
 * restore, never in a loop that has to keep up with audio (core/psram_state.h).
 */
static uint8_t scratch[BKP_MAX_ENTRY] PSRAM_STATE;

static const bkpPort_t *port = NULL;
static bool portBound = false;

/** Text of the last backup or restore, for the screen. */
static char lastResult[40] = "READY";

/** Chunk used to stream a record through the CRC. */
#define BKP_CRC_CHUNK 64U

/* ------------------------------------------------------------- platform --- */

__attribute__((weak)) const bkpPort_t *bkp_platformPort(void)
{
    return NULL;
}

/**
 * Clock of the record header. A platform with a real-time clock overrides it;
 * the radio has none, so the field stays zero and the screens show it as such
 * instead of inventing a date.
 */
__attribute__((weak)) bool bkp_platformTime(datetime_t *out)
{
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    return false;
}

void bkp_bind(const bkpPort_t *p)
{
    port = p;
    portBound = true;
}

static const bkpPort_t *getPort(void)
{
    if (!portBound) {
        port = bkp_platformPort();
        portBound = true;
    }

    if (port == NULL || port->read == NULL || port->write == NULL)
        return NULL;

    return port;
}

static void logLine(const char *fmt, ...)
{
    char line[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /*
     * The line already says which way it went ("backup: ..." or
     * "restore: ..."): on the radio printk lands in the PC protocol's log ring
     * and in the console, and on the host it is the test's stdout.
     */
#ifdef __ZEPHYR__
    printk("%s\n", line);
#else
    printf("%s\n", line);
#endif
}

const char *bkp_resultText(int result)
{
    switch (result) {
        case BKP_OK:            return "OK";
        case BKP_ERR_NO_PORT:   return "NO STORAGE";
        case BKP_ERR_NO_BACKUP: return "NO BACKUP";
        case BKP_ERR_CORRUPT:   return "CRC BAD";
        case BKP_ERR_VERSION:   return "VERSION";
        case BKP_ERR_TOO_BIG:   return "TOO BIG";
        case BKP_ERR_IO:        return "WRITE FAILED";
        case BKP_ERR_STORE:     return "STORE FAILED";
        case BKP_ERR_PARAM:     return "BAD ARG";
        default:                return "ERROR";
    }
}

const char *bkp_lastResult(void)
{
    return lastResult;
}

/* ---------------------------------------------------------------- areas --- */

static int areaRead(const bkpPort_t *p, uint8_t area, uint32_t off, void *buf,
                    size_t len)
{
    return p->read(p->ctx, (uint32_t)area * BKP_AREA_SIZE + off, buf, len);
}

static int areaWrite(const bkpPort_t *p, uint8_t area, uint32_t off,
                     const void *buf, size_t len)
{
    return p->write(p->ctx, (uint32_t)area * BKP_AREA_SIZE + off, buf, len);
}

static uint16_t headerCrc(const bkpHeader_t *h, uint16_t crc)
{
    bkpHeader_t copy = *h;
    copy.crc = 0;
    return crc_ccitt_update(crc, &copy, sizeof(copy));
}

/**
 * Check a header on its own: magic, version, size, count and directory.
 * The CRC of the payload is a separate, slower pass (verifyRecord).
 */
static int checkHeader(const bkpHeader_t *h)
{
    if (h->magic != BKP_MAGIC)
        return BKP_ERR_NO_BACKUP;

    if (h->version != BKP_VERSION)
        return BKP_ERR_VERSION;

    if (h->hdrSize != sizeof(bkpHeader_t) || h->count > BKP_E_COUNT ||
        h->totalSize > BKP_AREA_SIZE || h->totalSize < sizeof(bkpHeader_t))
        return BKP_ERR_CORRUPT;

    for (uint16_t i = 0; i < h->count; i++) {
        const bkpDirEntry_t *e = &h->dir[i];

        if (e->id >= BKP_E_COUNT || e->length > BKP_MAX_ENTRY ||
            e->offset < sizeof(bkpHeader_t) ||
            (uint32_t)e->offset + e->length > h->totalSize)
            return BKP_ERR_CORRUPT;

        /* An entry may appear once: a duplicated id is a damaged directory */
        for (uint16_t j = 0; j < i; j++) {
            if (h->dir[j].id == e->id)
                return BKP_ERR_CORRUPT;
        }
    }

    return BKP_OK;
}

/** Stream the whole record through the CRC. Everything, before any write. */
static int verifyRecord(const bkpPort_t *p, uint8_t area, const bkpHeader_t *h)
{
    uint8_t chunk[BKP_CRC_CHUNK];
    uint16_t crc = headerCrc(h, 0);
    uint32_t off = (uint32_t)sizeof(bkpHeader_t);

    while (off < h->totalSize) {
        size_t n = (size_t)(h->totalSize - off);

        if (n > sizeof(chunk))
            n = sizeof(chunk);

        if (areaRead(p, area, off, chunk, n) != 0)
            return BKP_ERR_IO;

        crc = crc_ccitt_update(crc, chunk, n);
        off += (uint32_t)n;
    }

    return (crc == h->crc) ? BKP_OK : BKP_ERR_CORRUPT;
}

/**
 * Find the newest valid record: both headers are read, the newest by sequence
 * whose CRC checks out wins. Nothing is written.
 *
 * When neither area holds a usable record the reason matters, because the
 * screen shows it: a part that was never written is "no backup", one whose
 * record is damaged is a CRC error and one written by another firmware is a
 * version error. Only the areas whose magic is right are worth that report;
 * everything else is an erased or foreign area.
 */
static int findNewest(const bkpPort_t *p, bkpHeader_t *out, uint8_t *area)
{
    bkpHeader_t h[2];
    bool ok[2] = { false, false };
    int  reason = BKP_ERR_NO_BACKUP;

    for (uint8_t a = 0; a < 2; a++) {
        if (areaRead(p, a, 0, &h[a], sizeof(h[a])) != 0)
            continue;

        if (h[a].magic != BKP_MAGIC)
            continue;

        int rc = checkHeader(&h[a]);

        if (rc == BKP_OK)
            rc = verifyRecord(p, a, &h[a]);

        if (rc == BKP_OK)
            ok[a] = true;
        else
            reason = rc;
    }

    if (!ok[0] && !ok[1])
        return reason;

    uint8_t pick = ok[0] ? 0 : 1;

    if (ok[0] && ok[1]) {
        /* Wraparound-safe comparison, as the slot stores do it */
        pick = ((int32_t)(h[0].sequence - h[1].sequence) > 0) ? 0 : 1;
    }

    *out = h[pick];
    *area = pick;
    return BKP_OK;
}

/* --------------------------------------------------------------- stores --- */

/** Length of the payload of one store, or a negative bkpResult. */
static int entryPack(uint8_t id, void *buf, size_t cap);

static int entrySize(uint8_t id)
{
    int n = entryPack(id, scratch, sizeof(scratch));
    return (n < 0) ? BKP_ERR_STORE : n;
}

static int packSettings(void *buf, size_t cap)
{
    if (cap < SETTINGS_RECORD_SIZE)
        return -ENOSPC;

    return settings_recordPack((uint8_t *)buf, cap, &state.settings);
}

static int packVfo(void *buf, size_t cap)
{
    if (cap < sizeof(channel_t))
        return -ENOSPC;

    memcpy(buf, &state.channel, sizeof(channel_t));
    return (int)sizeof(channel_t);
}

static int packChannels(void *buf, size_t cap)
{
    return chstore_export(buf, cap);
}

static int packContacts(void *buf, size_t cap)
{
    return cstore_export(buf, cap);
}

static int packMessages(void *buf, size_t cap)
{
    return messaging_serialise((uint8_t *)buf, cap);
}

/** The zone entry: the plan of this build and the direction of its shift. */
static int packZones(void *buf, size_t cap)
{
    if (cap < 8)
        return -ENOSPC;

    uint8_t *out = (uint8_t *)buf;
    uint32_t signature = zones_signature();

    out[0] = (uint8_t)zones_count();
    out[1] = zones_shiftInverted() ? 1 : 0;
    out[2] = 0;
    out[3] = 0;
    out[4] = (uint8_t)(signature & 0xFF);
    out[5] = (uint8_t)((signature >> 8) & 0xFF);
    out[6] = (uint8_t)((signature >> 16) & 0xFF);
    out[7] = (uint8_t)((signature >> 24) & 0xFF);
    return 8;
}

static int entryPack(uint8_t id, void *buf, size_t cap)
{
    switch (id) {
        case BKP_E_SETTINGS: return packSettings(buf, cap);
        case BKP_E_VFO:      return packVfo(buf, cap);
        case BKP_E_CHANNELS: return packChannels(buf, cap);
        case BKP_E_CONTACTS: return packContacts(buf, cap);
        case BKP_E_MESSAGES: return packMessages(buf, cap);
        case BKP_E_ZONES:    return packZones(buf, cap);
        default:             return -ENODEV;
    }
}

/*
 * Applying one store. Every one of these goes through the store's own write
 * path, so the read-back check of the store is the last line of defence: the
 * backup's CRC says the record is the one that was written, the store's says
 * the bytes reached the flash.
 */

static int applySettings(const uint8_t *buf, size_t len)
{
    settings_t s;
    uint8_t cause = 0;

    if (settings_recordLoad(buf, len, &s, &cause) != 0)
        return BKP_ERR_STORE;

    if (nvm_writeSettings(&s) != 0)
        return BKP_ERR_IO;

    state.settings = s;
    state.settings_reset = SETTINGS_NO_RESET;

    /* Same clamps as state_init(): a record is data, not a command */
    if (state.settings.brightness > 100)
        state.settings.brightness = 100;
    if (!settings_txPowerValid(state.settings.tx_power))
        state.settings.tx_power = default_settings.tx_power;

    /*
     * And the callsign: a backup is a record like any other, and this one was
     * never validated (the keypad entry and the PC protocol are).  A field with
     * no terminator in its 10 bytes makes every consumer of the M17 addresses
     * read past them, so it becomes N0CALL here, before it reaches the flash.
     */
    if (!settings_callsignFieldValid(state.settings.callsign)) {
        memcpy(state.settings.callsign, default_settings.callsign,
               SETTINGS_CALLSIGN_LEN);
        if (nvm_writeSettings(&state.settings) != 0)
            return BKP_ERR_IO;
    }

    return BKP_OK;
}

static int applyVfo(const uint8_t *buf, size_t len)
{
    if (len < sizeof(channel_t))
        return BKP_ERR_STORE;

    channel_t ch;
    memcpy(&ch, buf, sizeof(ch));

    /*
     * A VFO is always a receiver: OPMODE_NONE is what "no mode" is, and
     * restoring it left the radio on MAIN_VFO with a frequency, a name and
     * switchMode(OPMODE_NONE) as its only receiver -- a screen that looks
     * normal over a radio that is deaf.  The power has the PA's own ceiling as
     * its limit (0 is the operator's "do not transmit" and is kept).
     */
    if (ch.rx_frequency == 0 || ch.mode == OPMODE_NONE || ch.mode > OPMODE_WFM_BC)
        return BKP_ERR_STORE;

    if (!settings_txPowerValid(ch.power))
        return BKP_ERR_STORE;

    if (ch.tx_frequency == 0)
        ch.tx_frequency = ch.rx_frequency;

    if (nvm_writeSettingsAndVfo(NULL, &ch) != 0)
        return BKP_ERR_IO;

    state.channel = ch;
    return BKP_OK;
}

static int applyChannels(const uint8_t *buf, size_t len)
{
    return (chstore_import(buf, len) == 0) ? BKP_OK : BKP_ERR_STORE;
}

static int applyContacts(const uint8_t *buf, size_t len)
{
    return (cstore_import(buf, len) == 0) ? BKP_OK : BKP_ERR_STORE;
}

static int applyMessages(const uint8_t *buf, size_t len)
{
    if (messaging_deserialise(buf, len) < 0)
        return BKP_ERR_STORE;

    /* RAM and flash have to agree at once: a power cut after a restore must
     * not bring back the messages the restore replaced. */
    if (!messaging_store_write(buf, len))
        return BKP_ERR_IO;

    return BKP_OK;
}

static int applyZones(const uint8_t *buf, size_t len)
{
    if (len < 8)
        return BKP_ERR_STORE;

    uint32_t signature = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                         ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);

    if (signature != zones_signature())
        return BKP_ERR_STORE;

    zones_setShiftInverted(buf[1] != 0);
    return BKP_OK;
}

static int entryApply(uint8_t id, const uint8_t *buf, size_t len)
{
    switch (id) {
        case BKP_E_SETTINGS: return applySettings(buf, len);
        case BKP_E_VFO:      return applyVfo(buf, len);
        case BKP_E_CHANNELS: return applyChannels(buf, len);
        case BKP_E_CONTACTS: return applyContacts(buf, len);
        case BKP_E_MESSAGES: return applyMessages(buf, len);
        case BKP_E_ZONES:    return applyZones(buf, len);
        default:             return BKP_ERR_STORE;
    }
}

static const char *entryName(uint8_t id)
{
    switch (id) {
        case BKP_E_SETTINGS: return "settings";
        case BKP_E_VFO:      return "vfo";
        case BKP_E_CHANNELS: return "channels";
        case BKP_E_CONTACTS: return "contacts";
        case BKP_E_MESSAGES: return "messages";
        case BKP_E_ZONES:    return "zones";
        default:             return "?";
    }
}

/* ------------------------------------------------------------- the API --- */

int bkp_inspect(bkpInfo_t *out)
{
    if (out == NULL)
        return BKP_ERR_PARAM;

    memset(out, 0, sizeof(*out));

    const bkpPort_t *p = getPort();
    if (p == NULL)
        return BKP_ERR_NO_PORT;

    bkpHeader_t h;
    uint8_t area = 0;
    int rc = findNewest(p, &h, &area);

    if (rc != BKP_OK)
        return rc;

    out->valid = true;
    out->version = h.version;
    out->sequence = h.sequence;
    out->size = h.totalSize;
    out->when = h.when;
    out->area = area;
    out->count = h.count;

    for (uint16_t i = 0; i < h.count; i++)
        out->lengths[h.dir[i].id] = h.dir[i].length;

    return BKP_OK;
}

int bkp_backup(void)
{
    const bkpPort_t *p = getPort();

    if (p == NULL) {
        snprintf(lastResult, sizeof(lastResult), "%s",
                 bkp_resultText(BKP_ERR_NO_PORT));
        return BKP_ERR_NO_PORT;
    }

    bkpHeader_t prev;
    uint8_t prevArea = 0;
    int have = findNewest(p, &prev, &prevArea);

    bkpHeader_t h;
    memset(&h, 0, sizeof(h));
    h.magic = BKP_MAGIC;
    h.version = BKP_VERSION;
    h.hdrSize = sizeof(bkpHeader_t);
    h.sequence = (have == BKP_OK) ? (prev.sequence + 1) : 1U;
    if (!bkp_platformTime(&h.when))
        memset(&h.when, 0, sizeof(h.when));

    /* Sizes first: the header carries the directory the payload follows */
    uint32_t off = (uint32_t)sizeof(bkpHeader_t);
    uint16_t count = 0;

    for (uint8_t id = 0; id < BKP_E_COUNT; id++) {
        int n = entrySize(id);
        uint32_t stored = ((uint32_t)n + BKP_ALIGN - 1U) & ~(BKP_ALIGN - 1U);

        if (n < 0 || stored > BKP_AREA_SIZE - off) {
            logLine("backup: %s does not fit (%d bytes)", entryName(id), n);
            snprintf(lastResult, sizeof(lastResult), "%s",
                     bkp_resultText(BKP_ERR_TOO_BIG));
            return BKP_ERR_TOO_BIG;
        }

        h.dir[count].id = id;
        h.dir[count].flags = 0;
        h.dir[count].length = (uint16_t)n;
        h.dir[count].offset = off;
        count++;
        off += stored;
    }

    h.count = count;
    h.totalSize = off;

    /* Always the other area: a save never costs the previous record */
    uint8_t area = (have == BKP_OK) ? (uint8_t)(prevArea ^ 1U) : 0;

    if (p->erase(p->ctx, (uint32_t)area * BKP_AREA_SIZE, BKP_AREA_SIZE) != 0) {
        logLine("backup: erase of area %u failed", (unsigned)area);
        snprintf(lastResult, sizeof(lastResult), "%s",
                 bkp_resultText(BKP_ERR_IO));
        return BKP_ERR_IO;
    }

    uint16_t crc = headerCrc(&h, 0);

    for (uint16_t i = 0; i < count; i++) {
        const bkpDirEntry_t *e = &h.dir[i];
        int n = entryPack(e->id, scratch, sizeof(scratch));

        if (n != (int)e->length) {
            /* The store changed between the two passes: refuse, and leave the
             * area without a header so the previous record stays valid */
            logLine("backup: %s changed while packing", entryName(e->id));
            snprintf(lastResult, sizeof(lastResult), "%s",
                     bkp_resultText(BKP_ERR_STORE));
            return BKP_ERR_STORE;
        }

        uint32_t stored = ((uint32_t)n + BKP_ALIGN - 1U) & ~(BKP_ALIGN - 1U);

        /* The flash takes writes on the write unit: pad with the erased value */
        if (stored > (uint32_t)n)
            memset(scratch + n, 0xFF, stored - (uint32_t)n);

        if (areaWrite(p, area, e->offset, scratch, stored) != 0) {
            logLine("backup: writing %s failed", entryName(e->id));
            snprintf(lastResult, sizeof(lastResult), "%s",
                     bkp_resultText(BKP_ERR_IO));
            return BKP_ERR_IO;
        }

        crc = crc_ccitt_update(crc, scratch, stored);
    }

    /* The header goes last: until it is there, the area holds nothing */
    h.crc = crc;

    if (areaWrite(p, area, 0, &h, sizeof(h)) != 0) {
        logLine("backup: writing the header failed");
        snprintf(lastResult, sizeof(lastResult), "%s",
                 bkp_resultText(BKP_ERR_IO));
        return BKP_ERR_IO;
    }

    int rc = verifyRecord(p, area, &h);
    if (rc != BKP_OK) {
        logLine("backup: read-back check failed (%s)", bkp_resultText(rc));
        snprintf(lastResult, sizeof(lastResult), "%s", bkp_resultText(rc));
        return rc;
    }

    logLine("backup: %u bytes, record %lu, area %u, %u stores",
            (unsigned)h.totalSize, (unsigned long)h.sequence,
            (unsigned)area, (unsigned)count);
    snprintf(lastResult, sizeof(lastResult), "BACKUP OK %u B",
             (unsigned)h.totalSize);
    bb_mark(BB_M_BACKUP);

    return BKP_OK;
}

int bkp_restore(void)
{
    const bkpPort_t *p = getPort();

    if (p == NULL) {
        snprintf(lastResult, sizeof(lastResult), "%s",
                 bkp_resultText(BKP_ERR_NO_PORT));
        return BKP_ERR_NO_PORT;
    }

    /*
     * Everything that can be checked without writing is checked first: the
     * magic, the version, the directory and the CRC of the whole record, and
     * the zone plan of the record against the one this build has.
     */
    bkpHeader_t h;
    uint8_t area = 0;
    int rc = findNewest(p, &h, &area);

    if (rc != BKP_OK) {
        logLine("restore: refused, %s", bkp_resultText(rc));
        snprintf(lastResult, sizeof(lastResult), "%s", bkp_resultText(rc));
        return rc;
    }

    for (uint16_t i = 0; i < h.count; i++) {
        const bkpDirEntry_t *e = &h.dir[i];

        if (e->id != BKP_E_ZONES)
            continue;

        if (e->length != 8 ||
            areaRead(p, area, e->offset, scratch, 8) != 0) {
            rc = BKP_ERR_STORE;
            break;
        }

        uint32_t signature = (uint32_t)scratch[4] |
                             ((uint32_t)scratch[5] << 8) |
                             ((uint32_t)scratch[6] << 16) |
                             ((uint32_t)scratch[7] << 24);

        if (signature != zones_signature()) {
            logLine("restore: refused, the zone plan of the backup is "
                    "another one");
            rc = BKP_ERR_STORE;
            break;
        }
    }

    if (rc != BKP_OK) {
        snprintf(lastResult, sizeof(lastResult), "%s", bkp_resultText(rc));
        return rc;
    }

    /* From here on the stores are written, one at a time, each one verified by
     * its own flash path. A failure is reported and the rest is still tried:
     * a half restored radio with a log that says which store failed is better
     * than one that stops without a word. */
    uint16_t applied = 0;
    uint16_t failed = 0;

    for (uint16_t i = 0; i < h.count; i++) {
        const bkpDirEntry_t *e = &h.dir[i];
        uint32_t stored = ((uint32_t)e->length + BKP_ALIGN - 1U) &
                          ~(BKP_ALIGN - 1U);

        if (stored > sizeof(scratch) ||
            areaRead(p, area, e->offset, scratch, stored) != 0) {
            logLine("restore: reading %s failed", entryName(e->id));
            failed++;
            rc = BKP_ERR_IO;
            continue;
        }

        int r = entryApply(e->id, scratch, e->length);

        if (r != BKP_OK) {
            /*
             * The result and the store's own error code: "WRITE FAILED" alone
             * does not say whether the store refused the record or the flash
             * refused the write, and on the host the code is the driver's
             * errno (see platform/drivers/NVM/nvmem_linux.c).
             */
            logLine("restore: %s refused (%s, rc %d)", entryName(e->id),
                    bkp_resultText(r), r);
            failed++;
            rc = r;
            continue;
        }

        applied++;
        logLine("restore: %s, %u bytes", entryName(e->id),
                (unsigned)e->length);
    }

    if (failed > 0) {
        snprintf(lastResult, sizeof(lastResult), "%s %u/%u",
                 bkp_resultText(rc), (unsigned)applied,
                 (unsigned)(applied + failed));
        return rc;
    }

    logLine("restore: %u stores of record %lu (area %u)", (unsigned)applied,
            (unsigned long)h.sequence, (unsigned)area);
    snprintf(lastResult, sizeof(lastResult), "RESTORE OK %u/%u",
             (unsigned)applied, (unsigned)h.count);
    bb_mark(BB_M_RESTORE);

    return BKP_OK;
}
