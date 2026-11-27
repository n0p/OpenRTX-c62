/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include "interfaces/nvmem.h"
#include "calibration/calib_C62.h"
#include "core/crc.h"
#include "core/settings_record.h"

/*
 * Settings and VFO data are kept in separate erase sectors of the storage
 * partition, so that updating one record does not force an erase of the
 * other. Each record carries a magic value and a CRC16 to detect both erased
 * and corrupted storage.
 *
 * A record is not rewritten in place: the sector is a journal of 16 slots of
 * 256 bytes, and each save appends to the next slot that is still erased.  The
 * erase happens once every 16 saves, so a power cut during a save costs that
 * one save (its slot fails its magic or its CRC on the next boot and the
 * previous slot is loaded) instead of the whole record, which is what
 * "erase the sector then write" did on a radio whose power switch is the
 * battery: docs/SETTINGS.md §1 has the slot map.  The slot index is the
 * sequence number -- the newest usable slot is the one to load -- so neither
 * record needs a field for it and both keep their on-flash layout (the
 * settings record keeps its own magic/CRC/version header, the VFO record the
 * nvmRecord one below).
 *
 * A save also reads its slot back and compares it: a write the driver reported
 * as done is not proof that the bytes are there, and the caller keeps its
 * debounce key only when the record is really stored.
 */
#define SECTOR_SIZE 0x1000
#define SETTINGS_ADDR 0x0000
#define VFO_ADDR (SETTINGS_ADDR + SECTOR_SIZE)
#define NVM_MAGIC 0x4F525458U /* "ORTX" */
#define NVM_RECORD_MAX 256U
#define NVM_WRITE_ALIGN 4U

/* One slot per record copy, and the sector is nothing but slots */
#define NVM_SLOT_SIZE NVM_RECORD_MAX
#define NVM_SLOTS (SECTOR_SIZE / NVM_SLOT_SIZE)
#define NVM_ERASED 0xFFFFFFFFU

struct nvmRecord {
    uint32_t magic;
    uint16_t crc;
    uint16_t pad;
};

static const struct flash_area *storage;
static uint16_t settingsCrc;
static uint16_t vfoCrc;

/**
 * Address of one slot of a record's journal.
 *
 * @param off: record offset inside the storage partition (its sector).
 * @param slot: slot index, below NVM_SLOTS.
 * @return absolute offset inside the storage partition.
 */
static uint32_t slotAddr(uint32_t off, unsigned slot)
{
    return off + slot * NVM_SLOT_SIZE;
}

/**
 * Read from the stock firmware's settings journal partition.
 *
 * @param ctx: flash area of the journal.
 * @param off: offset in the partition.
 * @param buf: destination buffer.
 * @param len: number of bytes to read.
 * @return zero on success, a negative error code otherwise.
 */
static int journalRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    return flash_area_read((const struct flash_area *)ctx, off, buf, len);
}

/**
 * The slot a save goes to: the first one whose header is still erased.
 *
 * A slot whose magic is neither the record's nor all-ones is a write that was
 * cut in half, and it is skipped: the flash can only be programmed from 1 to
 * 0, so a half-written word cannot be rewritten in place reliably.  When every
 * slot is used, the sector is erased and the journal starts again at slot 0 --
 * which is the one moment a save is as dangerous as it used to be for all of
 * them.
 *
 * @param off: record offset inside the storage partition.
 * @return the slot index, or -1 on a flash error.
 */
static int nextSlot(uint32_t off)
{
    for (unsigned slot = 0; slot < NVM_SLOTS; slot++) {
        uint32_t magic;

        if (flash_area_read(storage, slotAddr(off, slot), &magic,
                            sizeof(magic)) < 0)
            return -1;

        if (magic == NVM_ERASED)
            return (int)slot;
    }

    if (flash_area_erase(storage, off, SECTOR_SIZE) < 0)
        return -1;

    return 0;
}

/**
 * Read a span back and compare it with what was meant to be written.
 *
 * @param addr: address inside the storage partition.
 * @param expect: bytes as written.
 * @param len: number of bytes to compare.
 * @return zero when the flash holds exactly those bytes.
 */
static int verifyWrite(uint32_t addr, const void *expect, size_t len)
{
    const uint8_t *want = (const uint8_t *)expect;
    uint8_t chunk[64];

    for (size_t off = 0; off < len; off += sizeof(chunk)) {
        size_t n = len - off;

        if (n > sizeof(chunk))
            n = sizeof(chunk);

        if (flash_area_read(storage, addr + off, chunk, n) < 0)
            return -1;

        if (memcmp(chunk, want + off, n) != 0)
            return -1;
    }

    return 0;
}

/**
 * Read a record from the journal: the newest slot whose magic and CRC check
 * out, or -1 when no slot holds a usable one.
 *
 * @param off: record offset inside the storage partition.
 * @param data: destination buffer for the record payload.
 * @param len: payload size in bytes.
 * @return zero on success, a negative error code otherwise.
 */
