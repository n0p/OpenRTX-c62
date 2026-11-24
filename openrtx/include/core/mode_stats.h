/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MODE_STATS_H
#define MODE_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file mode_stats.h
 * @brief Per-mode receive and transmit instrumentation, for the C62.
 *
 * One table per operating mode with the same fields for all of them: how many
 * blocks, frames and bursts went through, which ones were good, what the input
 * looked like and what demodulating them cost. The point is that a question
 * asked of the radio ("is this mode slow, or is it deaf?") is answered by the
 * same line whichever mode is running, instead of by a different log per mode.
 *
 * Compiled out unless CONFIG_C62_MODE_STATS is defined (CMake option
 * C62_MODE_STATS, off by default): the macros below then expand to nothing,
 * no symbol is referenced from a mode and the firmware image is the one the
 * build would produce without this file. With the option on, a hook costs a
 * few integer adds on the calling thread: no printf, no allocation, no locks,
 * no work outside the block that is being measured. Nothing is transmitted
 * anywhere until the remote control asks for a summary with "rx".
 *
 * Units are fixed, they are the ones the remote control prints:
 *   levels    samples, 0 to MODE_STATS_FULL_SCALE (full scale)
 *   quality   thousandths (EVM, phase error, ...), 0 is perfect
 *   timing    thousandths of a symbol
 *   cost      microseconds of CPU per block of baseband
 *   TX times  milliseconds
 *
 * Hook points, in the order a mode calls them:
 *   modeStats_blockStart()  before demodulating a block of samples
 *   modeStats_block()       after it, with the samples that went in
 *   modeStats_frames()      per frame/burst/slot taken out of that block
 *   modeStats_valid()       ... of which decoded with its check passing
 *   modeStats_badCrc()      ... of which failed it
 *   modeStats_lost()        blocks or frames that never arrived
 *   modeStats_resync()      every time the receiver resynchronises
 *   modeStats_txKey()       the PA is keyed
 *   modeStats_txBlock()     a block/frame left for the modulator
 *   modeStats_txStarve()    a frame went out with no audio behind it
 *   modeStats_txEnd()       the transmission ended
 */

/** Full scale of one input sample, the level fields are in these units */
#define MODE_STATS_FULL_SCALE 32768

/** Duration of one processed block of baseband, ms: the cost budget */
#define MODE_STATS_BLOCK_MS 30

/**
 * Modes with their own row. The values are part of the remote control output
 * and do not change.
 */
enum modeStatsMode
{
    MS_MODE_FM = 0, /**< Analog FM                    */
    MS_MODE_M17,    /**< M17                          */
    MS_MODE_DMR,    /**< DMR                          */
    MS_MODE_TETRA,  /**< TETRA                        */
    MS_MODE_COUNT   /**< Not a mode: every row at once */
};

/**
 * A copy of one mode's counters, for the tests and the remote control.
 *
 * Averages are kept as they will be printed: the mean level is the mean of
 * the per-block means, the mean quality the mean of the per-frame figures.
 */
struct modeStatsValues
{
    uint32_t blocks;  /**< Blocks of baseband processed            */
    uint32_t frames;  /**< Frames, bursts or slots taken from them */
    uint32_t valid;   /**< Frames whose check passed               */
    uint32_t badCrc;  /**< Frames whose check failed               */
    uint32_t lost;    /**< Blocks or frames that never arrived     */
    uint32_t resyncs; /**< Resynchronisations of the receiver      */

    uint32_t levelMean; /**< Mean of the per-block mean |sample| */
    uint32_t levelPeak; /**< Peak |sample| seen                  */
    bool rssiValid;     /**< An RSSI reading was reported        */
    int16_t rssiPeak;   /**< Highest RSSI, dBm                   */
    bool errorValid;    /**< A quality figure was reported       */
    uint32_t errorMean; /**< Mean quality figure, thousandths    */
    bool timingValid;   /**< A timing fit was reported           */
    int32_t timingFit;  /**< Best timing fit, 1/1000 symbol      */
    int32_t timingStep; /**< Step of that fit, 1/1000 symbol     */

    bool costValid;     /**< The demodulation cost was measured  */
    uint32_t costMinUs; /**< Shortest block, microseconds        */
    uint32_t costMeanUs;/**< Mean block, microseconds            */
    uint32_t costMaxUs; /**< Longest block, microseconds         */

    uint32_t txKeys;    /**< Transmissions keyed                 */
    bool txFirstValid;  /**< The first block was timed           */
    uint32_t txFirstMs; /**< PTT to first block out, ms (last)   */
    uint32_t txStarved; /**< Frames sent with no audio in time   */
    uint32_t txGaps;    /**< Blocks that arrived late            */
};

#ifdef CONFIG_C62_MODE_STATS

/**
 * Mark the start of the demodulation of one block of baseband. Reads the
 * cycle counter; pair it with modeStats_block().
 */
void modeStats_blockStart(enum modeStatsMode mode);

/**
 * Mark the end of the demodulation of one block: closes the cost measurement
 * and accounts the level of the samples that went in.
 *
 * @param samples: the block's samples, or NULL to skip the level.
 * @param count: number of samples.
 */
