/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file mode_stats.cpp
 * @brief Host tests of the per-mode statistics layer (core/mode_stats.h).
 *
 * mode_stats.c is built here with CONFIG_C62_MODE_STATS and
 * MODE_STATS_CPU_MHZ=1, so a cycle is a microsecond and the tests can drive
 * the counter directly: the layer takes everything it measures as an
 * argument, which is what makes it testable without a radio.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

extern "C" {
#include "core/mode_stats.h"

/* Overrides the layer's weak definition, one cycle is one microsecond */
static uint32_t fakeCycles;

uint32_t modeStats_platformCycles(void)
{
    return fakeCycles;
}
}

static void setCycles(uint32_t cycles)
{
    fakeCycles = cycles;
}

static void advanceCycles(uint32_t cycles)
{
    fakeCycles += cycles;
}

static modeStatsValues values(enum modeStatsMode mode)
{
    modeStatsValues v;
    modeStats_get(mode, &v);
    return v;
}

TEST_CASE("Counters add up per mode")
{
    modeStats_reset(MS_MODE_COUNT);

    modeStats_frames(MS_MODE_DMR, 10);
    modeStats_frames(MS_MODE_DMR, 5);
    modeStats_valid(MS_MODE_DMR, 12);
    modeStats_badCrc(MS_MODE_DMR, 3);
    modeStats_lost(MS_MODE_DMR, 2);
    modeStats_resync(MS_MODE_DMR);
    modeStats_resync(MS_MODE_DMR);

    modeStatsValues dmr = values(MS_MODE_DMR);
    REQUIRE(dmr.frames == 15);
    REQUIRE(dmr.valid == 12);
    REQUIRE(dmr.badCrc == 3);
    REQUIRE(dmr.lost == 2);
    REQUIRE(dmr.resyncs == 2);

    /* Another mode is untouched */
    modeStatsValues m17 = values(MS_MODE_M17);
    REQUIRE(m17.frames == 0);
    REQUIRE(m17.resyncs == 0);
}

TEST_CASE("Counters saturate instead of wrapping")
{
    modeStats_reset(MS_MODE_COUNT);

    modeStats_frames(MS_MODE_FM, UINT32_MAX);
    modeStats_frames(MS_MODE_FM, 1000);
    REQUIRE(values(MS_MODE_FM).frames == UINT32_MAX);

    modeStats_valid(MS_MODE_FM, UINT32_MAX - 5);
    modeStats_valid(MS_MODE_FM, 10);
    REQUIRE(values(MS_MODE_FM).valid == UINT32_MAX);

    modeStats_badCrc(MS_MODE_FM, UINT32_MAX);
    modeStats_badCrc(MS_MODE_FM, UINT32_MAX);
    REQUIRE(values(MS_MODE_FM).badCrc == UINT32_MAX);

    modeStats_lost(MS_MODE_FM, UINT32_MAX);
    modeStats_lost(MS_MODE_FM, 1);
    REQUIRE(values(MS_MODE_FM).lost == UINT32_MAX);

    setCycles(0);
    modeStats_blockStart(MS_MODE_FM);
    modeStats_block(MS_MODE_FM, nullptr, 0);
    REQUIRE(values(MS_MODE_FM).blocks == 1);
}

TEST_CASE("Input level: mean of the per-block means, and the peak")
{
    modeStats_reset(MS_MODE_COUNT);

    int16_t block[4] = { 100, -200, 300, -400 };
    setCycles(0);
    modeStats_blockStart(MS_MODE_TETRA);
    modeStats_block(MS_MODE_TETRA, block, 4);

    /* mean |sample| = (100 + 200 + 300 + 400) / 4 = 250, peak 400 */
    modeStatsValues v = values(MS_MODE_TETRA);
    REQUIRE(v.blocks == 1);
    REQUIRE(v.levelMean == 250);
    REQUIRE(v.levelPeak == 400);

    int16_t quiet[2] = { 50, -50 };
    modeStats_blockStart(MS_MODE_TETRA);
    modeStats_block(MS_MODE_TETRA, quiet, 2);

    /* Second block mean 50: the average of 250 and 50 is 150 */
    v = values(MS_MODE_TETRA);
    REQUIRE(v.blocks == 2);
    REQUIRE(v.levelMean == 150);
    REQUIRE(v.levelPeak == 400);

    /* A block without samples counts but does not touch the level */
    modeStats_blockStart(MS_MODE_TETRA);
    modeStats_block(MS_MODE_TETRA, nullptr, 0);
    v = values(MS_MODE_TETRA);
    REQUIRE(v.blocks == 3);
    REQUIRE(v.levelMean == 150);
    REQUIRE(v.levelPeak == 400);
}

