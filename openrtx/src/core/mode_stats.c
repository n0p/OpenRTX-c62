/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file mode_stats.c
 * @brief Per-mode counters, quality figures and demodulation cost.
 *
 * Everything behind CONFIG_C62_MODE_STATS: without it this translation unit
 * is empty and nothing here is linked. With it on, the API is a set of
 * integer accumulator updates on the calling thread, and the only formatted
 * output is produced on demand by modeStats_format(), from the remote control
 * thread, never from the audio or the receiver path.
 */

#include "core/mode_stats.h"

#ifdef CONFIG_C62_MODE_STATS

#include <stdio.h>
#include <string.h>

/**
 * CPU clock, to turn cycle counts into microseconds. The Zephyr build exposes
 * its configured core clock as a macro; elsewhere (host tests) one cycle is
 * taken to be one microsecond, and the tests say so explicitly.
 */
#ifndef MODE_STATS_CPU_MHZ
#ifdef CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC
#define MODE_STATS_CPU_MHZ (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U)
#else
#define MODE_STATS_CPU_MHZ 1U
#endif
#endif

/**
 * One transmission's blocks arrive at the mode's own cadence: an M17 frame is
 * 40 ms, a DMR burst 60 ms on one timeslot. A block more than two periods
 * after the previous one is a gap, not a jitter.
 */
static const uint8_t txPeriodMs[MS_MODE_COUNT] = { 40, 40, 60, 60 };

static const char *const modeNames[MS_MODE_COUNT] = {
    "fm",
    "m17",
    "dmr",
    "tetra",
};

/** Accumulators of one mode. Written by one thread: the one that owns it. */
struct modeStatsState {
    uint32_t blocks;
    uint32_t frames;
    uint32_t valid;
    uint32_t badCrc;
    uint32_t lost;
    uint32_t resyncs;

    uint64_t levelSum;    /* Sum of the per-block mean level */
    uint32_t levelBlocks; /* Blocks whose level was measured */
    uint32_t levelPeak;

    bool rssiValid;
    int16_t rssiPeak;

    uint64_t errorSum;
    uint32_t errorCount;

    bool timingValid;
    int32_t timingFit;
    int32_t timingStep;

    uint32_t costStart; /* Cycle counter at blockStart */
    bool costPending;
    uint64_t costSum;   /* Microseconds, summed */
    uint32_t costBlocks;
    uint32_t costMinUs;
    uint32_t costMaxUs;

    uint32_t txKeys;
    bool txActive;
    bool txFirstValid;
    uint32_t txFirstMs;
    bool txSeenBlock;
    uint32_t txKeyTime;
    uint32_t txLastBlock;
    uint32_t txStarved;
    uint32_t txGaps;
};

static struct modeStatsState stats[MS_MODE_COUNT];

/** Saturating 32-bit addition: a counter that never wraps. */
static uint32_t addSat(uint32_t value, uint32_t add)
{
    return (value > (UINT32_MAX - add)) ? UINT32_MAX : (value + add);
}

static struct modeStatsState *stateOf(enum modeStatsMode mode)
{
    if ((unsigned)mode >= (unsigned)MS_MODE_COUNT)
        return NULL;
    return &stats[mode];
}

__attribute__((weak)) uint32_t modeStats_platformCycles(void)
{
    return 0;
}

void modeStats_blockStart(enum modeStatsMode mode)
{
    struct modeStatsState *st = stateOf(mode);
    if (st == NULL)
        return;

    st->costStart = modeStats_platformCycles();
    st->costPending = true;
}

void modeStats_block(enum modeStatsMode mode, const int16_t *samples,
                     size_t count)
{
    struct modeStatsState *st = stateOf(mode);
    if (st == NULL)
        return;

    /* Close the cost measurement: the level below is not part of it */
    if (st->costPending) {
        uint32_t elapsed = modeStats_platformCycles() - st->costStart;
        uint32_t us = elapsed / MODE_STATS_CPU_MHZ;

        st->costPending = false;
        if (st->costBlocks == 0)
            st->costMinUs = us;
        else if (us < st->costMinUs)
            st->costMinUs = us;
        if (us > st->costMaxUs)
            st->costMaxUs = us;
        st->costSum += us;
        if (st->costBlocks != UINT32_MAX)
            st->costBlocks++;
    }

    st->blocks = addSat(st->blocks, 1);

    if ((samples == NULL) || (count == 0))
        return;

    uint32_t sum = 0;
    uint32_t peak = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t v = samples[i];
        uint32_t a = (uint32_t)((v < 0) ? -v : v);
        sum += a;
        if (a > peak)
            peak = a;
    }

    uint32_t mean = sum / (uint32_t)count;
    st->levelSum += mean;
    if (st->levelBlocks != UINT32_MAX)
        st->levelBlocks++;
    if (peak > st->levelPeak)
        st->levelPeak = peak;
}

