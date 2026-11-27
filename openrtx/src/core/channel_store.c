/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/channel_store.h"
#include "core/crc.h"
#include "core/settings.h"
#include "core/utils.h"
#include "interfaces/delays.h"
#include "core/psram_state.h"
#include <string.h>
#include <errno.h>

/*
 * Ping-pong layout inside the channel partition:
 *
 *   0x0000  slot A: header (32 B) + CHSTORE_MAX_CHANNELS * 32 B
 *   0x2000  slot B: same
 *   0x4000  free, room for a larger bank or a third slot
 *
 * A save always erases and rewrites the slot that is not holding the current
 * bank, so the previous bank survives a power cut at any point of the write.
 * Sequence numbers decide which of the two slots is the current one.
 */

static const struct chstorePort *port = NULL;

/*
 * The bank is touched by two threads: the state task writes it back from
 * chstore_task() and the UI edits it from the keypad.  The 4 KiB array, the
 * count and the pending flag are not atomic, and bankWrite() computes the
 * payload CRC before it writes the bytes: an edit landing between the two
 * leaves a slot whose CRC does not describe its contents, which the next boot
 * reads as a broken newest slot and replaces with the previous bank -- the
 * operator's change disappears and the screen says "WRITE FAILED" without
 * saying why.  A mutex of the store's own, like the one core/messaging.c uses;
 * on the host build the store is driven from one thread at a time and this
 * compiles away.
 */
#ifdef __ZEPHYR__
#include <zephyr/kernel.h>
static struct k_mutex bankLock;
#define BANK_LOCK()   ((void)k_mutex_lock(&bankLock, K_FOREVER))
#define BANK_UNLOCK() ((void)k_mutex_unlock(&bankLock))
#else
#define BANK_LOCK()   ((void)0)
#define BANK_UNLOCK() ((void)0)
#endif

/* The bank itself: 4 KiB, the RAM cost of the whole feature. */
static chstore_channel_t bank[CHSTORE_MAX_CHANNELS] PSRAM_STATE;

static int commitLocked(void);

static uint16_t nbChannels = 0;
static uint16_t lastUsedIdx = CHSTORE_NONE;
static uint32_t sequence = 0;
static int8_t activeSlot = -1;
static bool ready = false;
static bool pending = false;
static long long pendingSince = 0;

__attribute__((weak)) const struct chstorePort *chstore_platformPort(void)
{
    return NULL;
}

static bool portValid(void)
{
    if (port == NULL || port->read == NULL || port->write == NULL ||
        port->erase == NULL)
        return false;

    if (port->size < 2 * CHSTORE_SLOT_SIZE)
        return false;

    if (port->sector == 0 || (CHSTORE_SLOT_SIZE % port->sector) != 0)
        return false;

    return true;
}

/*
 * Read and validate a slot header. Only the header: the payload CRC is checked
 * when the slot is actually selected, so that a broken newest slot still lets
 * the older one load.
 */
static int slotHeader(uint8_t slot, chstore_hdr_t *hdr)
{
    uint32_t off = (uint32_t)slot * CHSTORE_SLOT_SIZE;
    chstore_hdr_t copy;

    if (port->read(port->ctx, off, hdr, sizeof(*hdr)) != 0)
        return -EIO;

    if (hdr->magic != CHSTORE_MAGIC)
        return -EINVAL;

    /* A bank written by a newer firmware is not touched, not misread */
    if (hdr->version != CHSTORE_VERSION &&
        hdr->version != CHSTORE_VERSION_LEGACY)
        return -EINVAL;

    if (hdr->count > CHSTORE_MAX_CHANNELS)
        return -EINVAL;

    if (hdr->last_used != CHSTORE_NONE && hdr->last_used >= hdr->count)
        return -EINVAL;

    copy = *hdr;
    copy.hcrc = 0;
    copy.pcrc = 0;
    if (crc_ccitt(&copy, sizeof(copy)) != hdr->hcrc)
        return -EINVAL;

    return 0;
}

