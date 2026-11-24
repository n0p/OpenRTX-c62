/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_TX_FSM_H
#define OPENRTX_DMR_TX_FSM_H

/**
 * @file DmrTxFsm.h
 * @brief Transmit stage machine of the DMR mode, as a pure function.
 *
 * One call per 30 ms timeslot decides what the slot carries and whether it is
 * the last one of the transmission.  It is kept apart from OpMode_DMR.cpp --
 * it needs no audio stream, no radio and no vocoder -- so the host tests can
 * walk every state and every PTT condition, and so the exit path is one piece
 * of code instead of a rule spread over the slot renderer.
 *
 * States and transitions (a slot is 30 ms; "PTT" is the live key state).  The
 * slot rendered *in* a state carries that state's burst, so a call on the air
 * is: LC header, voice/empty alternating, terminator, empty tail.
 *
 *   LEAD ──PTT down and >= 1 frame queued──▶ LC_HEADER ──▶ VOICE
 *     │                                                      │
 *     │ PTT up before anything was announced                 │ PTT up
 *     ▼                                                      ▼
 *   done (one empty slot)                              TERMINATOR ──▶ TAIL ──▶ done
 *
 * VOICE stays in VOICE while the PTT is down, emitting a voice burst every
 * other slot (the slot in between is an empty one: TDMA phase).  TAIL renders
 * the empty slot whose start the terminator's PA ramp already descended into.
 *
 * Safety exit: a state machine that only leaves TX when some condition inside
 * a state becomes true can be trapped by a condition that never becomes true --
 * releasing the PTT during the lead time, with an empty vocoder queue, was
 * exactly that: the PA stayed keyed and the radio never returned to receive.
 * Therefore the slot counter is checked first: DMR_TX_MAX_SLOTS_AFTER_PTT
 * slots after the PTT was seen released the transmission ends from whatever
 * state it is in, and the caller unkeys.  Four slots are 120 ms: the longest
 * legitimate tail (terminator + tail slot) is two.
 */

#include <stdbool.h>

#include "protocols/DMR/DmrTx.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Stages of the transmit call. */
enum dmrTxStage {
    DMR_TX_STAGE_LEAD = 0,  ///< PA up, waiting for the first vocoder frame
    DMR_TX_STAGE_LC_HEADER, ///< Voice LC header announces the call
    DMR_TX_STAGE_VOICE,     ///< Voice bursts, one every other slot
    DMR_TX_STAGE_TERMINATOR,///< Terminator ends the call
    DMR_TX_STAGE_TAIL       ///< One empty slot, then unkey
};

/**
 * Slots the transmitter is allowed to keep running after the PTT was seen
 * released.  Terminator plus tail slot is two; the two extra slots are the
 * margin that turns "the state machine forgot to leave" into "one extra
 * 60 ms of carrier".
 */
#define DMR_TX_MAX_SLOTS_AFTER_PTT 4

/**
 * Slots the lead stage may wait for the first vocoder frame before giving up:
 * 33 slots are one second.  With a dead microphone or a failing encoder the
 * queue never fills, and the operator would otherwise hold a keyed carrier
 * with nothing on it for as long as the PTT is pressed.
 */
#define DMR_TX_MAX_LEAD_SLOTS 33

/** State of one transmission. */
struct dmrTxFsm {
    enum dmrTxStage stage;  ///< Current stage.
    bool burstSlot;         ///< Next VOICE slot carries a burst (TDMA phase).
    unsigned slotsAfterPtt; ///< Slots rendered since the PTT was released.
    unsigned leadSlots;     ///< Slots spent waiting for the first frame.
};

/** What the caller has to render, and whether the transmission ends here. */
struct dmrTxStep {
    enum dmrTxBurst kind;  ///< Burst to render into this slot.
    uint8_t data;          ///< DMR_TXB_DATA only: enum dmrTxDataBurst.
    bool finished;         ///< This is the last slot: unkey after rendering it.
};

