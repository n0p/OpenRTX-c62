/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BLACKBOX_H
#define BLACKBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file blackbox.h
 * @brief Post-mortem recorder: breadcrumbs, liveness beats, crash record.
 *
 * A radio that freezes leaves nothing behind: no log line, no trace, and no way
 * to tell which thread stopped.  This layer leaves a small record in flash
 * before the radio reboots, and the next boot prints it and invalidates it.
 *
 * It has three parts, and they are separate on purpose:
 *
 *  - **Beats.**  Each thread that matters stamps a counter once per pass
 *    (bb_beat()).  A supervisor outside all of them notices that one stopped.
 *  - **Breadcrumbs.**  The paths that are suspected when something wedges --
 *    a mode change, a radio reconfiguration, an audio path opening, a keying
 *    -- drop a one-word mark (bb_mark()).  The record keeps the last
 *    BB_MARK_N, newest last, so the crash is read backwards.
 *  - **The record.**  A fixed 516-byte layout (@ref bbLayout) with magic,
 *    version, a validity signature and a CRC-16, written once to its own flash
 *    sector and read back at boot by the platform.
 *
 * Everything except the platform hooks is portable C with no Zephyr, no
 * pthread and no stdio in it: the host tests drive the detector with a fake
 * clock and a fake flash, and the same code runs on the radio.
 *
 * @section cost What it costs
 *
 *  - bb_mark(): one store of a 32-bit word into a ring in .bss and one
 *    increment of the ring head.  No lock, no allocation, no clock read: the
 *    tick comes from a word that the beats and the supervisor keep fresh, so
 *    the function is safe from a thread, from an interrupt and from a fault
 *    handler.  Two loads, an AND, an OR and two stores.
 *  - bb_beat(): one increment, one clock read (once per thread pass) and one
 *    store of the coarse clock.
 *  - RAM: BB_MARK_N * 4 bytes of ring, BB_BEAT_COUNT counters, the thread
 *    pointers used to name them, and the platform's copy of the last record
 *    (BB_RECORD_SIZE) for the debug protocol.
 *  - Flash: one 4 KiB sector of the storage partition, of which 516 bytes are
 *    used.  The record costs nothing when nothing crashes: the sector is
 *    written only when a supervisor decides the radio is wedged.
 */

/** Marks kept in the ring, and in the record.  Power of two. */
#define BB_MARK_N 32

/** Threads the record can hold.  The walk stops at this many. */
#define BB_MAX_THREADS 10

/**
 * Size of one record on flash, CRC included and padded to 4 bytes.
 *
 * Fixed, so that the reader needs no length field to know how much to read and
 * a truncated write is detected by the CRC rather than by a length that agrees
 * with itself.
 */
#define BB_RECORD_SIZE 516

/** Where the CRC-16/CCITT-FALSE over bytes [0, BB_OFF_CRC) lives. */
#define BB_OFF_CRC 512

/** "BBX1", little endian, first word of the record. */
#define BB_RECORD_MAGIC 0x31425842U

/** Layout version.  A reader that does not know it says so and stops. */
#define BB_RECORD_VERSION 1U

/**
 * The validity signature.
 *
 * Separate from the magic so that the record can be invalidated after it has
 * been read without erasing the sector: writing zero over these four bytes
 * (a 1 -> 0 write, legal on NOR flash and aligned to the write block) turns a
 * valid record into an invalid one, magic, CRC and all.
 */
#define BB_VALID_SIGNATURE 0x42425258U

/** Fixed offsets of the record layout, all little endian (@ref bbLayout). */
#define BB_OFF_MAGIC 0     /**< u32  BB_RECORD_MAGIC                       */
#define BB_OFF_VERSION 4   /**< u16  BB_RECORD_VERSION                     */
#define BB_OFF_HDRSIZE 6   /**< u16  BB_HDR_SIZE, so a reader can skip it   */
#define BB_OFF_VALID \
    8                      /**< u32  BB_VALID_SIGNATURE when the record is
                            *   untouched; 0 once it has been read        */
