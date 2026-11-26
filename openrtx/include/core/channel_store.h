/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHANNEL_STORE_H
#define CHANNEL_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "core/datatypes.h"
#include "core/cps.h"
#include "core/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Channel bank kept on the radio itself, so that channels can be created,
 * edited and deleted without a CPS.
 *
 * The bank lives in its own flash partition (see docs/CHANNELS.md) and is
 * written with a ping-pong of two slots: a save always goes to the slot that
 * does not hold the current data, and only becomes visible after the header
 * read-back check, so a power cut in the middle of a save costs at most the
 * save itself and never the bank.
 *
 * All frequencies are in Hz, all sizes in bytes.
 */

/** Maximum number of channels in the bank. */
#define CHSTORE_MAX_CHANNELS 128

/** Name field size, including the terminating NUL. */
#define CHSTORE_NAME_LEN 16

/** Value meaning "no channel" where a channel index is expected. */
#define CHSTORE_NONE 0xFFFF

/** Size of the flash area the bank lives in (the OEM channel bank). */
#define CHSTORE_PARTITION_SIZE 0x40000

/** Size of one of the two ping-pong slots: two 4 KiB erase sectors. */
#define CHSTORE_SLOT_SIZE 0x2000

/** Erase sector size assumed by the store. */
#define CHSTORE_SECTOR_SIZE 0x1000

/** Magic value of the record header: "CRTS". */
#define CHSTORE_MAGIC 0x53545243U

/**
 * Version of the record format described in this header.
 *
 *  - 1: 32-byte records, FM M17 and DMR with colour code and timeslot, no DMR
 *       destination, no per-channel mode beyond what channel_t already had.
 *  - 2: 37-byte records: the DMR call destination (type + 24-bit ID) is stored
 *       per channel.  A version 1 bank is migrated on load, see
 *       chstore_channel_v1_t.
 */
#define CHSTORE_VERSION 2

/** Version of the record format this build migrates from. */
#define CHSTORE_VERSION_LEGACY 1

/**
 * Delay before an updated "last used" index is written back to flash, ms.
 * Shorter than the settings/VFO debounce of state_task() so that the two
 * writes do not land in the same pass, and longer than the noise of a knob.
 */
#define CHSTORE_LASTUSED_DELAY_MS 2000

/** Frequency limits: BK4819 coverage, see docs/CHANNELS.md. */
#define CHSTORE_FREQ_MIN     18000000U    /*  18 MHz */
#define CHSTORE_FREQ_MAX_LO 660000000U    /* 660 MHz */
#define CHSTORE_FREQ_MIN_HI 840000000U    /* 840 MHz */
#define CHSTORE_FREQ_MAX   1300000000U    /* 1300 MHz */

/** Channel flags. */
enum chstoreFlags
{
    CHSTORE_FLAG_RX_ONLY = 0x01, /**< TX disabled on this channel */
    CHSTORE_FLAG_SIMPLEX = 0x02, /**< TX frequency follows RX */
    CHSTORE_FLAG_SKIP    = 0x04  /**< Passed over by the memory scan
                                  *   (core/scanner.h): a channel the operator
                                  *   keeps for direct selection but does not
                                  *   want to hear every lap.  The flag is in a
                                  *   byte that already existed, so the record
                                  *   size and the format version do not
                                  *   change. */
};

/**
 * A channel as stored on the radio. 37 bytes, chosen so that a full bank of
 * CHSTORE_MAX_CHANNELS channels still fits in one ping-pong slot (128 * 37 +
 * 32 = 4768 of the 8192 bytes of a slot).
 *
 * This is deliberately not channel_t: that structure carries a 32 byte
 * description, a geolocation and scan/group indices, which no in-radio editor
 * can set and which would not fit the partition.
 */
typedef struct
{
    char     name[CHSTORE_NAME_LEN]; /**< Channel name, NUL terminated      */
    uint32_t rx_freq;                /**< RX frequency, Hz                  */
    uint32_t tx_freq;                /**< TX frequency, Hz                  */
    uint16_t power;                  /**< TX power, mW                      */
    uint8_t  mode;                   /**< Operating mode, enum opmode       */
    uint8_t  bandwidth;              /**< Bandwidth, enum bandwidth         */
    uint8_t  rx_tone_en : 1,         /**< RX CTCSS enabled                  */
             rx_tone    : 7;         /**< RX CTCSS index, enum CTCSSfreq    */
    uint8_t  tx_tone_en : 1,         /**< TX CTCSS enabled                  */
             tx_tone    : 7;         /**< TX CTCSS index, enum CTCSSfreq    */
    uint8_t  dmr_cc : 4,             /**< DMR colour code, 0..15            */
             dmr_ts : 4;             /**< DMR timeslot: 0 = TS1, 1 = TS2    */
    uint8_t  flags;                  /**< enum chstoreFlags                 */
    uint8_t  dmr_dest_type;          /**< enum dmrDestType (DMR mode)       */
    uint32_t dmr_dest_id;            /**< TG or subscriber, 24 bit used     */
}
__attribute__((packed)) chstore_channel_t;