static int readRecord(uint32_t off, void *data, size_t len)
{
    struct nvmRecord rec;
    uint8_t buf[NVM_RECORD_MAX];
    size_t padded = (len + NVM_WRITE_ALIGN - 1U) & ~(NVM_WRITE_ALIGN - 1U);

    if ((storage == NULL) || (padded > sizeof(buf)))
        return -1;

    for (int slot = (int)NVM_SLOTS - 1; slot >= 0; slot--) {
        const uint32_t addr = slotAddr(off, (unsigned)slot);

        if (flash_area_read(storage, addr, &rec, sizeof(rec)) < 0)
            continue;

        if (rec.magic != NVM_MAGIC)
            continue;

        if (flash_area_read(storage, addr + sizeof(rec), buf, padded) < 0)
            continue;

        if (rec.crc != crc_ccitt(buf, len))
            continue;

        memcpy(data, buf, len);
        return 0;
    }

    return -1;
}

/**
 * Append a record to the journal: the next free slot, then a read-back.
 * Payloads are padded to the flash write block size, as the underlying flash
 * driver rejects unaligned writes.
 *
 * @param off: record offset inside the storage partition.
 * @param data: payload to be written.
 * @param len: payload size in bytes.
 * @return zero on success, a negative error code otherwise.
 */
static int writeRecord(uint32_t off, const void *data, size_t len)
{
    struct nvmRecord rec = { .magic = NVM_MAGIC, .crc = crc_ccitt(data, len) };
    uint8_t buf[NVM_RECORD_MAX];
    size_t padded = (len + NVM_WRITE_ALIGN - 1U) & ~(NVM_WRITE_ALIGN - 1U);

    if ((storage == NULL) || (padded > sizeof(buf)))
        return -1;

    memcpy(buf, data, len);
    memset(buf + len, 0xFF, padded - len);

    const int slot = nextSlot(off);
    if (slot < 0)
        return -1;

    const uint32_t addr = slotAddr(off, (unsigned)slot);

    if (flash_area_write(storage, addr, &rec, sizeof(rec)) < 0)
        return -1;

    if (flash_area_write(storage, addr + sizeof(rec), buf, padded) < 0)
        return -1;

    /* Both halves, because a slot that reads back as a valid record is the
     * only thing the next boot will accept. */
    if (verifyWrite(addr, &rec, sizeof(rec)) < 0)
        return -1;

    return verifyWrite(addr + sizeof(rec), buf, padded);
}

void nvm_init()
{
    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &storage) < 0)
        storage = NULL;
}

/**
 * Read one slot of a block that carries no magic/CRC framing of its own.
 *
 * Settings are that kind of block: core/settings_record.h puts magic, CRC and
 * version inside the payload, because the version has to be checked before the
 * payload is interpreted, and one framing is enough.  The slot is the journal's
 * sequence number, so what decides between two slots is the record itself --
 * nvm_readSettings() asks settings_recordLoad() about each one, newest first.
 */
static int readBlobSlot(uint32_t off, unsigned slot, void *buf, size_t len)
{
    if (storage == NULL)
        return -1;

    return flash_area_read(storage, slotAddr(off, slot), buf, len);
}

/** Append a block to its journal, padded to the flash write unit, and verify. */
static int writeBlob(uint32_t off, const void *data, size_t len)
{
    uint8_t buf[NVM_RECORD_MAX];
    size_t padded = (len + NVM_WRITE_ALIGN - 1U) & ~(NVM_WRITE_ALIGN - 1U);

    if ((storage == NULL) || (padded > sizeof(buf)))
        return -1;

    memcpy(buf, data, len);
    memset(buf + len, 0xFF, padded - len);

    const int slot = nextSlot(off);
    if (slot < 0)
        return -1;

    const uint32_t addr = slotAddr(off, (unsigned)slot);

    if (flash_area_write(storage, addr, buf, padded) < 0)
        return -1;

    return verifyWrite(addr, buf, padded);
}

void nvm_terminate()
{
    if (storage != NULL) {
        flash_area_close(storage);
        storage = NULL;
    }
}

void nvm_readCalibData(void *buf)
{
    struct C62Calib *cal = (struct C62Calib *)buf;
    const struct flash_area *journal;
    int ret = -1;

    if (flash_area_open(FIXED_PARTITION_ID(factory_cal), &journal) == 0) {
        ret = calibC62_read(journalRead, (void *)journal, journal->fa_size,
                            cal);
        flash_area_close(journal);
    }

    if (ret < 0) {
        calibC62_defaults(cal);
        printk("TX calibration: stock defaults, no valid factory data\n");
    } else {
        printk("TX calibration: factory data, %u VHF and %u UHF points\n",
               cal->vhf.count, cal->uhf.count);
    }
}

void nvm_readHwInfo(hwInfo_t *info)
{
    (void)info;
}