#define BB_OFF_UPTIME 12   /**< u32  ms since boot when the hang was seen  */
#define BB_OFF_REASON 16   /**< u8   enum bbReason, 0 in bytes 17..19      */
#define BB_OFF_DETAIL 20   /**< u32  stall mask or fatal error reason      */
#define BB_OFF_RESET 24    /**< u32  platform reset cause                  */
#define BB_OFF_CRUMBSEQ 28 /**< u32  bb_mark() calls since boot            */
#define BB_OFF_RTXMODE 32  /**< u16  enum opmode the radio was in          */
#define BB_OFF_UIMODE 34   /**< u16  enum opmode the UI had selected       */
#define BB_OFF_RXFREQ 36   /**< u32  Hz                                    */
#define BB_OFF_TXFREQ 40   /**< u32  Hz                                    */
#define BB_OFF_RTXFLAGS 44 /**< u16  bit0 rx on, bit1 tx on, bit2 keyed    */
#define BB_OFF_THREADS 46  /**< u16  thread records that follow            */
#define BB_OFF_CRUMBS 48   /**< u16  breadcrumbs that follow               */
#define BB_OFF_THREADSZ 50 /**< u16  BB_THREAD_REC_SIZE                    */
#define BB_OFF_SPARE 52    /**< 12 bytes reserved, zero                    */
#define BB_HDR_SIZE 64

/** One thread record. */
#define BB_THREAD_REC_SIZE 32
#define BB_OFF_THREADS_BASE BB_HDR_SIZE
#define BB_OFF_CRUMBS_BASE \
    (BB_OFF_THREADS_BASE + BB_MAX_THREADS * BB_THREAD_REC_SIZE)
#define BB_TR_NAME 0   /**< 12 bytes, NUL padded                       */
#define BB_TR_PRIO 12  /**< i8                                         */
#define BB_TR_STATE 13 /**< u8  Zephyr thread_state, 0 when unknown    */
#define BB_TR_FREE 14  /**< u16 bytes of stack never used              */
#define BB_TR_SIZE 16  /**< u16 stack size in bytes                    */
#define BB_TR_ROLE \
    18                 /**< u16 enum bbBeat + 1 when a watched thread,
                           *   0 for everything else                     */
#define BB_TR_PC 20    /**< u32 stacked PC, 0 when it could not be read */
#define BB_TR_LR 24    /**< u32 stacked LR, 0 when it could not be read */
#define BB_TR_SP 28    /**< u32 stack pointer at the last switch out   */

/** Default stall threshold, milliseconds.  Each watched beat has its own. */
#ifndef BB_STALL_MS
#define BB_STALL_MS 2000
#endif

/**
 * Threads whose liveness is watched.  The record's thread table names them
 * through their role, so a reader does not depend on how the platform names
 * its threads.
 *
 * The value is also the bit in the record's stall mask, so a new beat is
 * appended, never inserted: a record written by an older firmware has to keep
 * reading as what it was.
 */
typedef enum bbBeat {
    BB_BEAT_UI = 0, /**< the user interface thread: keys, screen, FSM    */
    BB_BEAT_RTX,    /**< the radio thread: modes, chip, audio streams    */
    BB_BEAT_PROTO,  /**< the PC protocol transport thread                */
    BB_BEAT_MAIN,   /**< the device thread: state, watchdog, supervise   */
    BB_BEAT_KBD,    /**< the keypad scanner thread: the matrix, at 50 Hz  */
    BB_BEAT_COUNT
} bbBeat_t;

/** Why the record was written. */
typedef enum bbReason {
    BB_REASON_NONE = 0,
    BB_REASON_STALL, /**< a watched thread stopped beating                */
    BB_REASON_FATAL, /**< a fault, an assert or a stack overflow          */
    BB_REASON_MANUAL /**< asked for by the operator or by a test          */
} bbReason_t;

/**
 * Breadcrumb identifiers.
 *
 * Short, stable numbers: the record stores the identifier and the reader turns
 * it into text with bb_markName(), so a new mark only needs an entry here, in
 * the name table, and the call site.
 */
