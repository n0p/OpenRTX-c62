/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TX_GUARD_H
#define OPENRTX_TX_GUARD_H

/**
 * @file TxGuard.h
 * @brief Transmit lifecycle: phases with deadlines, a bound on every stage and
 *        the trace of the last transmission.
 *
 * Every digital mode keys the PA and then waits for something: the baseband
 * stream to start, the codec to deliver the first frame, the last slot to
 * drain.  Any of those waits can fail to end -- a stream whose consumer never
 * runs, an encoder thread that died, a DSP call that never returns -- and the
 * result is the same: a keyed transmitter that nobody can stop, because the
 * thread that would unkey is the one that is blocked.
 *
 * This module is the bookkeeping half of the answer, the other half is that the
 * supervisor (rtx_supervise(), called from the main thread) can force the
 * streams closed and the PA off from *outside* the stuck thread.  It knows
 * nothing about the radio or about threads: the caller passes the time in, so
 * the state machine and its deadlines are walked on the host (see
 * tests/unit/tx_guard.cpp).
 *
 * Phases, in order, with the budget of each:
 *
 *   idle ──enter()──▶ starting ──streamsOpen()──▶ wait_first
 *                                                      │ firstBlock()
 *                                                      ▼
 *                                                   running ──finishing()──▶ draining ──exit()
 *
 * A phase that overruns its budget, or a transmission that lasts longer than
 * TXG_MAX_TX_MS, makes txg_tick() return TXG_ACTION_ABORT once: the supervisor
 * then unkeys and cancels the streams, and the mode unwinds through its normal
 * exit path (the abort is idempotent and the exit is still the mode's).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Lifecycle phases of a transmission. */
enum txGuardPhase {
    TXG_PHASE_IDLE = 0,   ///< Not transmitting
    TXG_PHASE_STARTING,   ///< PTT seen, opening the streams
    TXG_PHASE_WAIT_FIRST, ///< Streams open, no block handed to the sink yet
    TXG_PHASE_RUNNING,    ///< Blocks flowing, the operator keys the radio
    TXG_PHASE_DRAINING,   ///< PTT released, terminator and tail on the air
    TXG_PHASE_ABORT       ///< Aborted by the supervisor, waiting for the exit
};

/** Why a transmission ended, or why it was aborted. */
enum txGuardReason {
    TXG_REASON_NONE = 0,      ///< Still transmitting
    TXG_REASON_PTT,           ///< The operator released the PTT
    TXG_REASON_MODE,          ///< The mode ended the call (terminator, EOT)
    TXG_REASON_START_FAILED,  ///< The streams could not be opened
    TXG_REASON_ENCODER,       ///< The encoder stopped producing
    TXG_REASON_TIMEOUT_START, ///< Aborted: the streams never opened
    TXG_REASON_TIMEOUT_FIRST, ///< Aborted: no block reached the sink
    TXG_REASON_TIMEOUT_DRAIN, ///< Aborted: the tail never ended
    TXG_REASON_TIMEOUT_MAX,   ///< Aborted: the transmission ran too long
    TXG_REASON_COUNT
};

/** What the supervisor has to do after a txg_tick(). */
enum txGuardAction {
    TXG_ACTION_CONTINUE = 0, ///< Nothing to do
    TXG_ACTION_ABORT         ///< Unkey and cancel the streams now
};

/**
 * Phase budgets, in milliseconds.  They are the numbers the radio is required
 * to meet, not estimates: what does not fit is a failure to report.
 */
#define TXG_START_MS 250     ///< PTT to streams open
#define TXG_FIRST_MS 300     ///< Streams open to the first block in the sink
#define TXG_DRAIN_MS 160     ///< PTT released to out of TX (the UI asks < 200)
#define TXG_MAX_TX_MS 300000 ///< Five minutes: a stuck PTT, not a conversation
/** Events kept in the trace of the last transmission. */
#define TXG_TRACE_EVENTS 10
/** Blocks per second the sink is expected to consume, for the cadence check. */
#define TXG_BLOCK_MS 30

/** One entry of the trace: what happened and when. */
struct txGuardEvent {
    uint8_t code;  ///< enum txGuardEvent
    uint32_t time; ///< Milliseconds since boot
};

/** Everything the supervisor, the trace and the tests look at. */
struct txGuard {
    uint8_t phase;          ///< enum txGuardPhase
    uint8_t reason;         ///< enum txGuardReason, once the transmission ended
    uint32_t enterTime;     ///< PTT seen
    uint32_t streamTime;    ///< Streams open
    uint32_t firstTime;     ///< First block handed to the sink
    uint32_t finishTime;    ///< PTT released / last block rendered
    uint32_t exitTime;      ///< Out of TX
    uint32_t blocks;        ///< Blocks handed to the sink in this transmission
    uint32_t slips;         ///< Blocks that arrived late: cadence lost
    uint32_t blockTime;     ///< Time of the last block
    uint32_t transmissions; ///< Transmissions since boot
    uint32_t aborts;        ///< Aborted transmissions since boot
    uint32_t lastLatencyMs; ///< enterTime to firstTime of the last one
    uint32_t lastTailMs;    ///< finishTime to exitTime of the last one
    struct txGuardEvent trace[TXG_TRACE_EVENTS];
    uint8_t traceCount;     ///< Entries written, saturating
};

