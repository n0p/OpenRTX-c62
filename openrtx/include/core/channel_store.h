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

/** Version of the record format described in this header. */
#define CHSTORE_VERSION 1

/** Delay before an updated "last used" index is written back to flash, ms. */
#define CHSTORE_LASTUSED_DELAY_MS 3000

/** Frequency limits: BK4819 coverage, see docs/CHANNELS.md. */
#define CHSTORE_FREQ_MIN     18000000U    /*  18 MHz */
#define CHSTORE_FREQ_MAX_LO 660000000U    /* 660 MHz */
#define CHSTORE_FREQ_MIN_HI 840000000U    /* 840 MHz */
#define CHSTORE_FREQ_MAX   1300000000U    /* 1300 MHz */

/** Channel flags. */
enum chstoreFlags
{
    CHSTORE_FLAG_RX_ONLY = 0x01, /**< TX disabled on this channel */
    CHSTORE_FLAG_SIMPLEX = 0x02  /**< TX frequency follows RX */
};

/**
 * A channel as stored on the radio. 32 bytes, chosen so that a full bank of
 * CHSTORE_MAX_CHANNELS channels fits in a single 4 KiB sector.
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
}
__attribute__((packed)) chstore_channel_t;

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
 * supported range, mode known, power non zero. A channel that fails this is
 * never written.
 */
bool chstore_valid(const chstore_channel_t *ch);

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
 */
void chstore_fromChannel(const channel_t *in, chstore_channel_t *out);

/**
 * Short mode name for the display ("FM", "M17", "DMR", "TETRA").
 */
const char *chstore_modeName(uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_STORE_H */