/* Copy the fields a version 1 record has into a version 2 record. */
static void migrateV1(const chstore_channel_v1_t *old, chstore_channel_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->name, old->name, CHSTORE_NAME_LEN);
    out->rx_freq = old->rx_freq;
    out->tx_freq = old->tx_freq;
    out->power = old->power;
    out->mode = old->mode;
    out->bandwidth = old->bandwidth;
    out->rx_tone_en = old->rx_tone_en;
    out->rx_tone = old->rx_tone;
    out->tx_tone_en = old->tx_tone_en;
    out->tx_tone = old->tx_tone;
    out->dmr_cc = old->dmr_cc;
    out->dmr_ts = old->dmr_ts;
    out->flags = old->flags;

    /*
     * The version 1 build transmitted every DMR call to the talkgroup held in
     * the RTX status, so a migrated channel keeps that behaviour: "group, no
     * identifier of its own" means "use the destination of the moment".  NONE
     * would refuse to transmit, which is not what the operator had.
     */
    out->dmr_dest_type = DMR_DEST_NONE;
    out->dmr_dest_id = 0;
}

/* Read a slot payload into the bank and check its CRC. */
static int slotPayload(uint8_t slot, const chstore_hdr_t *hdr)
{
    uint32_t off = (uint32_t)slot * CHSTORE_SLOT_SIZE + sizeof(chstore_hdr_t);
    uint16_t crc = 0;

    if (hdr->version == CHSTORE_VERSION) {
        size_t len = (size_t)hdr->count * sizeof(chstore_channel_t);

        if (port->read(port->ctx, off, bank, len) != 0)
            return -EIO;

        if (crc_ccitt(bank, len) != hdr->pcrc)
            return -EINVAL;

        return 0;
    }

    /*
     * Legacy bank: read one 32-byte record at a time and convert it in place,
     * so the migration costs 32 bytes of stack and not a second copy of the
     * whole bank.  The CRC covers the bytes as they are on flash.
     */
    for (uint16_t i = 0; i < hdr->count; i++) {
        chstore_channel_v1_t old;

        if (port->read(port->ctx, off + (uint32_t)i * sizeof(old), &old,
                       sizeof(old)) != 0)
            return -EIO;

        crc = crc_ccitt_update(crc, &old, sizeof(old));
        migrateV1(&old, &bank[i]);
    }

    if (crc != hdr->pcrc)
        return -EINVAL;

    return 0;
}

/* Adopt the contents of slot as the current bank. */
static void slotAdopt(uint8_t slot, const chstore_hdr_t *hdr)
{
    nbChannels = hdr->count;
    lastUsedIdx = hdr->last_used;
    sequence = hdr->sequence;
    activeSlot = (int8_t)slot;
}

/*
 * Read back a range and compare it with what was meant to be written.
 *
 * A device can report a successful write and have programmed less than it was
 * given -- a page boundary, a busy chip that dropped the tail -- so the only
 * confirmation worth anything is the bytes that come out of the flash.
 */
static int verifyBack(uint32_t off, const void *data, size_t len)
{
    uint8_t chunk[CHSTORE_VERIFY_CHUNK];
    const uint8_t *want = (const uint8_t *)data;
    size_t done = 0;

    while (done < len) {
        size_t n = len - done;

        if (n > sizeof(chunk))
            n = sizeof(chunk);
        if (port->read(port->ctx, off + (uint32_t)done, chunk, n) != 0)
            return -EIO;
        if (memcmp(chunk, want + done, n) != 0)
            return -EIO;
        done += n;
    }

    return 0;
}

/*
 * Write the bank to the slot that is not active, then re-read the header to
 * confirm what landed. On success that slot becomes the active one; on failure
 * the previous slot is still the newest valid record.
 */