/** Trace event codes. */
enum txGuardEventCode {
    TXG_EV_ENTER = 0, ///< PTT seen
    TXG_EV_OPENED,    ///< Streams open
    TXG_EV_OPEN_FAIL, ///< Streams refused to open
    TXG_EV_FIRST,     ///< First block handed to the sink
    TXG_EV_FINISH,    ///< Last block rendered, tail in flight
    TXG_EV_EXIT,      ///< Out of TX
    TXG_EV_ABORT,     ///< Supervisor forced the end
    TXG_EV_KEYED,     ///< PA keyed
    TXG_EV_ENCODER,   ///< Encoder stopped producing
    TXG_EV_CADENCE    ///< A block arrived later than TXG_BLOCK_MS
};

/**
 * Reset a guard to idle.
 *
 * @param guard: guard to reset.
 * @param now: current time, in milliseconds.
 */
void txg_init(struct txGuard *guard, uint32_t now);

/**
 * Open a transmission.  Ignored, and false, if one is already open.
 *
 * @param guard: guard.
 * @param now: current time, in milliseconds.
 * @return true if the transmission was opened.
 */
bool txg_enter(struct txGuard *guard, uint32_t now);

/**
 * Note that the baseband/audio streams of this transmission are open.
 *
 * @param guard: guard.
 * @param now: current time, in milliseconds.
 */
void txg_streamsOpen(struct txGuard *guard, uint32_t now);

/**
 * Note that the streams could not be opened: the transmission failed before it
 * ever keyed, and the mode is returning to receive.
 *
 * @param guard: guard.
 * @param now: current time, in milliseconds.
 */
void txg_streamsFailed(struct txGuard *guard, uint32_t now);

/**
 * Note the first block handed to the sink: from here the transmitter is
 * putting samples on the air.
 *
 * @param guard: guard.
 * @param now: current time, in milliseconds.
 */
void txg_firstBlock(struct txGuard *guard, uint32_t now);

/**
 * Note a block handed to the sink, after the first one.  Used to count them and
 * to flag a cadence that slipped.
 *
 * @param guard: guard.
 * @param now: current time, in milliseconds.
 */
void txg_block(struct txGuard *guard, uint32_t now);

/**
 * Note that the mode started its tail: PTT released, terminator on the air.
 *
 * @param guard: guard.
 * @param now: current time, in milliseconds.
 */
void txg_finishing(struct txGuard *guard, uint32_t now);

/**
 * Leave transmit.  Always safe to call, from any phase.
 *
 * @param guard: guard.
 * @param now: current time, in milliseconds.
 * @param reason: why the transmission ended.
 */
void txg_exit(struct txGuard *guard, uint32_t now, enum txGuardReason reason);

/**
 * Check the deadlines of the current transmission.  Call it periodically from
 * the supervisor: it returns TXG_ACTION_ABORT at the first overrun and keeps
 * returning it until txg_exit(), so a lost abort request cannot be forgotten.
 *
 * @param guard: guard.
 * @param now: current time, in milliseconds.
 * @param ptt: true while the operator holds the PTT.
 * @return what the supervisor has to do.
 */
enum txGuardAction txg_tick(struct txGuard *guard, uint32_t now, bool ptt);

/**
 * Note an event in the trace.
 *
 * @param guard: guard.
 * @param code: event code.
 * @param now: current time, in milliseconds.
 */
void txg_note(struct txGuard *guard, enum txGuardEventCode code, uint32_t now);

/**
 * Name of a reason, for the log and the remote control.
 *
 * @param reason: reason code.
 * @return a constant string, never NULL.
 */
const char *txg_reasonName(uint8_t reason);

/**
 * Name of a phase.
 *
 * @param phase: phase code.
 * @return a constant string, never NULL.
 */
const char *txg_phaseName(uint8_t phase);

/**
 * One-line summary of the last transmission: the trace that says where a
 * transmission stopped.
 *
 * Format: "TX n=3 ab=0 ph=exit rs=ptt ent=1200 str=+8 fst=+31 blk=47 last=31
 *          tail=94 pk=12345"
 *
 * @param guard: guard.
 * @param buf: destination buffer.
 * @param len: size of the buffer.
 * @return the characters written, without the terminator, or a negative error.
 */
int txg_format(const struct txGuard *guard, char *buf, size_t len);

/**
 * Detailed trace, one event per line group: "t=+12 opened t=+40 first".
 *
 * @param guard: guard.
 * @param buf: destination buffer.
 * @param len: size of the buffer.
 * @return the characters written, without the terminator, or a negative error.
 */
int txg_formatTrace(const struct txGuard *guard, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TX_GUARD_H */