TEST_CASE("Demodulation cost: minimum, mean, maximum, over all blocks")
{
    modeStats_reset(MS_MODE_COUNT);

    /* One cycle is one microsecond in this build */
    const uint32_t costs[3] = { 120, 300, 180 };
    for (uint32_t cost : costs) {
        setCycles(1000);
        modeStats_blockStart(MS_MODE_DMR);
        advanceCycles(cost);
        modeStats_block(MS_MODE_DMR, nullptr, 0);
    }

    modeStatsValues v = values(MS_MODE_DMR);
    REQUIRE(v.costValid);
    REQUIRE(v.blocks == 3);
    REQUIRE(v.costMinUs == 120);
    REQUIRE(v.costMaxUs == 300);
    REQUIRE(v.costMeanUs == 200); /* (120 + 300 + 180) / 3 */

    /* A block measured without a blockStart does not add a sample */
    modeStats_block(MS_MODE_DMR, nullptr, 0);
    v = values(MS_MODE_DMR);
    REQUIRE(v.blocks == 4);
    REQUIRE(v.costMeanUs == 200);
}

TEST_CASE("Demodulation cost survives the 32-bit cycle counter wrapping")
{
    modeStats_reset(MS_MODE_COUNT);

    /* Start just below the wrap, end just above it: the difference is small */
    setCycles(0xFFFFFFF0u);
    modeStats_blockStart(MS_MODE_M17);
    setCycles(0x00000064u); /* 0x100000000 - 0xFFFFFFF0 + 0x64 = 0x74 */
    modeStats_block(MS_MODE_M17, nullptr, 0);

    REQUIRE(values(MS_MODE_M17).costMaxUs == 0x74u);
}

TEST_CASE("Quality: the mean of the reported figures")
{
    modeStats_reset(MS_MODE_COUNT);

    REQUIRE_FALSE(values(MS_MODE_DMR).errorValid);

    modeStats_error(MS_MODE_DMR, 100);
    modeStats_error(MS_MODE_DMR, 300);
    modeStatsValues v = values(MS_MODE_DMR);
    REQUIRE(v.errorValid);
    REQUIRE(v.errorMean == 200);

    /* Timing keeps the last fit and its step */
    REQUIRE_FALSE(v.timingValid);
    modeStats_timing(MS_MODE_DMR, -250, 125);
    modeStats_timing(MS_MODE_DMR, 375, 125);
    v = values(MS_MODE_DMR);
    REQUIRE(v.timingValid);
    REQUIRE(v.timingFit == 375);
    REQUIRE(v.timingStep == 125);

    /* RSSI keeps the highest reading, and remembers it was reported */
    REQUIRE_FALSE(v.rssiValid);
    modeStats_rssi(MS_MODE_DMR, -95);
    modeStats_rssi(MS_MODE_DMR, -72);
    modeStats_rssi(MS_MODE_DMR, -80);
    modeStats_rssi(MS_MODE_DMR, -120); /* A worse reading does not replace it */
    v = values(MS_MODE_DMR);
    REQUIRE(v.rssiValid);
    REQUIRE(v.rssiPeak == -72);
}

TEST_CASE("Transmit: keys, first block, starved frames and gaps")
{
    modeStats_reset(MS_MODE_COUNT);

    /* Keyed at 1000 ms, first block out 8 ms later */
    modeStats_txKey(MS_MODE_M17, 1000);
    modeStats_txBlock(MS_MODE_M17, 1008);

    modeStatsValues v = values(MS_MODE_M17);
    REQUIRE(v.txKeys == 1);
    REQUIRE(v.txFirstValid);
    REQUIRE(v.txFirstMs == 8);
    REQUIRE(v.txGaps == 0);

    /* Frames every 40 ms: 1048, 1088 are on time */
    modeStats_txBlock(MS_MODE_M17, 1048);
    modeStats_txBlock(MS_MODE_M17, 1088);
    REQUIRE(values(MS_MODE_M17).txGaps == 0);

    /* 1200 ms is 112 ms later, more than two 40 ms periods: a gap */
    modeStats_txBlock(MS_MODE_M17, 1200);
    REQUIRE(values(MS_MODE_M17).txGaps == 1);

    modeStats_txStarve(MS_MODE_M17);
    modeStats_txStarve(MS_MODE_M17);
    REQUIRE(values(MS_MODE_M17).txStarved == 2);

    /* Blocks outside a transmission are not counted */
    modeStats_txEnd(MS_MODE_M17);
    modeStats_txBlock(MS_MODE_M17, 1500);
    REQUIRE(values(MS_MODE_M17).txGaps == 1);

    /* A second key times its own first block */
    modeStats_txKey(MS_MODE_M17, 2000);
    modeStats_txBlock(MS_MODE_M17, 2012);
    v = values(MS_MODE_M17);
    REQUIRE(v.txKeys == 2);
    REQUIRE(v.txFirstMs == 12);
}