static int bankWrite(void)
{
    uint8_t slot = (activeSlot == 0) ? 1 : 0;
    uint32_t off = (uint32_t)slot * CHSTORE_SLOT_SIZE;
    chstore_hdr_t hdr;
    chstore_hdr_t copy;
    chstore_hdr_t check;
    size_t len = (size_t)nbChannels * sizeof(chstore_channel_t);

    if (!portValid())
        return -ENODEV;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = CHSTORE_MAGIC;
    hdr.version = CHSTORE_VERSION;
    hdr.count = nbChannels;
    hdr.sequence = sequence + 1;
    hdr.last_used = lastUsedIdx;

    copy = hdr;
    copy.hcrc = 0;
    copy.pcrc = 0;
    hdr.hcrc = crc_ccitt(&copy, sizeof(copy));
    hdr.pcrc = crc_ccitt(bank, len);

    if (port->erase(port->ctx, off, CHSTORE_SLOT_SIZE) != 0)
        return -EIO;

    if (port->write(port->ctx, off, &hdr, sizeof(hdr)) != 0)
        return -EIO;

    if (len > 0 && port->write(port->ctx, off + sizeof(hdr), bank, len) != 0)
        return -EIO;

    if (port->read(port->ctx, off, &check, sizeof(check)) != 0)
        return -EIO;

    if (check.magic != CHSTORE_MAGIC || check.sequence != hdr.sequence ||
        check.count != hdr.count || check.hcrc != hdr.hcrc ||
        check.pcrc != hdr.pcrc)
        return -EIO;

    /*
     * The header says what the payload should be; only the payload says
     * whether it is there.  A write that stopped short leaves a header that
     * validates -- it was written first and completely -- over a bank whose
     * CRC does not match it.  Adopting that slot would make it the active one,
     * so the next save would erase the only slot still holding a good bank,
     * and the radio would boot without channels after a second failure.
     */
    if (verifyBack(off + sizeof(hdr), bank, len) != 0)
        return -EIO;

    sequence = hdr.sequence;
    activeSlot = (int8_t)slot;
    pending = false;

    return 0;
}

/*
 * Mark the bank as changed in RAM and write it back at once.
 *
 * The caller holds the bank lock: the CRC bankWrite() computes has to describe
 * the bytes it writes, and only the lock can promise that.
 */
static int bankCommit(void)
{
    pending = true;
    return commitLocked();
}

void chstore_bind(const struct chstorePort *p)
{
    port = p;
    ready = false;
}

static void bankLockInit(void)
{
#ifdef __ZEPHYR__
    static bool ready = false;

    if (!ready) {
        k_mutex_init(&bankLock);
        ready = true;
    }
#endif
}

/* Body of chstore_init(), run by a caller that already holds the lock. */
static int initLocked(void)
{
    chstore_hdr_t ha, hb;
    int va, vb;
    int pick;

    if (ready)
        return 0;

    if (port == NULL)
        port = chstore_platformPort();

    nbChannels = 0;
    lastUsedIdx = CHSTORE_NONE;
    sequence = 0;
    activeSlot = -1;
    pending = false;
    memset(bank, 0, sizeof(bank));
    ready = true;

    /* No media on this device: a usable but empty bank, writes will fail. */
    if (!portValid())
        return 0;

    va = slotHeader(0, &ha);
    vb = slotHeader(1, &hb);

    if (va == 0 && vb == 0)
        pick = ((int32_t)(ha.sequence - hb.sequence) > 0) ? 0 : 1;
    else if (va == 0)
        pick = 0;
    else if (vb == 0)
        pick = 1;
    else
        pick = -1;

    if (pick == 0 && slotPayload(0, &ha) == 0) {
        slotAdopt(0, &ha);
    } else if (pick == 1 && slotPayload(1, &hb) == 0) {
        slotAdopt(1, &hb);
    } else if (pick == 0 && vb == 0 && slotPayload(1, &hb) == 0) {
        /* Newest record written short: the other slot still holds the bank. */
        slotAdopt(1, &hb);
    } else if (pick == 1 && va == 0 && slotPayload(0, &ha) == 0) {
        slotAdopt(0, &ha);
    }

    return 0;
}

int chstore_init(void)
{
    int ret;

    bankLockInit();
    BANK_LOCK();
    ret = initLocked();
    BANK_UNLOCK();

    return ret;
}

int chstore_count(void)
{
    int n;

    BANK_LOCK();
    n = (int)nbChannels;
    BANK_UNLOCK();

    return n;
}

int chstore_get(uint16_t idx, chstore_channel_t *out)
{
    int ret = 0;

    if (out == NULL)
        return -EINVAL;

    BANK_LOCK();
    if (idx >= nbChannels)
        ret = -EINVAL;
    else
        *out = bank[idx];
    BANK_UNLOCK();

    return ret;
}

bool chstore_freqValid(freq_t f)
{
    return (f >= CHSTORE_FREQ_MIN && f <= CHSTORE_FREQ_MAX_LO) ||
           (f >= CHSTORE_FREQ_MIN_HI && f <= CHSTORE_FREQ_MAX);
}

