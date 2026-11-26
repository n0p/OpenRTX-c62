/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DISPLAY_FLUSH_H
#define DISPLAY_FLUSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file display_flush.h
 * @brief Panel transfers with a deadline: a frame that cannot be sent is
 *        dropped, never waited for.
 *
 * The screen is drawn from the user interface thread, and on this board the
 * flush ends in a wait for a DMA transfer to complete.  That wait, as the
 * driver stack ships it, has no end: `display_write()` -> `spi_transceive()`
 * -> `spi_context_wait()` -> `k_sem_take(&ctx->sync, K_FOREVER)`.  When a
 * transfer does not complete -- a DMA channel that never runs, an interrupt
 * that never fires -- the UI thread parks there for ever and the radio looks
 * alive while the keyboard and the screen are dead.  That is exactly the
 * airband freeze in docs/RX_AIRBAND.md.
 *
 * This module puts a deadline in front of every transfer of a frame.  Each
 * transfer is submitted asynchronously and waited for with a timeout; the
 * first one that misses its deadline abandons the frame: the transfer is
 * released, the frame is counted as dropped (docs/BLACKBOX.md, `diag`) and the
 * caller continues with the next keypress.  A frozen panel is a fault, a
 * frozen user interface is a dead radio; this trades the first for the second.
 *
 * It is portable C with no Zephyr in it, so the host tests drive it with a
 * fake bus that never completes and check that the caller still returns.
 */

/** Default deadline of one panel transfer, milliseconds. */
#ifndef DISP_FLUSH_TIMEOUT_MS
#define DISP_FLUSH_TIMEOUT_MS 100
#endif

/** One transfer of a frame: a command byte or a block of pixel data. */
typedef struct
{
    const void *buf;   /**< bytes to send, valid until the frame returns     */
    size_t      len;   /**< how many                                             */
    bool        isData;/**< DC line: false a command, true data               */
} displayChunk_t;

/**
 * How the frame reaches the panel.  Every hook is called from the thread that
 * draws; none of them may block for longer than the deadline it is given.
 */
typedef struct
{
    /**
     * Start one transfer.  Returns 0 when it is on its way, a negative value
     * when it could not be started at all (no bus, no channel, busy).
     */
    int (*submit)(void *ctx, bool isData, const void *buf, size_t len);

    /**
     * Wait for the transfer submitted last.  Returns 0 when it completed,
     * anything else when it did not: the deadline expired first, or the
     * driver reported the transfer as failed.  The platform is expected to
     * bound this call, not to wait for ever.
     */
    int (*wait)(void *ctx, uint32_t timeoutMs);

    /** Give up on a transfer that did not complete and release the bus. */
    void (*abandon)(void *ctx);

    /** Monotonic milliseconds, for the timings the counters report. */
    uint32_t (*nowMs)(void *ctx);

    void *ctx; /**< passed to every hook                                      */
} displayFlushIo_t;

/** What the screen path has been doing since boot; read by `diag`. */
typedef struct
{
    uint32_t frames;      /**< frames that reached the panel                 */
    uint32_t dropped;     /**< frames abandoned: a transfer did not complete  */
    uint32_t transfers;   /**< individual transfers started                  */
    uint32_t timeouts;    /**< transfers that did not complete (deadline or
                           *   the driver's error)                           */
    uint32_t lastMs;      /**< duration of the last complete frame, ms       */
    uint32_t lastDropMs;  /**< duration of the last dropped frame, ms        */
    uint32_t maxMs;       /**< longest complete frame since boot, ms         */
    uint16_t failLen;     /**< bytes of the transfer that was lost           */
    uint8_t  failChunk;   /**< its position in the frame, 0 = first          */
    bool     failData;    /**< it was pixel data, not a command              */
} displayFlushStats_t;

/**
 * Send one frame, one transfer at a time, each bounded by @p timeoutMs.
 *
 * On the first transfer that cannot be started or that misses its deadline,
 * the frame is abandoned: no further transfer is attempted, the bus is
 * released and the frame is counted in @ref displayFlushStats_t.dropped.
 *
 * @param io: hooks into the panel bus.
 * @param chunks: transfers, in wire order.
 * @param count: how many.
 * @param timeoutMs: deadline of each individual transfer.
 * @return true when the whole frame went out, false when it was dropped.
 */
bool display_flush_frame(const displayFlushIo_t *io,
                         const displayChunk_t *chunks, size_t count,
                         uint32_t timeoutMs);

/** Counters since boot.  Never NULL. */
const displayFlushStats_t *display_flush_stats(void);

/** Forget the counters.  For the tests and for a bench session. */
void display_flush_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_FLUSH_H */
