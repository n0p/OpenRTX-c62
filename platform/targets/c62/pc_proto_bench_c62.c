/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The radio side of the bench namespace on the C62 (core/pc_proto_bench.c,
 * docs/BENCH.md).  Built only with C62_BENCH_PROTO, which in turn needs
 * C62_PC_PROTO: it is a bench instrument, not something a radio in a pocket
 * carries.
 *
 * How each operation reaches the radio, and why that way:
 *
 *  - TUNE writes state.channel and raises ui_rtxSyncRequest, which is the same
 *    door the scanner and the VFO search use.  The rtx thread turns that into
 *    an rtxStatus and the mode driver programs the chip, so a bench retune goes
 *    through exactly the code path an operator's retune goes through.  Driving
 *    the BK4819 behind the rtx thread's back would measure a radio nobody has.
 *  - PTT presses the same button the operator presses, through
 *    platform_pttOverride(): the mode's own transmit path keys the PA, with its
 *    own audio route, its own guard and its own way out.  Nothing here calls
 *    radio_enableTx() directly, so a bench transmission cannot outlive the
 *    mode that started it.
 *  - Two things are then done *after* the PA is up, on the next poll, because
 *    the driver would overwrite them while keying: the microphone is muted
 *    (REG_7D gain 0, so an unmodulated carrier is really unmodulated) and, when
 *    the client asked for one, the deviation is scaled by writing REG_40 --
 *    the same register the mode wrote from the factory calibration, so the
 *    starting point is the calibrated deviation and not a guess.
 *  - The tone is the chip's own CTCSS generator, addressed through the normal
 *    channel fields, so it reaches the FM modulator the way an operator's
 *    sub-tone would.  Its range is the standard 67.0-254.1 Hz table: a tone
 *    the table does not have is refused (0 comes back) rather than silently
 *    rounded to something else.  The SDR is what measures the deviation it
 *    produces.
 *
 *  - The transmit power is set by TUNE (PC_TAG_BENCH_POWER, milliwatts) and
 *    lands in state.channel.power, which is the field the rtx thread copies
 *    into the configuration the driver keys with.  On a VFO that field starts
 *    at 0, so before this a bench run keyed the PA with no power behind it and
 *    measured nothing, with the answer claiming a transmission (docs/BENCH.md).
 *    A keying at 0 mW is refused here and by the core, never performed.
 *
 * Nothing here measures the carrier: the radio has no reference for that, and
 * PC_TAG_BENCH_DELTA stays 0.
 *
 * Locking: every state_mutex acquisition here is bounded (state_tryLock).  The
 * transport thread answers the client, and it must not park because the UI
 * holds the lock across ui_updateFSM(): a busy radio answers PC_ERR_BUSY and
 * the client retries, instead of the whole protocol going silent
 * (docs/PC_PROTO.md).
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "core/openrtx.h"
#include "core/pc_proto.h"
#include "core/state.h"
#include "core/cps.h"
#include "core/ui.h"
#include "rtx/rtx.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include <hwconfig.h>
#include "drivers/baseband/BK4819.h"

#ifdef CONFIG_C62_BENCH_PROTO

/* Registers the bench touches once the PA is up, with their field masks. */
#define REG_AF_MIC_GAIN 0x7D
#define REG_MIC_GAIN_MASK 0x001F
#define REG_TX_DEVIATION 0x40
#define REG_DEV_TUNING_MASK 0x0FFF
#define REG_DEV_ENABLE 0x1000

/* The CTCSS table is in tenths of Hz; a tone that matches an entry within 1 %
 * is programmed as that entry, anything else is refused. */
#define TONE_TOLERANCE_TENTHS 10

/** How long a bench handler will wait for state_mutex before answering BUSY. */
#define BENCH_LOCK_MS 20

/** Return value for "the radio was busy": the core turns it into PC_ERR_BUSY. */
#define BENCH_BUSY (-(int)PC_ERR_BUSY)