bool chstore_dmrDestValid(const chstore_channel_t *ch)
{
    if (ch == NULL)
        return false;

    return settings_dmrDestValid(ch->dmr_dest_type, ch->dmr_dest_id);
}

bool chstore_valid(const chstore_channel_t *ch)
{
    if (ch == NULL)
        return false;

    if (ch->name[0] == '\0' || memchr(ch->name, '\0', CHSTORE_NAME_LEN) == NULL)
        return false;

    /* A channel with an unusable frequency would only "tune" into a fault. */
    if (!chstore_freqValid(ch->rx_freq) || !chstore_freqValid(ch->tx_freq))
        return false;

    if (ch->mode != OPMODE_FM && ch->mode != OPMODE_M17 &&
        ch->mode != OPMODE_DMR && ch->mode != OPMODE_TETRA &&
        ch->mode != OPMODE_AIRBAND && ch->mode != OPMODE_YSF &&
        ch->mode != OPMODE_WFM_BC && ch->mode != OPMODE_NXDN &&
        ch->mode != OPMODE_P25 && ch->mode != OPMODE_DPMR &&
        ch->mode != OPMODE_DSTAR)
        return false;

    /*
     * 6.25 kHz is a real channel spacing of this radio: the FM and NXDN quick
     * menus offer it (docs/UI.md) and the editor carries it into the record, so
     * refusing it here refused the channel with a message that did not say why
     * -- "INVALID CHANNEL" for a bandwidth the radio itself had just set. The
     * field is a byte, so the record has always been able to carry it.
     */
    if (ch->bandwidth != BW_12_5 && ch->bandwidth != BW_25
        && ch->bandwidth != BW_6_25)
        return false;

    if (ch->power == 0 || ch->power > 5000)
        return false;

    if (ch->rx_tone_en && ch->rx_tone >= CTCSS_FREQ_NUM)
        return false;

    if (ch->tx_tone_en && ch->tx_tone >= CTCSS_FREQ_NUM)
        return false;

    /* Colour code is a nibble: anything above 15 would be truncated on air */
    if (ch->dmr_cc > 15)
        return false;

    if (!chstore_dmrDestValid(ch))
        return false;

    return true;
}

int chstore_set(uint16_t idx, const chstore_channel_t *ch)
{
    if (!chstore_valid(ch))
        return -EINVAL;

    BANK_LOCK();

    if (idx >= nbChannels) {
        BANK_UNLOCK();
        return -EINVAL;
    }

    chstore_channel_t old = bank[idx];
    bank[idx] = *ch;

    if (bankCommit() != 0) {
        /* Keep RAM describing what is on flash, and nothing pending: after the
         * rollback the two are the same bank again, so a retry would only
         * erase and rewrite a slot that already holds it. */
        bank[idx] = old;
        pending = false;
        BANK_UNLOCK();
        return -EIO;
    }

    BANK_UNLOCK();
    return 0;
}

int chstore_add(const chstore_channel_t *ch)
{
    if (!chstore_valid(ch))
        return -EINVAL;

    BANK_LOCK();

    if (nbChannels >= CHSTORE_MAX_CHANNELS) {
        BANK_UNLOCK();
        return -EINVAL;
    }

    bank[nbChannels] = *ch;
    nbChannels++;

    if (bankCommit() != 0) {
        /* Keep RAM and flash consistent: the bank did not grow, and with the
         * record gone from RAM there is nothing left to write. */
        nbChannels--;
        memset(&bank[nbChannels], 0, sizeof(chstore_channel_t));
        pending = false;
        BANK_UNLOCK();
        return -EIO;
    }

    int idx = (int)(nbChannels - 1);

    BANK_UNLOCK();
    return idx;
}

bool chstore_skipped(uint16_t idx)
{
    bool skip = false;

    BANK_LOCK();
    if (idx < nbChannels)
        skip = (bank[idx].flags & CHSTORE_FLAG_SKIP) != 0;
    BANK_UNLOCK();

    return skip;
}

int chstore_setSkipped(uint16_t idx, bool skip)
{
    chstore_channel_t ch;
    int ret = 0;

    BANK_LOCK();
    if (idx >= nbChannels) {
        BANK_UNLOCK();
        return -EINVAL;
    }
    ch = bank[idx];
    BANK_UNLOCK();

    if (skip)
        ch.flags |= (uint8_t)CHSTORE_FLAG_SKIP;
    else
        ch.flags &= (uint8_t)~CHSTORE_FLAG_SKIP;

    ret = chstore_set(idx, &ch);
    return ret;
}