/**
 * Reset a state machine for a new transmission.
 *
 * @param fsm: state machine to initialise.
 */
static inline void dmrTxFsm_init(struct dmrTxFsm *fsm)
{
    fsm->stage = DMR_TX_STAGE_LEAD;
    fsm->burstSlot = true;
    fsm->slotsAfterPtt = 0;
    fsm->leadSlots = 0;
}

/**
 * Advance the state machine by one timeslot.
 *
 * @param fsm: state machine.
 * @param ptt: current PTT state.
 * @param haveFrames: true if at least one vocoder frame is queued.
 * @return burst to render and whether the transmission ends with this slot.
 */
static inline struct dmrTxStep dmrTxFsm_next(struct dmrTxFsm *fsm, bool ptt,
                                             bool haveFrames)
{
    struct dmrTxStep step = { DMR_TXB_EMPTY, 0, false };

    if (ptt)
        fsm->slotsAfterPtt = 0;
    else if (fsm->slotsAfterPtt < DMR_TX_MAX_SLOTS_AFTER_PTT + 1)
        fsm->slotsAfterPtt += 1;

    /* Safety exit, checked before any stage: it never depends on the stage. */
    if (fsm->slotsAfterPtt > DMR_TX_MAX_SLOTS_AFTER_PTT) {
        fsm->stage = DMR_TX_STAGE_TAIL;
        step.finished = true;
        return step;
    }

    switch (fsm->stage) {
        case DMR_TX_STAGE_LEAD:
            /*
             * The PA is up but the call has not been announced: the LC header
             * opens a call and the first voice burst follows one slot later,
             * so keying it with an empty queue would put a header and then
             * silence on the air.  Wait for one frame; if the operator let go
             * in the meantime, end here: nothing was announced and the empty
             * slot of this very slot is the last thing transmitted.
             */
            if (ptt == false) {
                step.finished = true;
            } else if (haveFrames) {
                fsm->leadSlots = 0;
                fsm->stage = DMR_TX_STAGE_LC_HEADER;
            } else if (++fsm->leadSlots > DMR_TX_MAX_LEAD_SLOTS) {
                /* One second without a frame: the vocoder is not producing */
                step.finished = true;
            }
            break;

        case DMR_TX_STAGE_LC_HEADER:
            step.kind = DMR_TXB_VOICE_LC;
            fsm->stage = DMR_TX_STAGE_VOICE;
            break;

        case DMR_TX_STAGE_VOICE:
            /*
             * One burst every other slot: a burst carries 60 ms of speech,
             * which is what the vocoder produces in those 60 ms.  The slot in
             * between is silence, not a shorter burst; a handheld that keys the
             * other slot would trample the station that owns it.
             */
            if (fsm->burstSlot) {
                step.kind = DMR_TXB_VOICE;
                fsm->burstSlot = false;
            } else {
                step.kind = DMR_TXB_EMPTY;
                fsm->burstSlot = true;
            }
            if (ptt == false)
                fsm->stage = DMR_TX_STAGE_TERMINATOR;
            break;

        case DMR_TX_STAGE_TERMINATOR:
            step.kind = DMR_TXB_TERMINATOR;
            fsm->stage = DMR_TX_STAGE_TAIL;
            break;

        case DMR_TX_STAGE_TAIL:
            /* One empty slot after the terminator: it carries the tail of the
             * last pulse and the terminator's PA ramp landed inside its guard. */
            step.kind = DMR_TXB_EMPTY;
            step.finished = true;
            break;

        default:
            /* Not reachable; treat it as the end rather than as a new call. */
            fsm->stage = DMR_TX_STAGE_TAIL;
            step.finished = true;
            break;
    }

    return step;
}

/* ------------------------------------------------------------ data call --- */