/** What the bench left programmed, so it can be put back. */
static struct
{
    bool     toneOn;
    uint16_t toneHz;        /**< Hz, as programmed                        */
    uint8_t  level;         /**< deviation percentage in force            */
    uint8_t  pending;       /**< deviation percentage to land on TX       */
    uint16_t micGain;       /**< REG_7D before the bench muted it         */
    bool     micMuted;
    bool     toneSaved;
    uint8_t  savedToneEn;
    uint8_t  savedToneIdx;
    bool     ptt;
    /** A press waiting for the answer to be written: -1 none, 0 up, 1 down */
    int8_t   pttPending;
    /** The operator's channel, kept from the first TUNE or PTT of a session:
     *  the bench rewrites frequency, mode, power and rx_only in state.channel,
     *  and without this a receive-only channel stayed transmittable after the
     *  session and the bench frequency became the stored VFO. */
    channel_t savedChannel;
    bool     channelSaved;
} bs;

/** Remember the operator's channel, once per session (state_mutex held). */
static void benchSaveChannelLocked(void)
{
    if (!bs.channelSaved) {
        bs.savedChannel = state.channel;
        bs.channelSaved = true;
    }
}

/** Remember the operator's channel, once per session.  Takes the lock. */
static bool benchSaveChannel(void)
{
    if (bs.channelSaved)
        return true;
    if (!state_tryLock(BENCH_LOCK_MS))
        return false;
    benchSaveChannelLocked();
    state_unlock();
    return true;
}

/** Index of the CTCSS entry nearest to `hz`, or -1 when none is close. */
static int nearestCtcss(uint16_t hz)
{
    uint32_t want = (uint32_t)hz * 10u;
    int best = -1;
    uint32_t bestDiff = 0;

    for (int i = 0; i < CTCSS_FREQ_NUM; i++) {
        uint32_t f = ctcss_tone[i];
        uint32_t diff = f > want ? f - want : want - f;
        if (best < 0 || diff < bestDiff) {
            best = i;
            bestDiff = diff;
        }
    }
    if (best < 0)
        return -1;
    if (bestDiff > (uint32_t)ctcss_tone[best] * TONE_TOLERANCE_TENTHS / 100u +
                       TONE_TOLERANCE_TENTHS)
        return -1;
    return best;
}

/** Mute the microphone and, when asked, scale the deviation.  Runs on the
 *  first poll that sees the PA up, because the driver wrote both registers
 *  while keying. */
static void benchLandTxSettings(void)
{
    if (!bs.micMuted) {
        /* Saved once per key-up: the driver wrote this register while keying,
         * so what is read here is the mode's own microphone gain. */
        bs.micGain = bk4819_readReg(&c62_bk4819, REG_AF_MIC_GAIN);
        bk4819_maskSetRegister(&c62_bk4819, REG_AF_MIC_GAIN, REG_MIC_GAIN_MASK,
                               0);
        bs.micMuted = true;
    }

    if (bs.pending != 0) {
        uint16_t reg = bk4819_readReg(&c62_bk4819, REG_TX_DEVIATION);
        uint32_t tuning = reg & REG_DEV_TUNING_MASK;
        uint32_t scaled = tuning * bs.pending / 100u;
        if (scaled > REG_DEV_TUNING_MASK)
            scaled = REG_DEV_TUNING_MASK;
        bk4819_writeReg(&c62_bk4819, REG_TX_DEVIATION,
                        (uint16_t)((reg & ~REG_DEV_TUNING_MASK) |
                                   (scaled & REG_DEV_TUNING_MASK) | REG_DEV_ENABLE));
        bs.level = bs.pending;
        bs.pending = 0;
    }
}

/** Put back everything a key-up changed. */
static void benchRestore(void)
{
    platform_pttOverride(-1);
    bs.ptt = false;

    if (bs.micMuted) {
        bk4819_maskSetRegister(&c62_bk4819, REG_AF_MIC_GAIN, REG_MIC_GAIN_MASK,
                               bs.micGain & REG_MIC_GAIN_MASK);
        bs.micMuted = false;
    }
    bs.pending = 0;
    bs.level = 0;

    if (bs.toneOn) {
        if (state_tryLock(BENCH_LOCK_MS)) {
            if (bs.toneSaved) {
                state.channel.fm.txToneEn = bs.savedToneEn;
                state.channel.fm.txTone = bs.savedToneIdx;
            }
            state_unlock();
            ui_rtxSyncRequest = true;
        }
        /* A lock that could not be taken leaves the tone in the channel: the
         * next RELEASE or the session reset puts it back.  Tearing the PA down
         * is what this function is for and that already happened above. */
    }

    bs.toneOn = false;
    bs.toneHz = 0;
    bs.toneSaved = false;

    /*
     * And the operator's channel comes back: the run rewrote its frequency,
     * mode, power and rx_only (TUNE), and a receive-only channel that stays
     * transmittable after the client has gone away is the one thing a bench
     * session must not leave behind.  A lock that cannot be taken leaves the
     * flag set, so the next RELEASE or the session reset tries again.
     */
    if (bs.channelSaved && state_tryLock(BENCH_LOCK_MS)) {
        state.channel = bs.savedChannel;
        state_unlock();
        bs.channelSaved = false;
        ui_rtxSyncRequest = true;
    }
}

