/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file mode_stats_off.c
 * @brief A probe for the disabled layer: every hook, with the option off.
 *
 * Compiled without CONFIG_C62_MODE_STATS by tests/mode_stats_off.sh, which
 * checks that the object references nothing of the layer and that the
 * function below is nothing but a return. That is what "costs nothing when
 * disabled" has to mean, and this is how it is verified.
 */

#include "core/mode_stats.h"

void probe(enum modeStatsMode mode, const int16_t *samples, size_t count,
           uint32_t nowMs)
{
    modeStats_blockStart(mode);
    modeStats_block(mode, samples, count);
    modeStats_frames(mode, count);
    modeStats_valid(mode, count);
    modeStats_badCrc(mode, count);
    modeStats_lost(mode, count);
    modeStats_resync(mode);
    modeStats_rssi(mode, 42);
    modeStats_error(mode, count);
    modeStats_timing(mode, (int32_t)count, 125);
    modeStats_txKey(mode, nowMs);
    modeStats_txBlock(mode, nowMs);
    modeStats_txStarve(mode);
    modeStats_txEnd(mode);
}
