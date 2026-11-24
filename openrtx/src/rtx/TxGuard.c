/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx/TxGuard.h"

#include <stdio.h>
#include <string.h>

static const char *const reasonNames[TXG_REASON_COUNT] = {
    "none",     "ptt",      "mode",     "start", "encoder",
    "to-start", "to-first", "to-drain", "to-max",
};

static const char *const phaseNames[] = {
    "idle", "start", "first", "run", "drain", "abort",
};

void txg_note(struct txGuard *guard, enum txGuardEventCode code, uint32_t now)
{
    if (guard->traceCount >= TXG_TRACE_EVENTS)
        return;

    struct txGuardEvent *ev = &guard->trace[guard->traceCount];
    ev->code = (uint8_t)code;
    ev->time = now;
    guard->traceCount++;
}

void txg_init(struct txGuard *guard, uint32_t now)
{
    uint32_t transmissions = guard->transmissions;
    uint32_t aborts = guard->aborts;

    memset(guard, 0, sizeof(*guard));
    guard->phase = TXG_PHASE_IDLE;
    guard->reason = TXG_REASON_NONE;
    guard->enterTime = now;
    guard->exitTime = now;
    /* Counters of the radio, not of this transmission */
    guard->transmissions = transmissions;
    guard->aborts = aborts;
}

static void startPhase(struct txGuard *guard, enum txGuardPhase phase)
{
    guard->phase = (uint8_t)phase;
}

bool txg_enter(struct txGuard *guard, uint32_t now)
{
    if (guard->phase != TXG_PHASE_IDLE)
        return false;

    /* A new trace per transmission; the counters of the radio stay */
    guard->reason = TXG_REASON_NONE;
    guard->firstTime = 0;
    guard->finishTime = 0;
    guard->blocks = 0;
    guard->slips = 0;
    guard->blockTime = now;
    guard->enterTime = now;
    guard->streamTime = now;
    guard->traceCount = 0;
    guard->transmissions++;
    startPhase(guard, TXG_PHASE_STARTING);
    txg_note(guard, TXG_EV_ENTER, now);
    return true;
}

void txg_streamsOpen(struct txGuard *guard, uint32_t now)
{
    if (guard->phase != TXG_PHASE_STARTING)
        return;

    guard->streamTime = now;
    txg_note(guard, TXG_EV_OPENED, now);
    startPhase(guard, TXG_PHASE_WAIT_FIRST);
}

void txg_streamsFailed(struct txGuard *guard, uint32_t now)
{
    txg_note(guard, TXG_EV_OPEN_FAIL, now);
    txg_exit(guard, now, TXG_REASON_START_FAILED);
}

void txg_firstBlock(struct txGuard *guard, uint32_t now)
{
    if ((guard->phase != TXG_PHASE_STARTING)
        && (guard->phase != TXG_PHASE_WAIT_FIRST))
        return;

    guard->firstTime = now;
    guard->blockTime = now;
    guard->blocks = 1;
    guard->lastLatencyMs = now - guard->enterTime;
    txg_note(guard, TXG_EV_FIRST, now);
    startPhase(guard, TXG_PHASE_RUNNING);
}

void txg_block(struct txGuard *guard, uint32_t now)
{
    if ((guard->phase != TXG_PHASE_RUNNING)
        && (guard->phase != TXG_PHASE_DRAINING))
        return;

    guard->blocks++;
    /*
     * A block that arrives later than two slots means the cadence slipped:
     * the sink is not being fed at the rate the mode renders at.  One late
     * block is a scheduling hiccup; a run of them is a starving stream, and
     * the trace tells it apart from a burst rate that was wrong from the
     * start (see the block count and the transmission time).
     */
    if ((now - guard->blockTime) > 2 * TXG_BLOCK_MS) {
        guard->slips++;
        if (guard->slips <= 2)
            txg_note(guard, TXG_EV_CADENCE, now);
    }
    guard->blockTime = now;
}

void txg_finishing(struct txGuard *guard, uint32_t now)
{
    if ((guard->phase != TXG_PHASE_RUNNING)
        && (guard->phase != TXG_PHASE_WAIT_FIRST))
        return;

    guard->finishTime = now;
    txg_note(guard, TXG_EV_FINISH, now);
    startPhase(guard, TXG_PHASE_DRAINING);
}