void modeStats_frames(enum modeStatsMode mode, uint32_t count)
{
    struct modeStatsState *st = stateOf(mode);
    if (st != NULL)
        st->frames = addSat(st->frames, count);
}

void modeStats_valid(enum modeStatsMode mode, uint32_t count)
{
    struct modeStatsState *st = stateOf(mode);
    if (st != NULL)
        st->valid = addSat(st->valid, count);
}

void modeStats_badCrc(enum modeStatsMode mode, uint32_t count)
{
    struct modeStatsState *st = stateOf(mode);
    if (st != NULL)
        st->badCrc = addSat(st->badCrc, count);
}

void modeStats_lost(enum modeStatsMode mode, uint32_t count)
{
    struct modeStatsState *st = stateOf(mode);
    if (st != NULL)
        st->lost = addSat(st->lost, count);
}

void modeStats_resync(enum modeStatsMode mode)
{
    struct modeStatsState *st = stateOf(mode);
    if (st != NULL)
        st->resyncs = addSat(st->resyncs, 1);
}

void modeStats_rssi(enum modeStatsMode mode, int dbm)
{
    struct modeStatsState *st = stateOf(mode);
    if (st == NULL)
        return;

    /* dBm are small numbers, but a caller cannot hand us one that is not */
    if (dbm > INT16_MAX)
        dbm = INT16_MAX;
    else if (dbm < INT16_MIN)
        dbm = INT16_MIN;

    if ((!st->rssiValid) || (dbm > st->rssiPeak)) {
        st->rssiPeak = (int16_t)dbm;
        st->rssiValid = true;
    }
}

void modeStats_error(enum modeStatsMode mode, uint32_t thousandths)
{
    struct modeStatsState *st = stateOf(mode);
    if (st == NULL)
        return;

    st->errorSum += thousandths;
    st->errorCount = addSat(st->errorCount, 1);
}

void modeStats_timing(enum modeStatsMode mode, int32_t fit, int32_t step)
{
    struct modeStatsState *st = stateOf(mode);
    if (st == NULL)
        return;

    st->timingFit = fit;
    st->timingStep = step;
    st->timingValid = true;
}

void modeStats_txKey(enum modeStatsMode mode, uint32_t nowMs)
{
    struct modeStatsState *st = stateOf(mode);
    if (st == NULL)
        return;

    st->txKeys = addSat(st->txKeys, 1);
    st->txActive = true;
    st->txSeenBlock = false;
    st->txKeyTime = nowMs;
    st->txLastBlock = nowMs;
}

void modeStats_txBlock(enum modeStatsMode mode, uint32_t nowMs)
{
    struct modeStatsState *st = stateOf(mode);
    if (st == NULL)
        return;

    if (!st->txActive)
        return;

    if (!st->txSeenBlock) {
        st->txFirstMs = nowMs - st->txKeyTime;
        st->txFirstValid = true;
        st->txSeenBlock = true;
    } else if ((nowMs - st->txLastBlock) > (2u * txPeriodMs[mode])) {
        st->txGaps = addSat(st->txGaps, 1);
    }

    st->txLastBlock = nowMs;
}

void modeStats_txStarve(enum modeStatsMode mode)
{
    struct modeStatsState *st = stateOf(mode);
    if (st != NULL)
        st->txStarved = addSat(st->txStarved, 1);
}

void modeStats_txEnd(enum modeStatsMode mode)
{
    struct modeStatsState *st = stateOf(mode);
    if (st != NULL)
        st->txActive = false;
}

void modeStats_reset(enum modeStatsMode mode)
{
    if (mode == MS_MODE_COUNT) {
        memset(stats, 0, sizeof(stats));
        return;
    }

    struct modeStatsState *st = stateOf(mode);
    if (st != NULL)
        memset(st, 0, sizeof(*st));
}