/**
 * Which burst a DMR_TXB_DATA slot of a short message carries.  The caller maps
 * this to the packet: the header announces the blocks, each block is one
 * burst, and the TD_LC closes the call.
 */
enum dmrTxDataBurst {
    DMR_TXDATA_HEADER = 0, ///< Data header of the packet
    DMR_TXDATA_BLOCK,      ///< Next rate 1/2 block of the packet
    DMR_TXDATA_LC          ///< Terminator of the data call (TD_LC)
};

/** Phases of a short message transmission. */
enum dmrTxDataStage {
    DMR_TXD_HEADER = 0, ///< Data header, announces the payload
    DMR_TXD_BLOCKS,     ///< Rate 1/2 blocks, one per burst slot
    DMR_TXD_END,        ///< TD_LC that closes the data call
    DMR_TXD_TAIL,       ///< One empty slot, then unkey
    DMR_TXD_DONE        ///< Nothing left to send
};

/**
 * Slots a data call may take before it is cut: a header, up to
 * DMR_SMS_BLOCKS_MAX (30) blocks, the TD_LC and the tail slot are 33 bursts
 * and as many empty slots between them, 66 slots.  Two more are the margin.
 */
#define DMR_TX_DATA_MAX_SLOTS 68

/** State of one short message transmission. */
struct dmrTxData {
    enum dmrTxDataStage stage; ///< Current phase.
    unsigned blocks;           ///< Blocks still to send.
    unsigned slots;            ///< Slots rendered, the safety counter.
    bool burstSlot;            ///< This slot carries a burst (TDMA phase).
};

/**
 * Reset the data machine for a new message.
 *
 * @param data: state machine to initialise.
 * @param blocks: rate 1/2 blocks the packet carries, header excluded.
 */
static inline void dmrTxData_init(struct dmrTxData *data, unsigned blocks)
{
    data->stage = DMR_TXD_HEADER;
    data->blocks = blocks;
    data->slots = 0;
    data->burstSlot = true;
}

/**
 * Advance the data machine by one timeslot.  There is no PTT here: a short
 * message runs to its end, one burst per TDMA frame, and the slot counter cuts
 * it even if a phase were to repeat (the same reason the voice machine has
 * one: a transmission that cannot end is a keyed PA).
 *
 * @param data: state machine.
 * @return burst to render and whether the transmission ends with this slot.
 */
static inline struct dmrTxStep dmrTxData_next(struct dmrTxData *data)
{
    struct dmrTxStep step = { DMR_TXB_EMPTY, 0, false };

    if ((data->stage == DMR_TXD_DONE) || (data->slots >= DMR_TX_DATA_MAX_SLOTS)) {
        data->stage = DMR_TXD_DONE;
        step.finished = true;
        return step;
    }
    data->slots++;

    /* One burst every other slot: the TDMA phase of the call */
    if (data->burstSlot == false) {
        data->burstSlot = true;
        return step;
    }
    data->burstSlot = false;

    switch (data->stage) {
        case DMR_TXD_HEADER:
            step.kind = DMR_TXB_DATA;
            step.data = DMR_TXDATA_HEADER;
            data->stage = (data->blocks > 0) ? DMR_TXD_BLOCKS : DMR_TXD_END;
            break;

        case DMR_TXD_BLOCKS:
            step.kind = DMR_TXB_DATA;
            step.data = DMR_TXDATA_BLOCK;
            data->blocks--;
            if (data->blocks == 0)
                data->stage = DMR_TXD_END;
            break;

        case DMR_TXD_END:
            step.kind = DMR_TXB_DATA;
            step.data = DMR_TXDATA_LC;
            data->stage = DMR_TXD_TAIL;
            break;

        case DMR_TXD_TAIL:
            step.finished = true;
            data->stage = DMR_TXD_DONE;
            break;

        default:
            step.finished = true;
            data->stage = DMR_TXD_DONE;
            break;
    }

    return step;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_TX_FSM_H */
