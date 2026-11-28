/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TXTRACE_H
#define TXTRACE_H

/**
 * @file TxTrace.h
 * @brief Why a PTT did nothing: the steps of the last startTx().
 *
 * A digital mode asks the audio path for a route, starts one or two streams,
 * keys the PA and checks that it came up.  When any of those says no, the mode
 * leaves the operator in receive with TX on the screen and nothing on the air,
 * and the radio has no way of saying *which* of them refused: the return code
 * is a bool by the time it reaches the RTX layer (docs/TX_SAFETY.md, "a PTT
 * that does nothing must be explainable").
 *
 * This is the smallest thing that answers it.  Every transmitting mode calls
 * txTrace_reset() on entry to startTx(), then txTrace_step() after each call
 * that returns a status, txTrace_block() on the early returns that are not a
 * call (a frequency of 0, no encoder, a PA that did not key) and
 * txTrace_keyed() when it has a carrier.  The record is a handful of pointers
 * and one integer: it costs no allocation, no lock and no branch in any path
 * that was not already branching.
 *
 * Only the RTX thread writes it, and it does so from inside startTx(); a reader
 * (the host bench, tests/host/tx_bench) reads it after rtx_task() has returned.
 * Writing it from two threads at once is not supported and is not a case the
 * firmware has.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What the last startTx() did. */
struct txTraceReport
{
    const char *mode;       ///< Mode that filled it, as the mode names itself.
    const char *lastStep;   ///< Last call the mode reached, NULL before any.
    const char *failedCall; ///< First call that returned an error, NULL if none.
    int         failedRc;   ///< What that call returned.
    const char *blockedBy;  ///< Why the mode refused, when no call failed.
    uint8_t     steps;      ///< Calls reached, in order.
    bool        keyed;      ///< The mode got the PA keyed and returned true.
};

/** Forget the previous transmission. Call first thing in startTx(). */
void txTrace_reset(const char *mode);

/**
 * Note a call and what it returned.
 *
 * @param call: the call, as it is written in the source.
 * @param rc: what it returned; a negative value is remembered as the failure.
 */
void txTrace_step(const char *call, int rc);

/**
 * Note a refusal that is not a call: a precondition the mode checked itself.
 *
 * @param why: the precondition, in words.
 */
void txTrace_block(const char *why);

/** Note that the transmission got its carrier. */
void txTrace_keyed(void);

/**
 * Read the record of the last startTx().
 *
 * @param out: filled with the record.
 */
void txTrace_report(struct txTraceReport *out);

#ifdef __cplusplus
}
#endif

#endif /* TXTRACE_H */