void txg_exit(struct txGuard *guard, uint32_t now, enum txGuardReason reason)
{
    if (guard->phase == TXG_PHASE_IDLE)
        return;

    /* An aborted transmission keeps the reason it was aborted for: that is the
     * answer the operator needs, not the reason the mode then used to leave. */
    if (guard->phase == TXG_PHASE_ABORT)
        reason = (enum txGuardReason)guard->reason;

    if (guard->firstTime == 0)
        guard->lastLatencyMs = now - guard->enterTime;
    if (guard->finishTime == 0)
        guard->finishTime = now;
    guard->lastTailMs = now - guard->finishTime;
    guard->exitTime = now;
    guard->reason = (uint8_t)reason;
    guard->phase = TXG_PHASE_IDLE;
    txg_note(guard, TXG_EV_EXIT, now);
}

enum txGuardAction txg_tick(struct txGuard *guard, uint32_t now, bool ptt)
{
    switch (guard->phase) {
        case TXG_PHASE_STARTING:
            if (now - guard->enterTime > TXG_START_MS) {
                guard->aborts++;
                guard->reason = TXG_REASON_TIMEOUT_START;
                guard->phase = TXG_PHASE_ABORT;
                txg_note(guard, TXG_EV_ABORT, now);
            }
            break;

        case TXG_PHASE_WAIT_FIRST:
            if (now - guard->streamTime > TXG_FIRST_MS) {
                guard->aborts++;
                guard->reason = TXG_REASON_TIMEOUT_FIRST;
                guard->phase = TXG_PHASE_ABORT;
                txg_note(guard, TXG_EV_ABORT, now);
            }
            break;

        case TXG_PHASE_RUNNING:
            /*
             * The PTT is the operator's, not a deadline: a long over is
             * legitimate.  What is not is a transmission that outlives the
             * absolute cap while the thread that owns it is gone.
             */
            if (now - guard->enterTime > TXG_MAX_TX_MS) {
                guard->aborts++;
                guard->reason = TXG_REASON_TIMEOUT_MAX;
                guard->phase = TXG_PHASE_ABORT;
                txg_note(guard, TXG_EV_ABORT, now);
            }
            break;

        case TXG_PHASE_DRAINING:
            /* The tail must end whether or not the PTT is still held down */
            if (now - guard->finishTime > TXG_DRAIN_MS) {
                guard->aborts++;
                guard->reason = TXG_REASON_TIMEOUT_DRAIN;
                guard->phase = TXG_PHASE_ABORT;
                txg_note(guard, TXG_EV_ABORT, now);
            }
            break;

        case TXG_PHASE_ABORT:
            /* Stay here until the mode unwinds: an abort is not forgotten */
            return TXG_ACTION_ABORT;

        default:
            break;
    }

    (void)ptt;
    return (guard->phase == TXG_PHASE_ABORT) ? TXG_ACTION_ABORT
                                             : TXG_ACTION_CONTINUE;
}

const char *txg_reasonName(uint8_t reason)
{
    return (reason < TXG_REASON_COUNT) ? reasonNames[reason] : "?";
}

const char *txg_phaseName(uint8_t phase)
{
    return (phase < sizeof(phaseNames) / sizeof(phaseNames[0]))
               ? phaseNames[phase]
               : "?";
}

int txg_format(const struct txGuard *guard, char *buf, size_t len)
{
    int n = snprintf(buf, len,
                     "TX n=%lu ab=%lu ph=%s rs=%s ent=%lu str=+%ld fst=+%ld "
                     "blk=%lu slip=%lu lat=%lu tail=%lu",
                     (unsigned long)guard->transmissions,
                     (unsigned long)guard->aborts,
                     txg_phaseName(guard->phase),
                     txg_reasonName(guard->reason),
                     (unsigned long)guard->enterTime,
                     (long)(guard->streamTime - guard->enterTime),
                     (long)((guard->firstTime == 0 ? guard->exitTime
                                                   : guard->firstTime)
                            - guard->enterTime),
                     (unsigned long)guard->blocks,
                     (unsigned long)guard->slips,
                     (unsigned long)guard->lastLatencyMs,
                     (unsigned long)guard->lastTailMs);
    return n;
}

static const char *const eventNames[] = {
    "enter", "open", "openfail", "first", "finish",
    "exit",  "abort", "keyed",   "enc",   "late",
};

int txg_formatTrace(const struct txGuard *guard, char *buf, size_t len)
{
    size_t used = 0;
    int n;

    for (uint8_t i = 0; (i < guard->traceCount) && (used < len); i++) {
        const struct txGuardEvent *ev = &guard->trace[i];
        const char *name = (ev->code < sizeof(eventNames) / sizeof(*eventNames))
                               ? eventNames[ev->code]
                               : "?";
        n = snprintf(buf + used, len - used, "%s%ld:%s", (i == 0) ? "" : " ",
                     (long)(ev->time - guard->enterTime), name);
        if (n < 0)
            return n;
        used += (size_t)n;
    }

    return (int)used;
}