void modeStats_get(enum modeStatsMode mode, struct modeStatsValues *out)
{
    const struct modeStatsState *st = stateOf(mode);
    if ((st == NULL) || (out == NULL))
        return;

    memset(out, 0, sizeof(*out));

    out->blocks = st->blocks;
    out->frames = st->frames;
    out->valid = st->valid;
    out->badCrc = st->badCrc;
    out->lost = st->lost;
    out->resyncs = st->resyncs;

    out->levelMean = (st->levelBlocks > 0) ?
                         (uint32_t)(st->levelSum / st->levelBlocks) :
                         0;
    out->levelPeak = st->levelPeak;
    out->rssiValid = st->rssiValid;
    out->rssiPeak = st->rssiPeak;
    out->errorValid = st->errorCount > 0;
    out->errorMean = (st->errorCount > 0) ?
                         (uint32_t)(st->errorSum / st->errorCount) :
                         0;
    out->timingValid = st->timingValid;
    out->timingFit = st->timingFit;
    out->timingStep = st->timingStep;

    out->costValid = st->costBlocks > 0;
    out->costMinUs = st->costMinUs;
    out->costMaxUs = st->costMaxUs;
    out->costMeanUs = (st->costBlocks > 0) ?
                          (uint32_t)(st->costSum / st->costBlocks) :
                          0;

    out->txKeys = st->txKeys;
    out->txFirstValid = st->txFirstValid;
    out->txFirstMs = st->txFirstMs;
    out->txStarved = st->txStarved;
    out->txGaps = st->txGaps;
}

const char *modeStats_name(enum modeStatsMode mode)
{
    if ((unsigned)mode >= (unsigned)MS_MODE_COUNT)
        return "?";
    return modeNames[mode];
}

int modeStats_format(enum modeStatsMode mode, char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return -1;

    struct modeStatsValues v;
    modeStats_get(mode, &v);

    /*
     * Built in pieces: the fields without a value print a dash, and no
     * floating point is used anywhere (newlib-nano has no %f).
     */
    char rssi[8] = "-";
    if (v.rssiValid)
        snprintf(rssi, sizeof(rssi), "%d", (int)v.rssiPeak);

    char quality[12] = "-";
    if (v.errorValid)
        snprintf(quality, sizeof(quality), "%lu", (unsigned long)v.errorMean);

    char timing[28] = "-/-";
    if (v.timingValid)
        snprintf(timing, sizeof(timing), "%ld/%ld", (long)v.timingFit,
                 (long)v.timingStep);

    char cost[56] = "-/-/-us p=-";
    if (v.costValid) {
        /* Budget: one 30 ms block, as tenths of a percent */
        uint32_t tenths = (uint32_t)((uint64_t)v.costMeanUs * 1000u
                                     / (MODE_STATS_BLOCK_MS * 1000u));
        snprintf(cost, sizeof(cost), "%lu/%lu/%luus p=%lu.%lu%%",
                 (unsigned long)v.costMinUs, (unsigned long)v.costMeanUs,
                 (unsigned long)v.costMaxUs, (unsigned long)(tenths / 10u),
                 (unsigned long)(tenths % 10u));
    }

    char first[16] = "-";
    if (v.txFirstValid)
        snprintf(first, sizeof(first), "%lums", (unsigned long)v.txFirstMs);

    int n = snprintf(buf, len,
                     "rx %s blk=%lu frm=%lu ok=%lu crc=%lu lost=%lu rs=%lu "
                     "lvl=%lu/%lu rssi=%s q=%s fit=%s cost=%s tx=%lu fst=%s "
                     "late=%lu gap=%lu",
                     modeStats_name(mode), (unsigned long)v.blocks,
                     (unsigned long)v.frames, (unsigned long)v.valid,
                     (unsigned long)v.badCrc, (unsigned long)v.lost,
                     (unsigned long)v.resyncs, (unsigned long)v.levelMean,
                     (unsigned long)v.levelPeak, rssi, quality, timing, cost,
                     (unsigned long)v.txKeys, first, (unsigned long)v.txStarved,
                     (unsigned long)v.txGaps);

    if (n < 0)
        return -1;
    if ((size_t)n >= len)
        return -1;

    return n;
}

#endif /* CONFIG_C62_MODE_STATS */