/**
 * Apply a bench press that was armed by BENCH_PTT.
 *
 * Called from the transport loop after the dispatch, so the answer to the frame
 * that asked for the keying is already out.  Nothing else lives here: the
 * unkey is immediate (it only drops the PA, there is nothing to protect).
 */
void pc_proto_platformTick(uint32_t nowMs)
{
    (void)nowMs;

    if (bs.pttPending < 0)
        return;

    const int8_t want = bs.pttPending;

    bs.pttPending = -1;
    platform_pttOverride(want != 0 ? 1 : -1);
    if (want == 0)
        bs.ptt = false;
}

int pc_proto_platformBench(uint8_t op, pcProtoBenchArgs_t *a)
{
    if (a == NULL)
        return -1;

    switch (op) {
        case PC_BENCH_OP_POLL: {
            rtxStatus_t s = rtx_getCurrentStatus();
            a->nowMs = (uint32_t)k_uptime_get();
            a->txState = (uint8_t)s.opStatus;
            a->rxHz = s.rxFrequency;
            a->txHz = s.txFrequency;
            a->mode = (uint8_t)s.opMode;
            a->bw = (uint8_t)s.bandwidth;
            a->deltaHz = 0; /* no reference on this radio: the SDR measures */
            a->rssi = (int16_t)rtx_getRssi();
            uint16_t raw = bk4819_readReg(&c62_bk4819, 0x67);
            a->rssiRaw = (int16_t)(((raw & 0x01FF) / 2) - 160);
            a->ptt = bs.ptt ? 1 : 0;
            a->toneHz = bs.toneOn ? bs.toneHz : 0;
            a->level = bs.level;
            a->power = (uint16_t)s.txPower;

            if (s.opStatus == TX)
                benchLandTxSettings();
            return 0;
        }

        case PC_BENCH_OP_TUNE: {
            if (a->rxHz == 0)
                return -1;
            if (a->power > SETTINGS_TX_POWER_MAX)
                return -1;
            /*
             * A mode outside the table would be written into the channel and
             * reach the driver as an opmode it does not know; the bench is a
             * measurement instrument, not a way to invent a mode.
             */
            if (a->mode > OPMODE_WFM_BC)
                return -1;
            if (!state_tryLock(BENCH_LOCK_MS))
                return BENCH_BUSY;
            benchSaveChannelLocked();
            state.channel.mode = a->mode != 0 ? a->mode : state.channel.mode;
            /* The wire carries bw + 1 so that 0 can mean "keep"; 1..3 map onto
             * the rtx bandwidth enum. */
            if (a->bw >= 1 && a->bw <= 3)
                state.channel.bandwidth = (uint8_t)(a->bw - 1);
            state.channel.rx_frequency = a->rxHz;
            state.channel.tx_frequency = a->txHz != 0 ? a->txHz : a->rxHz;
            state.channel.rx_only = false; /* a bench run keys the PA */
            /*
             * The power the run asked for.  It lives in the channel because
             * that is the one field the rtx thread copies into the
             * configuration the driver keys with; 0 keeps what is in force, so
             * a client that does not send the tag inherits the radio's own
             * power (and a radio with none is refused at PTT).
             */
            if (a->power != 0)
                state.channel.power = (uint16_t)a->power;

            /*
             * The power the run will key with, written where the rtx thread
             * reads it: the channel's own, or the setting when the channel has
             * none (a VFO).  A resulting 0 is kept as 0 -- it is the refusal
             * PTT answers with, and it must not become a silent minimum.
             */
            if (state.channel.power == 0)
                state.channel.power = state.settings.tx_power;
            a->power = state.channel.power;

            a->mode = state.channel.mode;
            a->bw = (uint8_t)(state.channel.bandwidth + 1);
            a->rxHz = state.channel.rx_frequency;
            a->txHz = state.channel.tx_frequency;
            state_unlock();
            ui_rtxSyncRequest = true;
            return 0;
        }

        case PC_BENCH_OP_PTT: {
            if (a->ptt == 0) {
                bs.pttPending = -1;
                benchRestore();
                a->ptt = 0;
                a->toneHz = 0;
                a->level = 0;
                a->txState = (uint8_t)rtx_getCurrentStatus().opStatus;
                return 0;
            }

            /*
             * No power, no keying.  The core refuses this before the call, and
             * it is refused here too: the PA enable lines going up with 0 mW
             * behind them is exactly the silent failure this bench namespace
             * exists to make impossible (docs/TX_SAFETY.md).  The answer
             * carries the power that was in force, so the client can say what
             * it asked for and what the radio had.
             */
            a->power = (uint16_t)state_txPowerMw();
            if (a->power == 0)
                return -1;

            /* The channel the release has to put back (see benchRestore()) */
            if (!benchSaveChannel())
                return BENCH_BUSY;

            /* Tone first: it has to be in the channel fields before the rtx
             * thread builds a new configuration, or the keying would use the
             * previous sub-tone. */
            if (a->toneHz != 0) {
                int idx = nearestCtcss(a->toneHz);
                if (idx < 0) {
                    a->toneHz = 0; /* refused, and said so */
                } else if (!state_tryLock(BENCH_LOCK_MS)) {
                    return BENCH_BUSY;
                } else {
                    if (!bs.toneOn) {
                        bs.savedToneEn = state.channel.fm.txToneEn;
                        bs.savedToneIdx = state.channel.fm.txTone;
                        bs.toneSaved = true;
                    }
                    state.channel.fm.txToneEn = 1;
                    state.channel.fm.txTone = (uint8_t)idx;
                    state_unlock();
                    ui_rtxSyncRequest = true;
                    bs.toneOn = true;
                    bs.toneHz = (uint16_t)(ctcss_tone[idx] / 10u);
                }
            }

            if (a->toneHz == 0 && bs.toneOn) {
                /* Keying without a tone after keying with one: put the
                 * operator's sub-tone back before the PA goes up. */
                if (state_tryLock(BENCH_LOCK_MS)) {
                    if (bs.toneSaved) {
                        state.channel.fm.txToneEn = bs.savedToneEn;
                        state.channel.fm.txTone = bs.savedToneIdx;
                    }
                    state_unlock();
                    ui_rtxSyncRequest = true;
                }
                bs.toneOn = false;
                bs.toneHz = 0;
                bs.toneSaved = false;
            }

            /*
             * The deviation scaling lands on the first poll with the PA up.
             *
             * The press itself is applied by pc_proto_platformTick(), after
             * this answer has been written.  Keying here would hand the radio
             * to the RTX thread -- and to its transmit path -- while the answer
             * to this very frame was still being formatted, so a radio whose
             * transmit path stops would take the answer with it, which is what
             * the bench measured: BENCH_PTT timeouts with no fault to show for
             * them (references/bench-measurement.md).
             */
            bs.pending = a->level;
            bs.ptt = true;
            bs.pttPending = 1;

            a->txState = (uint8_t)rtx_getCurrentStatus().opStatus;
            a->toneHz = bs.toneHz;
            a->level = bs.level; /* 0 until the scaling lands on the PA */
            /*
             * The power this run will key with, which is the number the client
             * compares its measurement against -- not the rtx status, which can
             * still read 0 for a few tens of milliseconds after a TUNE or
             * before the first configuration.  Answering 0 there made the core
             * refuse a keying that the platform had already armed
             * (bs.pttPending below), and the radio then transmitted while the
             * client believed the order had been rejected.
             */
            a->power = (uint16_t)state_txPowerMw();
            return 0;
        }

        case PC_BENCH_OP_RSSI: {
            a->nowMs = (uint32_t)k_uptime_get();
            a->rssi = (int16_t)rtx_getRssi();
            uint16_t raw = bk4819_readReg(&c62_bk4819, 0x67);
            a->rssiRaw = (int16_t)(((raw & 0x01FF) / 2) - 160);
            return 0;
        }

        case PC_BENCH_OP_RELEASE:
            benchRestore();
            return 0;

        default:
            return -1;
    }
}

#endif /* CONFIG_C62_BENCH_PROTO */