typedef enum bbMark {
    BB_M_NONE = 0,
    BB_M_BOOT,         /**< the firmware reached the end of start-up      */
    BB_M_TERM,         /**< a shutdown was asked for                      */
    BB_M_MODE_ENTER,   /**< switchMode(): leaving the old mode           */
    BB_M_MODE_READY,   /**< switchMode(): the new mode reports receive   */
    BB_M_MODE_FAIL,    /**< switchMode(): the new mode did not come up   */
    BB_M_MODE_OFF,     /**< switchMode(): nothing selected               */
    BB_M_RTX_CONF,     /**< rtx_task() applied a new configuration       */
    BB_M_RADIO_CONF,   /**< radio_updateConfiguration()                  */
    BB_M_RX_ON,        /**< radio_enableRx()                             */
    BB_M_TX_ON,        /**< radio_enableTx()                             */
    BB_M_TX_OFF,       /**< radio_disableRtx()                           */
    BB_M_TX_ABORT,     /**< rtx_supervise() had to unkey                 */
    BB_M_AUDIO_OPEN,   /**< audioPath_request() opened a path            */
    BB_M_AUDIO_CLOSE,  /**< audioPath_release() closed one               */
    BB_M_AIRBAND_LOAD, /**< radio_setOpmode(): the AM profile was written  */
    BB_M_WFM_ENTER,    /**< the broadcast FM tuner was started           */
    BB_M_SETTINGS,     /**< settings were written                        */
    BB_M_CHAN_LOAD,    /**< a stored channel was applied to the radio    */
    BB_M_UI_MODE,      /**< the operator selected another mode           */
    BB_M_BACKUP,       /**< the internal backup of the stores was written */
    BB_M_RESTORE,      /**< the internal backup was restored             */
    BB_M_HANG,         /**< dropped just before the record was written   */
    BB_M_DISP_DROP,    /**< the screen dropped a frame on its deadline    */
    BB_M_COUNT
} bbMark_t;

/** What the platform knows about the radio when the record is written. */
typedef struct {
    uint16_t rtxMode;  /**< enum opmode the RTX thread was running       */
    uint16_t uiMode;   /**< enum opmode the UI had selected              */
    uint16_t rtxFlags; /**< bit0 rx, bit1 tx, bit2 keyed                 */
    uint32_t rxFreq;
    uint32_t txFreq;
    uint32_t resetCause; /**< platform-specific, printed raw               */
    uint16_t threadCount;
    struct {
        char name[12];
        int8_t prio;
        uint8_t state;
        uint16_t stackFree;
        uint16_t stackSize;
        uint16_t role; /**< enum bbBeat + 1, 0 when not a watched thread */
        uint32_t pc;
        uint32_t lr;
        uint32_t sp;
    } threads[BB_MAX_THREADS];
} bbSnapshot_t;