int nvm_readVfoChannelData(channel_t *channel)
{
    if (channel == NULL)
        return -1;

    memset(channel, 0x00, sizeof(channel_t));

    if (readRecord(VFO_ADDR, channel, sizeof(channel_t)) < 0)
        return -1;

    vfoCrc = crc_ccitt(channel, sizeof(channel_t));

    return 0;
}

int nvm_readSettings(settings_t *settings)
{
    uint8_t buf[SETTINGS_RECORD_SIZE];
    uint8_t cause = SETTINGS_RESET_BLANK;
    bool causeSet = false;
    bool coerced = false;
    int ret = -1;

    if (settings == NULL)
        return -1;

    memset(settings, 0x00, sizeof(settings_t));
    memset(buf, 0xFF, sizeof(buf));

    /*
     * Newest slot first, and the first one settings_recordLoad() accepts is
     * the record: a save that was cut in half fails its own magic or CRC on
     * the next boot and the slot before it comes back with the last settings
     * the operator actually saved.  The reason reported is the newest slot's
     * (the one the radio tried to boot from), not the oldest slot's.
     */
    for (int slot = (int)NVM_SLOTS - 1; slot >= 0; slot--) {
        uint32_t magic = NVM_ERASED;
        uint8_t slotCause = SETTINGS_RESET_BLANK;

        if (readBlobSlot(SETTINGS_ADDR, (unsigned)slot, buf, sizeof(buf)) < 0)
            continue;

        /* An erased slot was never a record and has nothing to report */
        memcpy(&magic, buf, sizeof(magic));
        if (magic == NVM_ERASED)
            continue;

        /*
         * The version lives in the record header, so an old settings_t is
         * dropped as a whole (see core/settings_record.h) and the reason
         * reaches the screen through state.settings_reset.
         */
        ret = settings_recordLoad(buf, sizeof(buf), settings, &slotCause);

        /* The newest slot's reason is the one worth reporting */
        if (!causeSet) {
            cause = slotCause;
            causeSet = true;
        }

        if (ret == 0)
            break;
    }

    if (ret != 0)
        return -(int)cause;

    /*
     * A record written by version 7 is a prefix of this one and is migrated
     * rather than dropped (settings_record.c): the operator keeps his settings
     * and the appended field comes back at its default.  Say it on the log, so
     * that a bench session can tell a migrated radio from a reset one.
     */
    if (settings_recordVersion(buf, sizeof(buf)) != (uint16_t)SETTINGS_VERSION) {
        printk("settings: record v%u migrated to v%u, defaults for the new "
               "fields\n", (unsigned)settings_recordVersion(buf, sizeof(buf)),
               (unsigned)SETTINGS_VERSION);
    }

    /*
     * Auto is not a working mode on this board: with the pad handed to the UART
     * the accessory switch does not key, and a radio that does not transmit is
     * the one failure nobody diagnoses.  A record saved with Auto -- or a fresh
     * one from an older default -- comes back as Never, and the log says so, so
     * that the operator finds out here instead of in front of a dead PTT.
     * Selecting Auto in the menu is still possible: the value that reaches the
     * record is what this line rewrites on the next boot.
     */
    if ((ret == 0) && (settings->pc_port == (uint8_t)PORT_MODE_AUTO)) {
        settings->pc_port = (uint8_t)PORT_MODE_NEVER;
        printk("settings: accessory port was Auto, brought back as Never "
               "(the switch does not key with the pad on the UART)\n");
        /*
         * The rewritten value has to reach the flash, not only RAM.  The debounce
         * key is the CRC of the record as it sits on flash, so spoiling it makes
         * the next writeSettingsAndVfo() store the corrected record: the state
         * task writes three seconds after any change, and this one counts.
         */
        coerced = true;
    }

    /* Debounce key: the record as it is on flash, not the parsed structure */
    settingsCrc = crc_ccitt(buf, sizeof(buf));

    if (coerced)
        settingsCrc = (uint16_t)~settingsCrc;

    if (ret != 0)
        return -(int)cause;

    return 0;
}

int nvm_writeSettings(const settings_t *settings)
{
    return nvm_writeSettingsAndVfo(settings, NULL);
}

int nvm_writeSettingsAndVfo(const settings_t *settings, const channel_t *vfo)
{
    if (settings != NULL) {
        uint8_t buf[SETTINGS_RECORD_SIZE];
        int n = settings_recordPack(buf, sizeof(buf), settings);

        if (n < 0)
            return -1;

        uint16_t crc = crc_ccitt(buf, (size_t)n);

        if (crc != settingsCrc) {
            if (writeBlob(SETTINGS_ADDR, buf, (size_t)n) < 0)
                return -1;

            settingsCrc = crc;
        }
    }

    if (vfo != NULL) {
        uint16_t crc = crc_ccitt(vfo, sizeof(channel_t));

        if (crc != vfoCrc) {
            if (writeRecord(VFO_ADDR, vfo, sizeof(channel_t)) < 0)
                return -1;

            vfoCrc = crc;
        }
    }

    return 0;
}