/**
 * The version 1 record, exactly as it was written by the first firmware that
 * had an on-radio bank.  Kept only so that chstore_init() can migrate a bank
 * written by that build: the record grew by the DMR destination and a bank
 * that is refused on upgrade would cost the operator every channel.
 *
 * Do not use it for anything else, and do not add fields to it.
 */
typedef struct
{
    char     name[CHSTORE_NAME_LEN];
    uint32_t rx_freq;
    uint32_t tx_freq;
    uint16_t power;
    uint8_t  mode;
    uint8_t  bandwidth;
    uint8_t  rx_tone_en : 1,
             rx_tone    : 7;
    uint8_t  tx_tone_en : 1,
             tx_tone    : 7;
    uint8_t  dmr_cc : 4,
             dmr_ts : 4;
    uint8_t  flags;
}
__attribute__((packed)) chstore_channel_v1_t;

/**
 * Record header, 32 bytes, followed by count channel records.
 */
typedef struct
{
    uint32_t magic;      /**< CHSTORE_MAGIC                              */
    uint16_t version;    /**< CHSTORE_VERSION                            */
    uint16_t count;      /**< Number of channels                         */
    uint32_t sequence;   /**< Write counter, newest valid slot wins      */
    uint16_t last_used;  /**< Index of the last tuned channel or NONE    */
    uint16_t hcrc;       /**< CRC16 of this header, hcrc and pcrc zeroed */
    uint16_t pcrc;       /**< CRC16 of the channel records               */
    uint8_t  reserved[14]; /**< Zero, reserved for future use            */
}
__attribute__((packed)) chstore_hdr_t;

/**
 * Access to the flash partition holding the bank. The platform provides one
 * implementation (channel_store_c62.c on the C62); host tests give their own.
 * Calls return 0 on success and a negative value on failure.
 */
struct chstorePort
{
    int (*read)(void *ctx, uint32_t offset, void *buf, size_t len);
    int (*erase)(void *ctx, uint32_t offset, size_t len);
    int (*write)(void *ctx, uint32_t offset, const void *buf, size_t len);
    void     *ctx;    /**< Passed to the operations                     */
    uint32_t  size;   /**< Partition size, at least 2 * CHSTORE_SLOT_SIZE */
    uint32_t  sector; /**< Erase unit, must divide CHSTORE_SLOT_SIZE     */
};

/**
 * Platform hook: return the port for this device, or NULL when the device has
 * no channel bank. The weak default in channel_store.c returns NULL, so every
 * target without its own implementation simply reports an empty bank.
 */
const struct chstorePort *chstore_platformPort(void);

/**
 * Bind a port explicitly. Mostly for tests; chstore_init() binds the platform
 * port by itself when nothing has been bound yet.
 */
void chstore_bind(const struct chstorePort *port);

/**
 * Load the bank from flash. Idempotent. On a blank or unreadable partition the
 * bank is empty: the factory codeplug channels are deliberately not imported,
 * the user adds channels from the radio (see chstore_loadDefaults()).
 *
 * @return 0 if the bank is usable (empty or not), a negative errno otherwise.
 */
int chstore_init(void);

/** Number of channels in the bank, 0 if not initialised. */
int chstore_count(void);

/**
 * Copy channel idx out of the bank.
 *
 * @return 0 on success, -EINVAL if idx is out of range or the bank is empty.
 */
int chstore_get(uint16_t idx, chstore_channel_t *out);

/**
 * Validate a channel: name not empty, RX and TX frequencies inside the
 * supported range, mode known, power non zero, tones and DMR fields in range.
 * A channel that fails this is never written.
 *
 * The mode decides what the rest means: FM uses the CTCSS pair, M17 the
 * frequencies and power, DMR adds colour code, timeslot and the call
 * destination (a talkgroup or a subscriber), TETRA is receive only.  Fields
 * that the mode does not use are kept as they are, so switching a channel from
 * DMR to FM and back does not lose the talkgroup.
 */
bool chstore_valid(const chstore_channel_t *ch);

/**
 * True if the DMR call destination of a channel can be used: a known type and,
 * unless the type is NONE, an identifier in 1..16777215.
 */
bool chstore_dmrDestValid(const chstore_channel_t *ch);

/** True if f is inside 18-660 MHz or 840-1300 MHz. */
bool chstore_freqValid(freq_t f);

/**
 * Replace channel idx and write the bank back to flash.
 *
 * @return 0 on success, -EINVAL on a bad index or invalid channel, -EIO if the
 *         flash write failed (the previous bank is still the valid one).
 */
int chstore_set(uint16_t idx, const chstore_channel_t *ch);

/**
 * Append a channel at the end of the bank and write it back.
 *
 * @return the new channel index on success, a negative errno otherwise.
 */
int chstore_add(const chstore_channel_t *ch);

