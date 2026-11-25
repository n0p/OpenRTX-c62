/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrTxPlan.h
 * @brief What one 30 ms slot of a DMR voice call carries. Pure, so the host
 *        can walk it.
 *
 * The transmitter's slot machine is the part of the mode a test can check
 * without a radio: which slots carry a voice LC header, which ones carry a
 * voice burst, when the terminator goes out, and -- the rule that was missing
 * -- how the call ends when the operator lets go of the PTT.  Keeping it here,
 * as a function of the counters and of the PTT, is what lets
 * tests/host/dmr_tx_host run the real decision code and then decode the bursts
 * it produced with the repository's own receiver.
 *
 * The state machine as the radio runs it:
 *
 *   headers (2)  ->  voice (until the PTT goes up)  ->  terminator  ->  tail
 *
 * with the TDMA rule on top: a TDMA frame is two 30 ms slots and the call owns
 * exactly one of them, so a burst goes out every 60 ms and the slot in between
 * is silence.  The timeslot selects which of the two, and the counter that makes
 * the choice is the slot counter -- not the burst counter, which advances only
 * when a burst goes out and so cannot alternate.  The mode used to put a burst
 * in *every* slot on timeslot 1 (twice the duty cycle a TDMA channel allows,
 * and half of them in the other timeslot's slot, where a repeater sees them as
 * collisions) while on timeslot 2 its parity was derived from the burst count
 * and froze.
 *
 * Why the "ending" arrow matters: txSlot() used to have no transition into the
 * terminator stage at all -- nothing in the mode ever wrote stage 2 -- so a
 * released PTT left the transmitter generating silent slots for ever and the
 * transmission was ended from outside by the guard's 160 ms drain budget (or,
 * worse, by the 90 s air clock).  A call that cannot end by itself is the same
 * class of defect as a wait without a deadline: the radio depends on somebody
 * else to get out.
 */

#ifndef DMR_TX_PLAN_H
#define DMR_TX_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Strokes of dmrTxNextSlot(). */
#define DMRTX_NOTHING     0u  /**< silence: nothing in this slot            */
#define DMRTX_HEADER      1u  /**< a voice LC header                        */
#define DMRTX_VOICE       2u  /**< a voice burst, three AMBE+2 frames       */
#define DMRTX_TERMINATOR  3u  /**< the terminator that closes the call      */
#define DMRTX_DONE        4u  /**< the terminator and the tail went out     */

/** Voice bursts of a superframe (A..F). */
#define DMRTX_SUPERFRAME  6u

/** The counters of one call, as the mode keeps them. */
typedef struct
{
    uint8_t stage;   /**< 0 headers, 1 voice, 2 terminator, 3 tail        */
    uint8_t headers; /**< voice LC headers still to send                  */
    uint8_t bursts;  /**< voice bursts sent, modulo DMRTX_SUPERFRAME      */
    uint8_t tail;    /**< tail slots sent after the terminator            */
    uint8_t slots;   /**< 30 ms slots seen, for the TDMA slot parity      */
} dmrTxCall_t;

/** Start a call: `headers` voice LC headers first (0 for a call without). */
static inline void dmrTxCall_begin(dmrTxCall_t *c, uint8_t headers)
{
    c->stage   = 0;
    c->headers = headers;
    c->bursts  = 0;
    c->tail    = 0;
    c->slots   = 0;
}

/**
 * The next slot of the call.
 *
 * @param c:          the call's counters, advanced in place.
 * @param ptt:        the PTT level right now.
 * @param ending:     the call is already closing (the PTT went up, or the mode
 *                    was told to stop): sticky, so a bounce does not reopen it.
 * @param timeslot:   1 or 2, the timeslot the call was set up on.
 * @param tailSlots:  slots of silence after the terminator, so the last burst
 *                    is not cut by the PA going down.
 * @return one of DMRTX_*.
 */
static inline unsigned dmrTxNextSlot(dmrTxCall_t *c, bool ptt, bool ending,
                                     uint8_t timeslot, uint8_t tailSlots)
{
    /*
     * One slot of every TDMA frame is ours: the 60 ms frame holds two 30 ms
     * slots and the timeslot says which.  The parity comes from the slot
     * counter, which advances on every slot including the silent ones.
     */
    const uint8_t parity = (uint8_t)((timeslot == 2u) ? 1u : 0u);
    const bool ourSlot = ((c->slots++ & 1u) == parity);

    if (!ptt)
        ending = true;

    switch (c->stage) {
        case 0:
            if (c->headers > 0) {
                if (!ourSlot)
                    return DMRTX_NOTHING;
                c->headers--;
                if (c->headers == 0)
                    c->stage = 1;
                return DMRTX_HEADER;
            }
            /* No headers at all: straight to voice, without losing the slot */
            c->stage = 1;
            return dmrTxNextSlot(c, ptt, ending, timeslot, tailSlots);

        case 1:
            if (ending) {
                /* The PTT is up: the next slot in our timeslot is the
                 * terminator.  This is the transition that did not exist. */
                c->stage = 2;
                return DMRTX_NOTHING;
            }
            if (!ourSlot)
                return DMRTX_NOTHING;
            c->bursts = (uint8_t)((c->bursts + 1u) % DMRTX_SUPERFRAME);
            return DMRTX_VOICE;

        case 2:
            if (!ourSlot)
                return DMRTX_NOTHING;
            c->stage = 3;
            c->tail  = 0;
            return DMRTX_TERMINATOR;

        default:
            if (++c->tail >= tailSlots)
                return DMRTX_DONE;
            return DMRTX_NOTHING;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DMR_TX_PLAN_H */