int chstore_delete(uint16_t idx)
{
    BANK_LOCK();

    if (idx >= nbChannels) {
        BANK_UNLOCK();
        return -EINVAL;
    }

    uint16_t oldCount = nbChannels;
    uint16_t oldLast = lastUsedIdx;

    /*
     * The deleted record is kept aside: the shift below overwrites it, so an
     * undo that only moves the tail back one place would replicate the record
     * that took its place (deleting "b" from [a,b,c,d] left [a,c,c,d]) and
     * RAM would then say the opposite of what is on flash.
     */
    chstore_channel_t saved = bank[idx];

    if (idx + 1 < nbChannels)
        memmove(&bank[idx], &bank[idx + 1],
                (size_t)(nbChannels - idx - 1) * sizeof(chstore_channel_t));

    nbChannels--;
    memset(&bank[nbChannels], 0, sizeof(chstore_channel_t));

    if (lastUsedIdx != CHSTORE_NONE) {
        if (lastUsedIdx == idx)
            lastUsedIdx = CHSTORE_NONE;
        else if (lastUsedIdx > idx)
            lastUsedIdx--;
    }

    if (bankCommit() != 0) {
        /*
         * Roll back so that RAM keeps describing the bank on flash.  The
         * deleted record has to go back too: the shift above overwrote it, so
         * moving the tail up alone would leave a copy of the record that took
         * its place (deleting "b" from [a,b,c,d] gave [a,c,c,d]) and RAM would
         * then say the opposite of what is on flash.
         *
         * And with the rollback done there is nothing pending: RAM and flash
         * hold the same bank, so the retry the task would make a second later
         * only erased the slot to write back what was already in it.
         */
        if (idx + 1 < oldCount)
            memmove(&bank[idx + 1], &bank[idx],
                    (size_t)(oldCount - idx - 1) * sizeof(chstore_channel_t));
        bank[idx] = saved;
        nbChannels = oldCount;
        lastUsedIdx = oldLast;
        pending = false;
        BANK_UNLOCK();
        return -EIO;
    }

    BANK_UNLOCK();
    return 0;
}

int chstore_loadDefaults(void)
{
    BANK_LOCK();

    /*
     * The factory codeplug channels are deliberately not copied here: the OEM
     * bank is not imported and the user adds the channels from the radio.
     */
    memset(bank, 0, sizeof(bank));
    nbChannels = 0;
    lastUsedIdx = CHSTORE_NONE;

    if (bankCommit() != 0) {
        BANK_UNLOCK();
        return -EIO;
    }

    BANK_UNLOCK();
    return 0;
}

int chstore_lastUsed(void)
{
    int idx = -1;

    BANK_LOCK();
    if ((lastUsedIdx != CHSTORE_NONE) && (lastUsedIdx < nbChannels))
        idx = (int)lastUsedIdx;
    BANK_UNLOCK();

    return idx;
}

void chstore_setLastUsed(uint16_t idx)
{
    BANK_LOCK();
    if ((idx < nbChannels) && (idx != lastUsedIdx)) {
        lastUsedIdx = idx;
        pending = true;
        pendingSince = getTick();
    }
    BANK_UNLOCK();
}

/* Body of chstore_commit(), run by a caller that already holds the lock. */
static int commitLocked(void)
{
    if (!pending)
        return 0;

    int ret = bankWrite();
    if (ret != 0) {
        /* Keep the request alive: the next task pass or the shutdown retries. */
        pending = true;
    }

    return ret;
}

int chstore_commit(void)
{
    BANK_LOCK();
    int ret = commitLocked();
    BANK_UNLOCK();

    return ret;
}

void chstore_task(void)
{
    BANK_LOCK();
    if (pending && ((getTick() - pendingSince) >= CHSTORE_LASTUSED_DELAY_MS))
        (void)commitLocked();
    BANK_UNLOCK();
}