void modeStats_block(enum modeStatsMode mode, const int16_t *samples,
                     size_t count);

/** Account frames, bursts or slots taken out of the blocks. */
void modeStats_frames(enum modeStatsMode mode, uint32_t count);

/** Account frames that decoded and passed their CRC or parity check. */
void modeStats_valid(enum modeStatsMode mode, uint32_t count);

/** Account frames that decoded but failed their check. */
void modeStats_badCrc(enum modeStatsMode mode, uint32_t count);

/** Account blocks or frames that never arrived (overruns, gaps). */
void modeStats_lost(enum modeStatsMode mode, uint32_t count);

/** Account one resynchronisation of the receiver. */
void modeStats_resync(enum modeStatsMode mode);

/** Report an RSSI reading, dBm: the highest one is kept. */
void modeStats_rssi(enum modeStatsMode mode, int dbm);

/** Report a quality figure in thousandths: EVM, phase error, ... */
void modeStats_error(enum modeStatsMode mode, uint32_t thousandths);

/**
 * Report the timing fit the demodulator found.
 *
 * @param fit: best sampling instant, in 1/1000 of a symbol.
 * @param step: step of the search or loop that found it, same units.
 */
void modeStats_timing(enum modeStatsMode mode, int32_t fit, int32_t step);

/** A transmission starts: the PA was keyed at nowMs. */
void modeStats_txKey(enum modeStatsMode mode, uint32_t nowMs);

/**
 * A block or frame left for the modulator. The first one after a key is what
 * gives the PTT to first-block latency, the rest are checked against the
 * mode's own cadence to find the gaps.
 */
void modeStats_txBlock(enum modeStatsMode mode, uint32_t nowMs);

/** A frame went out with no audio behind it: the encoder was late. */
void modeStats_txStarve(enum modeStatsMode mode);

/** The transmission ended. */
void modeStats_txEnd(enum modeStatsMode mode);

/**
 * Clear one mode's counters, or all of them with MS_MODE_COUNT.
 */
void modeStats_reset(enum modeStatsMode mode);

/**
 * Copy one mode's counters out.
 */
void modeStats_get(enum modeStatsMode mode, struct modeStatsValues *out);

/**
 * Format one mode's line, the "rx" reply. One line, no newline, stable field
 * order and names:
 *
 *   rx m17 blk=42 frm=1200 ok=1180 crc=12 lost=0 rs=3 lvl=1024/8192
 *          rssi=-72 q=45 fit=125/200 cost=210/240/900us p=0.8% tx=2
 *          fst=8ms late=1 gap=0
 *
 * Fields with no value in that mode print a dash.
 *
 * @param mode: mode to format.
 * @param buf: destination.
 * @param len: destination size, 200 bytes are enough.
 * @return the length written, not counting the terminator, or -1 on error.
 */
int modeStats_format(enum modeStatsMode mode, char *buf, size_t len);

/** @return the mode's name, as the remote control prints it. */
const char *modeStats_name(enum modeStatsMode mode);

/**
 * Current value of the platform's cycle counter, provided by the platform.
 * The default implementation returns zero: a build without a counter reports
 * a demodulation cost of zero rather than a wrong one.
 */
uint32_t modeStats_platformCycles(void);

#else /* CONFIG_C62_MODE_STATS */

/*
 * Not a single instruction: the hooks are not compiled, none of the symbols
 * above is referenced and the arguments are not evaluated (sizeof does not
 * evaluate its operand), so a call site costs what the sizeof it contains
 * costs, which is nothing. They are typed all the same, so a call that would
 * not compile with the option on does not compile with it off either.
 */
#define modeStats_blockStart(mode)             ((void)sizeof(mode))
#define modeStats_block(mode, samples, count)                                 \
    ((void)sizeof(mode), (void)sizeof(samples), (void)sizeof(count))
#define modeStats_frames(mode, count) ((void)sizeof(mode), (void)sizeof(count))
#define modeStats_valid(mode, count)  ((void)sizeof(mode), (void)sizeof(count))
#define modeStats_badCrc(mode, count) ((void)sizeof(mode), (void)sizeof(count))
#define modeStats_lost(mode, count)   ((void)sizeof(mode), (void)sizeof(count))
#define modeStats_resync(mode)        ((void)sizeof(mode))
#define modeStats_rssi(mode, dbm)     ((void)sizeof(mode), (void)sizeof(dbm))
#define modeStats_error(mode, value)  ((void)sizeof(mode), (void)sizeof(value))
#define modeStats_timing(mode, fit, step)                                     \
    ((void)sizeof(mode), (void)sizeof(fit), (void)sizeof(step))
#define modeStats_txKey(mode, nowMs)  ((void)sizeof(mode), (void)sizeof(nowMs))
#define modeStats_txBlock(mode, nowMs)                                        \
    ((void)sizeof(mode), (void)sizeof(nowMs))
#define modeStats_txStarve(mode) ((void)sizeof(mode))
#define modeStats_txEnd(mode)    ((void)sizeof(mode))

#endif /* CONFIG_C62_MODE_STATS */

#ifdef __cplusplus
}
#endif

#endif /* MODE_STATS_H */