/** How the record reaches its flash sector, and how it is read back. */
typedef struct {
    int (*read)(void *ctx, uint32_t off, void *buf, size_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    void *ctx; /**< partition handle: a flash_area on the radio         */
} bbFlashIo_t;

/*
 * ---------------------------------------------------------------------------
 * Hot path
 * ---------------------------------------------------------------------------
 */

/**
 * Stamp a beat.  Called once per pass of a watched thread.
 *
 * Also refreshes the coarse clock that bb_mark() stamps breadcrumbs with, so
 * this is the one place that reads the platform clock.
 *
 * @param beat: BB_BEAT_*.
 */
void bb_beat(uint8_t beat);

/**
 * Drop a breadcrumb.  One store, no lock, safe from any context.
 *
 * @param mark: BB_M_*, or any other identifier: the record keeps the number
 *              and the reader prints it as text only when it knows it.
 */
void bb_mark(uint8_t mark);

/** Refresh the coarse clock without stamping a beat. */
void bb_tick(uint32_t nowMs);

/**
 * Forget every beat, every breadcrumb and the detector's own state.
 *
 * Not needed in normal operation -- the record is one per crash -- but it is
 * what makes the detector testable one case at a time, and what a "clear the
 * black box" action would call.  Use it while suspended: a beat that lands
 * between the counter and the ring would be half forgotten.
 */
void bb_clear(void);

/** Stop watching, e.g. while the radio is shutting its threads down. */
void bb_suspend(void);

/** Start watching again.  Beats that have not moved since are re-primed. */
void bb_resume(void);

/**
 * One pass of the supervisor.
 *
 * Compares every watched beat with the value it had at the previous pass: a
 * counter that has not moved for BB_STALL_MS is a stall.  Beats that have
 * never been stamped (thread not in this build, not started yet) are not
 * watched.
 *
 * A stall is reported **once**.  The pass that finds one stops watching before
 * it calls the platform hook: the hook writes the record and resets the radio,
 * and if the reset does not take -- the SoC refusing it, a platform that
 * cannot reset, a test -- the supervisor must not come back every 100 ms to
 * erase the record's flash sector and write it again.  One hang, one record,
 * one reset attempt; the hardware watchdog is the fallback, not a rewrite
 * loop.
 *
 * @param nowMs: monotonic clock, milliseconds.  Passed in, not read, so the
 *               host test can drive it deterministically.
 * @return true when a stall was detected and handed to the platform.
 */
bool bb_supervise(uint32_t nowMs);

/** Bitmask of the beats currently considered stuck (bit = enum bbBeat). */
uint32_t bb_stalledMask(void);

/** Value of the beat counter, for the tests and for the debug protocol. */
uint32_t bb_beatCount(uint8_t beat);

/** Breadcrumbs dropped since boot, for the record and for wrap detection. */
uint32_t bb_markCount(void);

/**
 * Which watched beat a thread owns.
 *
 * The thread is known from the first bb_beat() it stamped, so the record can
 * name the threads it watches by role instead of relying on the platform's
 * thread names.
 *
 * @param thread: platform thread handle, as returned by
 *                bb_platformCurrentThread().
 * @return enum bbBeat + 1, or 0 when the thread is not one of them.
 */
uint16_t bb_threadRole(const void *thread);

/** Thread that owns a beat (NULL until it has beaten once). */
const void *bb_threadOfBeat(uint8_t beat);

/*
 * ---------------------------------------------------------------------------
 * Record
 * ---------------------------------------------------------------------------
 */

/**
 * Build a record in @p rec.
 *
 * Fills the header from the live state (beats, breadcrumbs, clock), asks the
 * platform for the radio and thread snapshot, and computes the CRC.
 *
 * @param rec: destination, BB_RECORD_SIZE bytes.
 * @param cap: bytes available at @p rec.
 * @return the record length (BB_RECORD_SIZE), or a negative value.
 */
int bb_recordBuild(uint8_t *rec, size_t cap, uint32_t nowMs, uint8_t reason,
                   uint32_t detail);

/** True when @p rec holds a complete, valid, not yet read record. */
bool bb_recordValid(const uint8_t *rec, size_t len);

/**
 * Read the record from flash and check it.
 *
 * @param io: partition accessors.
 * @param rec: destination, BB_RECORD_SIZE bytes.
 * @param cap: bytes available.
 * @return 0 when a valid record was read, a negative value otherwise.
 */
int bb_recordLoad(const bbFlashIo_t *io, uint8_t *rec, size_t cap);

/**
 * Turn a stored record into an invalid one without erasing the sector.
 *
 * Writes zero over the validity word: a 1 -> 0 write of the flash write block
 * size, so the record is not served twice and the sector does not cost an
 * erase on every boot.
 *
 * @param io: partition accessors.
 * @return 0 on success.
 */
int bb_recordInvalidate(const bbFlashIo_t *io);

/**
 * Render a record as readable text, one line at a time.
 *
 * @param rec: the record.
 * @param len: bytes available.
 * @param emit: called once per line, without a line terminator.
 * @param ctx: passed to @p emit.
 */
void bb_textDump(const uint8_t *rec, size_t len,
                 void (*emit)(void *ctx, const char *line), void *ctx);

/** Names for the text dump and for the debug protocol. */
const char *bb_beatName(uint8_t beat);
const char *bb_markName(uint8_t mark);
const char *bb_reasonName(uint8_t reason);
const char *bb_modeName(uint16_t mode);

/*
 * ---------------------------------------------------------------------------
 * Platform hooks: weak defaults in core/blackbox.c, overridden by the platform
 * ---------------------------------------------------------------------------
 */

/** Fill @p s with the radio state and the thread table. */
void bb_platformSnapshot(bbSnapshot_t *s);

/**
 * Write the record to its flash sector.
 *
 * @return 0 when the record is on flash and will still be there after the
 *         reset.
 */
int bb_platformPersist(const uint8_t *rec, size_t len);

/** Reboot.  Called after the record has been written, whatever the outcome. */
void bb_platformReboot(void);

/**
 * What to do when a stall is detected.  The default builds the record, writes
 * it and reboots; a test overrides it to see the record without dying.
 */
void bb_platformHang(uint32_t mask, uint32_t nowMs);

/** Pointer to the calling thread, so the record can name the watched ones. */
const void *bb_platformCurrentThread(void);

/** Monotonic milliseconds. */
uint32_t bb_platformNowMs(void);

/**
 * Capture and persist a record now, without rebooting.
 *
 * Used by bb_platformHang() before the reset, by the fault handler, and by the
 * debug protocol when the operator asks for a manual post-mortem.
 *
 * @return true when the record reached the flash.
 */
bool bb_capture(uint8_t reason, uint32_t detail, uint32_t nowMs);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_H */