int chstore_toChannel(uint16_t idx, channel_t *out)
{
    if (out == NULL)
        return -EINVAL;

    BANK_LOCK();

    if (idx >= nbChannels) {
        BANK_UNLOCK();
        return -EINVAL;
    }

    const chstore_channel_t *ch = &bank[idx];

    memset(out, 0, sizeof(channel_t));
    out->mode = ch->mode;
    out->bandwidth = ch->bandwidth;
    out->power = ch->power;
    out->rx_frequency = ch->rx_freq;
    out->tx_frequency = (ch->flags & CHSTORE_FLAG_SIMPLEX) ? ch->rx_freq
                                                           : ch->tx_freq;
    out->rx_only = (ch->flags & CHSTORE_FLAG_RX_ONLY) ? 1 : 0;
    out->scanList_index = 0;
    out->groupList_index = 0;
    strncpy(out->name, ch->name, CPS_STR_SIZE - 1);

    /*
     * fm, dmr and m17 share a union in channel_t: only the block of the mode
     * the channel is on can be filled, the others would overwrite it.
     *
     * The DMR call destination has no field in channel_t: it travels to the
     * radio through chstore_applyRtx(), which is what the UI uses when it
     * selects a stored channel.
     */
    if (out->mode == OPMODE_DMR) {
        out->dmr.rxColorCode = ch->dmr_cc;
        out->dmr.txColorCode = ch->dmr_cc;
        out->dmr.dmr_timeslot = (ch->dmr_ts == 0) ? 1 : 2;
        out->dmr.contact_index = 0;
    } else {
        out->fm.rxToneEn = ch->rx_tone_en;
        out->fm.rxTone = ch->rx_tone;
        out->fm.txToneEn = ch->tx_tone_en;
        out->fm.txTone = ch->tx_tone;
    }

    BANK_UNLOCK();
    return 0;
}

int chstore_applySettings(uint16_t idx, settings_t *settings)
{
    if (settings == NULL)
        return -EINVAL;

    BANK_LOCK();

    if (idx >= nbChannels) {
        BANK_UNLOCK();
        return -EINVAL;
    }

    const chstore_channel_t *ch = &bank[idx];

    if (ch->mode == OPMODE_DMR) {
        settings->dmr_dest_type = ch->dmr_dest_type;
        settings->dmr_dest_id = ch->dmr_dest_id;
        settings->dmr_timeslot = (ch->dmr_ts == 0) ? 1 : 2;
    }

    BANK_UNLOCK();
    return 0;
}

int chstore_applyRtx(uint16_t idx, rtxStatus_t *rtx)
{
    if (rtx == NULL)
        return -EINVAL;

    BANK_LOCK();

    if (idx >= nbChannels) {
        BANK_UNLOCK();
        return -EINVAL;
    }

    const chstore_channel_t *ch = &bank[idx];

    rtx->opMode = ch->mode;
    rtx->rxFrequency = ch->rx_freq;
    rtx->txFrequency = (ch->flags & CHSTORE_FLAG_SIMPLEX) ? ch->rx_freq
                                                          : ch->tx_freq;
    rtx->txPower = ch->power;
    rtx->bandwidth = ch->bandwidth;
    rtx->txDisable = (ch->flags & CHSTORE_FLAG_RX_ONLY) ? 1 : 0;
    /* A receive-only channel closes the unattended gate too: no message may
     * key it either, whatever screen is up. */
    rtx->unattendedTxDisable = rtx->txDisable;

    rtx->rxToneEn = ch->rx_tone_en;
    rtx->rxTone = ch->rx_tone_en ? ctcssFreqFromIndex(ch->rx_tone) : 0;
    rtx->txToneEn = ch->tx_tone_en;
    rtx->txTone = ch->tx_tone_en ? ctcssFreqFromIndex(ch->tx_tone) : 0;

    if (ch->mode == OPMODE_DMR) {
        rtx->dmrColourCode = ch->dmr_cc;
        rtx->dmrTimeslot = (ch->dmr_ts == 0) ? 1 : 2;
        rtx->dmrDestType = ch->dmr_dest_type;
        rtx->dmrDestId = ch->dmr_dest_id;
    }

    BANK_UNLOCK();
    return 0;
}