/**
 * Delete channel idx, shifting the following ones down, and write the bank
 * back. The last-used index is cleared if it pointed at the deleted channel.
 *
 * @return 0 on success, -EINVAL on a bad index, -EIO on a failed write.
 */
int chstore_delete(uint16_t idx);

/**
 * Reset the bank to the factory state, that is: empty (the 16 stock channels
 * stay in the OEM partition and are not copied here). Writes the empty bank.
 *
 * @return 0 on success, -EIO on a failed write.
 */
int chstore_loadDefaults(void);

/**
 * True if the channel is flagged SKIP (CHSTORE_FLAG_SKIP): kept in the bank,
 * selected by hand, but passed over by the memory scan.  A bad index is not
 * skipped, so a stale index reports as "would be scanned" rather than silently
 * disappearing.
 */
bool chstore_skipped(uint16_t idx);

/**
 * Set or clear the SKIP flag of channel idx and write the bank back.
 *
 * @return 0 on success, -EINVAL on a bad index, -EIO if the write failed.
 */
int chstore_setSkipped(uint16_t idx, bool skip);

/**
 * Index of the channel that was selected at power-off, or -1 if none.
 */
int chstore_lastUsed(void);
/**
 * Remember the selected channel. This only touches RAM: the value reaches
 * flash from chstore_task() after CHSTORE_LASTUSED_DELAY_MS, or from
 * chstore_commit() when the radio shuts down, so that hopping across channels
 * does not wear the flash out.
 */
void chstore_setLastUsed(uint16_t idx);

/**
 * Write the bank back if anything changed since the last write.
 *
 * @return 0 if the bank is on flash or nothing was pending, negative on error.
 */
int chstore_commit(void);

/**
 * Periodic housekeeping: flush a pending "last used" index after the debounce
 * delay. Called from state_task(), does nothing while transmitting.
 */
void chstore_task(void);

/**
 * Fill a channel_t from channel idx, so that it can be copied into the radio
 * state and pushed to the RTX.
 *
 * @return 0 on success, negative on a bad index.
 */
int chstore_toChannel(uint16_t idx, channel_t *out);

/**
 * Build a stored record from a channel_t, as the VFO or the codeplug keeps it.
 * channel_t has no room for a DMR destination, so the caller that has one (the
 * channel editor, a zone) sets dmr_dest_type/dmr_dest_id afterwards.
 */
void chstore_fromChannel(const channel_t *in, chstore_channel_t *out);

/**
 * Fill an RTX configuration from a stored channel: mode, frequencies (TX
 * follows RX on a simplex channel), power, bandwidth, CTCSS tones and the DMR
 * colour code, timeslot and call destination.  The squelch level and the
 * disable-TX bits belong to the caller, they are not per channel.
 *
 * This is the one place that translates a stored channel into what the
 * transmitter sees, so a channel with a talkgroup arrives at the DMR mode with
 * that talkgroup and not with whatever the last VFO left behind.
 *
 * @return 0 on success, negative on a bad index.
 */
int chstore_applyRtx(uint16_t idx, rtxStatus_t *rtx);

/**
 * Copy the DMR settings a stored channel carries into the settings: the call
 * destination (type and identifier) and the timeslot.  This is what the UI
 * calls when the operator selects a channel, so that the quick menu edits the
 * values of the channel in use, and what keeps one of them from disagreeing
 * with the other: the RTX takes the destination and the slot from the settings
 * and the colour code from the channel.
 *
 * A channel that is not a DMR one leaves the settings as they are.
 *
 * @return 0 on success, -EINVAL on a bad index.
 */
int chstore_applySettings(uint16_t idx, settings_t *settings);

/**
 * Short mode name for the display ("FM", "M17", "DMR", "TETRA").
 */
const char *chstore_modeName(uint8_t mode);

/* ------------------------------------------------------- backup/restore --- */

/**
 * Size of the packed bank chstore_export() writes: two bytes of count plus one
 * record per channel. Worst case (a full bank) is 2 + 128 * 37 = 4738 bytes.
 */
#define CHSTORE_EXPORT_MAX (2U + CHSTORE_MAX_CHANNELS * sizeof(chstore_channel_t))

/**
 * Pack the whole bank for the internal backup (docs/BACKUP.md): a little
 * endian 16 bit count followed by that many records, exactly the payload the
 * store keeps on flash.
 *
 * @param buf: destination.
 * @param cap: capacity, at least CHSTORE_EXPORT_MAX for a full bank.
 * @return the number of bytes written, -ENOSPC if it does not fit.
 */
int chstore_export(void *buf, size_t cap);

/**
 * Replace the whole bank with one written by chstore_export(), and write it to
 * flash through the store's own two-slots path.
 *
 * Every record is validated first, and the bank on flash is left as it was
 * when anything does not check out: a restore that is refused costs nothing.
 *
 * @param buf: the packed bank.
 * @param len: its length, which has to match the count it carries.
 * @return 0 on success, -EINVAL on a malformed or invalid bank, -EIO if the
 *         flash write failed (the previous bank is reloaded and still valid).
 */
int chstore_import(const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_STORE_H */