TEST_CASE("Reset: one mode, or all of them")
{
    modeStats_reset(MS_MODE_COUNT);

    modeStats_frames(MS_MODE_FM, 1);
    modeStats_frames(MS_MODE_DMR, 2);
    modeStats_rssi(MS_MODE_FM, -80);

    modeStats_reset(MS_MODE_FM);
    REQUIRE(values(MS_MODE_FM).frames == 0);
    REQUIRE_FALSE(values(MS_MODE_FM).rssiValid);
    REQUIRE(values(MS_MODE_DMR).frames == 2);

    modeStats_reset(MS_MODE_COUNT);
    REQUIRE(values(MS_MODE_DMR).frames == 0);
}

TEST_CASE("A mode that was never used prints dashes, not made-up numbers")
{
    modeStats_reset(MS_MODE_COUNT);

    char line[256];
    int n = modeStats_format(MS_MODE_TETRA, line, sizeof(line));
    REQUIRE(n > 0);
    REQUIRE(n == (int)strlen(line));
    REQUIRE(std::string(line).find("rx tetra blk=0 frm=0 ok=0 crc=0 lost=0 "
                                   "rs=0 lvl=0/0 rssi=- q=- fit=-/- "
                                   "cost=-/-/-us p=- tx=0 fst=- late=0 gap=0") ==
            0);
}

TEST_CASE("The line has a stable format and carries every figure")
{
    modeStats_reset(MS_MODE_COUNT);

    const int16_t block[2] = { 1000, -2000 };
    setCycles(0);
    modeStats_blockStart(MS_MODE_DMR);
    advanceCycles(480);
    modeStats_block(MS_MODE_DMR, block, 2);
    modeStats_frames(MS_MODE_DMR, 12);
    modeStats_valid(MS_MODE_DMR, 10);
    modeStats_badCrc(MS_MODE_DMR, 2);
    modeStats_resync(MS_MODE_DMR);
    modeStats_error(MS_MODE_DMR, 250);
    modeStats_timing(MS_MODE_DMR, 375, 125);
    modeStats_rssi(MS_MODE_DMR, -72);
    modeStats_txKey(MS_MODE_DMR, 5000);
    modeStats_txBlock(MS_MODE_DMR, 5009);

    char line[256];
    int n = modeStats_format(MS_MODE_DMR, line, sizeof(line));
    REQUIRE(n > 0);

    const std::string s(line);
    REQUIRE(s ==
            "rx dmr blk=1 frm=12 ok=10 crc=2 lost=0 rs=1 lvl=1500/2000 "
            "rssi=-72 q=250 fit=375/125 cost=480/480/480us p=1.6% tx=1 "
            "fst=9ms late=0 gap=0");
    (void)n;

    modeStats_txStarve(MS_MODE_DMR);
    modeStats_txBlock(MS_MODE_DMR, 5300); /* 300 ms later: a gap */
    modeStats_format(MS_MODE_DMR, line, sizeof(line));
    REQUIRE(std::string(line).find("late=1 gap=1") != std::string::npos);
}

TEST_CASE("Formatting refuses a buffer it cannot fill")
{
    char line[8];
    REQUIRE(modeStats_format(MS_MODE_FM, line, sizeof(line)) < 0);
    REQUIRE(modeStats_format(MS_MODE_FM, nullptr, 0) < 0);
}

TEST_CASE("Mode names and out-of-range modes")
{
    /*
     * This case asserts that an out-of-range mode counts nothing, so it needs
     * the counters at zero: Catch2 runs the cases in a random order and the
     * saturation case legitimately leaves them at UINT32_MAX.
     */
    modeStats_reset(MS_MODE_COUNT);

    REQUIRE(std::string(modeStats_name(MS_MODE_FM)) == "fm");
    REQUIRE(std::string(modeStats_name(MS_MODE_M17)) == "m17");
    REQUIRE(std::string(modeStats_name(MS_MODE_DMR)) == "dmr");
    REQUIRE(std::string(modeStats_name(MS_MODE_TETRA)) == "tetra");
    REQUIRE(std::string(modeStats_name(MS_MODE_COUNT)) == "?");

    /* Nothing crashes, nothing is counted */
    modeStats_frames(MS_MODE_COUNT, 100);
    REQUIRE(values(MS_MODE_FM).frames == 0);
}