void chstore_fromChannel(const channel_t *in, chstore_channel_t *out)
{
    if (in == NULL || out == NULL)
        return;

    memset(out, 0, sizeof(chstore_channel_t));
    strncpy(out->name, in->name, CHSTORE_NAME_LEN - 1);
    out->rx_freq = in->rx_frequency;
    out->tx_freq = in->tx_frequency;
    out->power = (uint16_t)in->power;
    out->mode = in->mode;
    out->bandwidth = in->bandwidth;
    out->rx_tone_en = in->fm.rxToneEn;
    out->rx_tone = in->fm.rxTone;
    out->tx_tone_en = in->fm.txToneEn;
    out->tx_tone = in->fm.txTone;
    out->dmr_cc = in->dmr.rxColorCode & 0x0F;
    out->dmr_ts = (in->dmr.dmr_timeslot == 2) ? 1 : 0;
    out->flags = 0;

    if (in->rx_only)
        out->flags |= CHSTORE_FLAG_RX_ONLY;

    if (in->rx_frequency == in->tx_frequency)
        out->flags |= CHSTORE_FLAG_SIMPLEX;
}

/**
 * Short name of a mode, for the screen, the channel list and the header.
 *
 * This is the one place a mode is spelled: the registry (ui/ui_modes.h), the
 * channel editor and the main screen's label all read it, so a mode cannot be
 * called one thing in the list and another on the air.
 *
 * Two to five characters, because the header has room for five (see
 * UI_MODE_LABEL_MAX_W); a mode this build does not have keeps its name here
 * anyway, so a channel record that carries it still reads correctly.
 */
const char *chstore_modeName(uint8_t mode)
{
    switch (mode)
    {
        case OPMODE_FM:      return "FM";
        case OPMODE_M17:     return "M17";
        case OPMODE_DMR:     return "DMR";
        case OPMODE_TETRA:   return "TETRA";
        case OPMODE_NXDN:    return "NXDN";
        case OPMODE_DPMR:    return "dPMR";
        case OPMODE_P25:     return "P25";
        case OPMODE_YSF:     return "YSF";
        case OPMODE_DSTAR:   return "DSTAR";
        case OPMODE_AIRBAND: return "AM";
        case OPMODE_WFM_BC:  return "WFM";
        default:             return "----";
    }
}

int chstore_export(void *buf, size_t cap)
{
    size_t len;

    if (!ready)
        chstore_init();

    if (buf == NULL)
        return -EINVAL;

    BANK_LOCK();

    len = 2U + (size_t)nbChannels * sizeof(chstore_channel_t);
    if (len > cap) {
        BANK_UNLOCK();
        return -ENOSPC;
    }

    uint8_t *out = (uint8_t *)buf;
    out[0] = (uint8_t)(nbChannels & 0xFF);
    out[1] = (uint8_t)(nbChannels >> 8);

    if (nbChannels > 0)
        memcpy(out + 2, bank, (size_t)nbChannels * sizeof(chstore_channel_t));

    BANK_UNLOCK();
    return (int)len;
}

int chstore_import(const void *buf, size_t len)
{
    if (!ready)
        chstore_init();

    if (buf == NULL || len < 2)
        return -EINVAL;

    const uint8_t *in = (const uint8_t *)buf;
    uint16_t count = (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
    size_t want = 2U + (size_t)count * sizeof(chstore_channel_t);

    if (count > CHSTORE_MAX_CHANNELS || len != want)
        return -EINVAL;

    /* The records are packed, so the cast is aligned by construction */
    const chstore_channel_t *recs =
        (const chstore_channel_t *)(const void *)(in + 2);

    BANK_LOCK();

    /* Validate every record before the bank in RAM is touched */
    for (uint16_t i = 0; i < count; i++) {
        if (!chstore_valid(&recs[i])) {
            BANK_UNLOCK();
            return -EINVAL;
        }
    }

    memset(bank, 0, sizeof(bank));
    if (count > 0)
        memcpy(bank, recs, (size_t)count * sizeof(chstore_channel_t));
    nbChannels = count;
    if (lastUsedIdx >= nbChannels)
        lastUsedIdx = CHSTORE_NONE;
    pending = true;

    if (commitLocked() != 0) {
        /*
         * The write failed, so the slot that held the previous bank is still
         * the newest valid one: reload from it. Re-reading (instead of undoing
         * the copy) also clears `pending`, so the retry of chstore_task() does
         * not write the restored bank later and call it a success.
         */
        ready = false;
        (void)initLocked();
        BANK_UNLOCK();
        return -EIO;
    }

    BANK_UNLOCK();
    return 0;
}
