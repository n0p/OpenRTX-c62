/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Bounded panel transfers: see core/display_flush.h for why this exists and
 * docs/RX_AIRBAND.md for the freeze it was written for.
 *
 * The whole point of this file is what it does *not* do: it never waits for a
 * transfer beyond the deadline it was given, and it never retries one.  A
 * frame either goes out or is counted and forgotten.  There is no lock in here
 * because there is nothing shared to protect: one thread draws, one frame at a
 * time, and the counters are single words that a reader tolerates half written.
 */

#include "core/display_flush.h"

static volatile displayFlushStats_t stats;

bool display_flush_frame(const displayFlushIo_t *io,
                         const displayChunk_t *chunks, size_t count,
                         uint32_t timeoutMs)
{
    if ((io == NULL) || (io->submit == NULL) || (io->wait == NULL)
        || (chunks == NULL) || (count == 0))
        return false;

    const uint32_t t0 = (io->nowMs != NULL) ? io->nowMs(io->ctx) : 0;
    size_t failed = 0;
    bool ok = true;

    for (size_t i = 0; i < count; i++) {
        stats.transfers = stats.transfers + 1;

        if (io->submit(io->ctx, chunks[i].isData, chunks[i].buf,
                       chunks[i].len) != 0) {
            ok = false;
            failed = i;
            break;
        }

        if (io->wait(io->ctx, timeoutMs) != 0) {
            /* The deadline, which is the whole reason this module exists. */
            stats.timeouts = stats.timeouts + 1;
            ok = false;
            failed = i;
            break;
        }
    }

    const uint32_t elapsed = (io->nowMs != NULL)
                           ? (io->nowMs(io->ctx) - t0) : 0;

    if (!ok) {
        /*
         * Release the bus even when the transfer was never started: a start
         * that failed half way is exactly the case that leaves the driver
         * holding its context, and the next frame has to find it free.
         */
        if (io->abandon != NULL)
            io->abandon(io->ctx);

        stats.failLen = (uint16_t)((chunks[failed].len > 0xFFFF)
                                   ? 0xFFFF : chunks[failed].len);
        stats.failChunk = (uint8_t)((failed > 0xFF) ? 0xFF : failed);
        stats.failData = chunks[failed].isData;
        stats.lastDropMs = elapsed;
        stats.dropped = stats.dropped + 1;

        return false;
    }

    stats.lastMs = elapsed;
    if (elapsed > stats.maxMs)
        stats.maxMs = elapsed;
    stats.frames = stats.frames + 1;

    return true;
}

const displayFlushStats_t *display_flush_stats(void)
{
    return &stats;
}

void display_flush_reset(void)
{
    stats.frames = 0;
    stats.dropped = 0;
    stats.transfers = 0;
    stats.timeouts = 0;
    stats.lastMs = 0;
    stats.lastDropMs = 0;
    stats.maxMs = 0;
    stats.failLen = 0;
    stats.failChunk = 0;
    stats.failData = false;
}
