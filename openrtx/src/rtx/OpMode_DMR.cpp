/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DMR/DmrFec.h"
#include "protocols/DMR/DmrPhy.h"
#include "protocols/DMR/DmrSpeech.h"
#include "protocols/DMR/DmrStatus.h"
#include "rtx/OpMode_DMR.hpp"
#include "core/utils.h"

#ifdef CONFIG_DMR_SMS
#include "core/messaging.h"
#endif

#ifdef CONFIG_DMR_TX
#include "core/audio_latency.h"
#include "protocols/DMR/DmrRpt.h"
#include "protocols/DMR/DmrTx.h"
#include "protocols/DMR/DmrVocoder.h"
#endif

#include "core/state.h"

/* 30 ms of baseband at 48 kHz: one burst arrives every 60 ms per timeslot */
#define HALF_SAMPLES 1440

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

#ifdef CONFIG_DMR_TX
/* 8 kHz PCM samples the vocoder eats per frame */
#define VOCODER_PCM_SAMPLES 160

/* Frames the queue is allowed to take per slot: 1,5 is the average rate, two
 * keeps up and still leaves time for the modulator inside 30 ms */
#define VOCODER_FRAMES_PER_SLOT 2
#endif

static int16_t baseband[2 * HALF_SAMPLES];
#ifdef CONFIG_DMR_TX
/*
 * Microphone PCM.  Same rule as the baseband buffer below: the stream is given
 * the whole double buffer and hands out 160-sample halves, so one half is
 * exactly the 20 ms frame the vocoder wants.
 */
static int16_t micPcm[2 * VOCODER_PCM_SAMPLES];
#endif

/**
 * Hand a decoded burst to the status block, to the speech queue and, when it
 * is a data burst of a short message, to the reassembler of its timeslot.
 * Runs in the rtx thread: it must not decode audio here.
 */
void OpMode_DMR::burstTrampoline(const struct dmrBurst *burst, void *user)
{
    static_cast<OpMode_DMR *>(user)->onBurst(burst);
}

void OpMode_DMR::onBurst(const struct dmrBurst *burst)
{
    dmrStatus_burst(burst);

#ifdef CONFIG_DMR_TX
    /*
     * Repeater bookkeeping, before anything is handed to the decoder: the
     * colour code and the timeslot of the last outbound burst feed the gate,
     * the position of its timeslot feeds the frame phase, and a burst while
     * the activation machine is listening is the answer it was waiting for.
     */
    rptObserve(burst);
#endif

    if (burst->voice) {
        if (burst->frameCount > 0)
            dmrSpeech_push(burst->frame, burst->frameCount);
        return;
    }

#ifdef CONFIG_DMR_SMS
    /*
     * Short messages ride in data calls: a data header announces how many
     * rate 1/2 blocks follow.  Voice LC headers, terminators, CSBK, idle and
     * the trunking UDT are not ours.
     */
    if ((!burst->slotTypeValid) || ((burst->dataType != DMR_DT_DATA_HEADER)
                                    && (burst->dataType != DMR_DT_RATE_12)))
        return;

    const unsigned slot = burst->timeslot2 ? 1u : 0u;
    struct dmrSmsRxResult res;

    if (dmrSmsRx_feed(&smsRx[slot], burst->dataType, burst->lc, &res) == 0)
        return;

    if (res.response) {
        onResponse(&res.msg);
        return;
    }
    if (res.message)
        onMessage(&res.msg, res.headerWantsResponse);
#endif
}

#ifdef CONFIG_DMR_SMS
void OpMode_DMR::onMessage(const struct dmrSmsMessage *msg, bool wantsResponse)
{
    uint32_t now = (uint32_t)getTick();

    /*
     * A packet whose CRC-32 does not close is not a message: it is what a
     * burst lost on the air looks like.  Never show it, never acknowledge it
     * as delivered; the sender retries.
     */
    if (!msg->crcOk) {
        smsDropped++;
        return;
    }

    /*
     * A TMS acknowledgement (record type 0x1F) confirms a message we sent; it
     * carries no text and is not shown.
     */
    if (msg->tmsAck) {
        messaging_confirmed(0, msg->src, now);
        return;
    }

    if (msg->app == DMR_SMS_APP_RAW) {
        smsDropped++;
        return;
    }

    messaging_received(MSG_TRANSPORT_DMR, msg->src, msg->dst, msg->text,
                       wantsResponse, now);
    smsReceived++;

    if (wantsResponse) {
        /* A delivery response is a C_RHEAD sent back to the peer. */
        queueResponse(msg->src, selfId, (uint8_t)msg->sap,
                      DMR_SMS_RESP_ACK_CLASS, DMR_SMS_RESP_TYPE_ACK, 0);
    }

    /* A TMS sender that asked for its application acknowledgement gets it. */
    if ((msg->app == DMR_SMS_APP_TMS) && msg->tmsAckReq && (msg->tmsSeq >= 0))
        queueTmsAck(msg->src, selfId, (uint8_t)msg->tmsSeq);
}

void OpMode_DMR::onResponse(const struct dmrSmsMessage *msg)
{
    /* Class 00 is the ACK: the peer has the message. */
    if ((msg->crcOk) && (msg->dpf == DMR_SMS_DPF_RESPONSE))
        messaging_confirmed(0, msg->src, (uint32_t)getTick());
}

struct OpMode_DMR::dmrAck *OpMode_DMR::freeAck(void)
{
    for (unsigned i = 0; i < DMR_SMS_ACK_QUEUE; i++) {
        if (!ackQueue[i].used) {
            ackQueue[i].used = true;
            ackQueue[i].blocks = 0;
            return &ackQueue[i];
        }
    }
    return NULL; /* no room: the peer asks again */
}

void OpMode_DMR::queueResponse(uint32_t dst, uint32_t src, uint8_t sap,
                               uint8_t respClass, uint8_t respType,
                               uint8_t status)
{
    struct dmrAck *ack = freeAck();

    if (ack == NULL)
        return;
    dmrSms_buildResponse(ack->header, dst, src, sap, respClass, respType,
                         status);
}

void OpMode_DMR::queueTmsAck(uint32_t dst, uint32_t src, uint8_t seq)
{
    struct dmrSmsEncode req;
    struct dmrSmsPacket pkt;
    struct dmrAck *ack = freeAck();

    if (ack == NULL)
        return;

    memset(&req, 0, sizeof(req));
    memset(&pkt, 0, sizeof(pkt));
    req.app = DMR_SMS_APP_TMS;
    req.src = src;
    req.dst = dst;
    req.tmsSeq = seq;
    if ((dmrSms_encodeTmsAck(&req, &pkt) != 0) || (pkt.blockCount < 1)) {
        ack->used = false;
        return;
    }
    memcpy(ack->header, pkt.header, sizeof(ack->header));
    memcpy(ack->data, pkt.blocks[0], sizeof(ack->data));
    ack->blocks = 1;
}
#endif /* CONFIG_DMR_SMS */

OpMode_DMR::OpMode_DMR()
    : startRx(false)
    , dataErrors(0)
    , settle(SETTLE_SAMPLES)
    , basebandPath(-1)
    , basebandId(-1)
#ifdef CONFIG_DMR_SMS
    , selfId(0)
    , smsReceived(0)
    , smsDropped(0)
#endif
#ifdef CONFIG_DMR_TX
    , txRunning(false)
    , txFirstBlock(false)
    , micPath(-1)
    , micId(-1)
    , txPath(-1)
    , txId(-1)
    , frameHead(0)
    , frameCount(0)
#ifdef CONFIG_DMR_SMS
    , txBurst(0)
    , txMsgId(0)
    , txIsData(false)
    , txAcking(false)
#endif
#endif
{
    txBlocked = false;

#ifdef CONFIG_DMR_SMS
    memset(&smsRx, 0, sizeof(smsRx));
    memset(&ackQueue, 0, sizeof(ackQueue));
#endif
#ifdef CONFIG_DMR_TX
    memset(&tx, 0, sizeof(tx));
    memset(frameQueue, 0, sizeof(frameQueue));
#ifdef CONFIG_DMR_SMS
    memset(&txPacket, 0, sizeof(txPacket));
    dmrTxData_init(&dataFsm, 0);
#endif
    dmrTxFsm_init(&fsm);

    dmrRpt_phaseReset(&rptPhase);
    memset(&rptPlan, 0, sizeof(rptPlan));
    memset(&rptWake, 0, sizeof(rptWake));
    memset(&rptSeen, 0, sizeof(rptSeen));
    rptSeen.colourCode = DMR_RPT_CC_UNKNOWN;
    memset(&rptCal, 0, sizeof(rptCal));
    rptGate = DMR_RPT_GATE_OK;
    rptState = 0;
#endif
}

OpMode_DMR::~OpMode_DMR()
{
    disable();
}

void OpMode_DMR::reset()
{
    settle = SETTLE_SAMPLES;
    dataErrors = 0;
    dmrPhy_reset();
    dmrStatus_reset();
}

void OpMode_DMR::enable()
{
    dmrPhy_init();
    reset();
    dmrSpeech_start();
    startRx = true;
    txBlocked = false;
}

void OpMode_DMR::disable()
{
    startRx = false;
#ifdef CONFIG_DMR_TX
    stopTx();
#endif
    /* Leaving the mode ends whatever transmission was open */
    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_MODE);
    stopSampling();
    dmrSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    dmrStatus_clear();
}

void OpMode_DMR::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   48000, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_DMR::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

bool OpMode_DMR::rxSquelchOpen()
{
    return dmrPhy_synced();
}

#ifdef CONFIG_DMR_TX
/**
 * Take up to three frames out of the queue, zero-filling the rest: a burst
 * always carries three, and the vocoder is slower than the slot clock when it
 * is the first burst of a call.
 */
unsigned OpMode_DMR::dequeueFrames(uint8_t frames[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES])
{
    unsigned taken = 0;

    memset(frames, 0, DMR_FRAMES_PER_BURST * DMR_AMBE_BYTES);

    while ((taken < DMR_FRAMES_PER_BURST) && (frameCount > 0)) {
        memcpy(frames[taken], frameQueue[frameHead], DMR_AMBE_BYTES);
        frameHead = (frameHead + 1) % DMR_TX_QUEUE_FRAMES;
        frameCount--;
        taken++;
    }

    return taken;
}

/**
 * The part both transmitters share: configure the chain for this channel and
 * open the baseband output stream.  The caller adds its own input, or none for
 * a short message, and unkeys through stopTx() on any failure.
 */
bool OpMode_DMR::startBaseband(const rtxStatus_t *status)
{
    struct dmrTxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.colourCode = status->dmrColourCode;
    cfg.sourceId = status->dmrSourceId;
    cfg.destId = status->dmrDestId;
    /*
     * The link control says whether the call is addressed to a talkgroup or to
     * a single subscriber (TS 102 361-1 clause 9.3.6): a unit call is what the
     * destination type USER asks for, anything else is a group call, which is
     * what every DMR network this radio is likely to meet expects.
     */
    cfg.group = (status->dmrDestType != DMR_DEST_USER);
    cfg.timeslot = (status->dmrTimeslot == 2) ? 1 : 0;
    cfg.layout = DMR_TX_LAYOUT_MS;
    cfg.ramp = true;
    cfg.innerLevel = DMR_TX_INNER_LEVEL;
    dmrTx_init(&tx, &cfg);

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0)
        return false;

    /*
     * The stream gets the whole double buffer: 2 * DMR_TX_SLOT_SAMPLES is what
     * BUF_CIRC_DOUBLE expects, and half of it is exactly one 30 ms slot.
     */
    txId = audioStream_start(txPath, txSamples, 2 * DMR_TX_SLOT_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    return (txId >= 0);
}

/* ---------------------------------------------------------------- repeater --- */

/*
 * Repeater operation, the four pieces of it that need the radio:
 *
 *   rptObserve()      what the outbound bursts say (colour code, timeslot,
 *                     frame phase, echo)
 *   rptAccessStep()   the wake machine: gate, BS_Dwn_Act, listen, decide
 *   rptCallTiming()   the plan: which slot parity and which sub-slot delay
 *   putSlot()         the delay line that turns the plan into the stream
 *
 * Everything that decides is in DmrRpt.c and host-tested; this is the glue,
 * and it is written so that every exit leaves the radio receiving.
 */

/**
 * The audio calibration does not have buffers of its own: it runs from
 * receive, with the receiver stopped and the PA cold, so it borrows the two
 * arrays the mode already has.  480 samples are 10 ms at 48 kHz and exactly one
 * marker; the capture is 50 ms, which holds the stream latency (30 ms), the
 * hardware one and a few milliseconds of acoustic path with room to spare.
 */
#define CAL_BLOCK_SAMPLES DMR_LAT_MARKER_SAMPLES
#define CAL_CAPTURE_SAMPLES 2400

/** Repeater states, for the UI and the traces. */
enum rptUiState {
    RPT_UI_IDLE = 0,   ///< Nothing going on
    RPT_UI_WAKING,     ///< BS_Dwn_Act sent, waiting for the outbound
    RPT_UI_ALIGNED,    ///< Call on the repeater's grid
    RPT_UI_FREE,       ///< Call without alignment
    RPT_UI_REFUSED,    ///< Colour code, timeslot or no answer: no call
    RPT_UI_UNFIT,      ///< The latency is not known well enough to align
    RPT_UI_CAL,        ///< Calibration pass running
    RPT_UI_CAL_DONE,   ///< ... and it found a value
    RPT_UI_CAL_FAIL    ///< ... and it did not
};

/** Samples of delay between the baseband stream and the air. */
static int32_t rptLatencySamples(const rtxStatus_t *status)
{
    int32_t us = status->audioLatencyUs;

    if (us < 0)
        us = 0;

    return DMR_RPT_STREAM_LATENCY + DMR_RPT_US_TO_SAMPLES(us);
}

uint8_t OpMode_DMR::rptTimeslot(const rtxStatus_t *status) const
{
    /* A channel that follows the repeater takes the slot it last heard */
    if (status->dmrTsFollow && (rptSeen.timeslot != 0))
        return rptSeen.timeslot;

    return (status->dmrTimeslot == 2) ? 2 : 1;
}

void OpMode_DMR::rptSetState(uint8_t state)
{
    rptState = state;
    dmrStatus_repeater((int8_t)state, rptPhase.ppm, rptPhase.quality,
                       rptAligned ? rptShift : 0);
}

/** Publish the gate verdict, which the quick menu and the remote show. */
void OpMode_DMR::rptSetGate(int8_t gate)
{
    rptGate = gate;
    dmrStatus_repeaterGate(gate);
}

int16_t *OpMode_DMR::rptRenderTarget(void)
{
    return txSamples + (rptRenderHigh ? DMR_TX_SLOT_SAMPLES : 0);
}

/**
 * Hand one rendered slot to the baseband stream without touching the transmit
 * guard: the activation burst and the calibration bursts are short, bounded
 * and driven by a state machine of their own.
 */
bool OpMode_DMR::pushSlot(const int16_t *slot)
{
    stream_sample_t *idle = outputStream_getIdleBuffer(txId);

    if ((idle == NULL) || (slot == NULL))
        return false;

    memcpy(idle, slot, DMR_TX_SLOT_SAMPLES * sizeof(stream_sample_t));
    outputStream_sync(txId, true);

    return true;
}

void OpMode_DMR::rptObserve(const struct dmrBurst *burst)
{
    if (burst->slotTypeValid) {
        rptSeen.colourCode = burst->colourCode;

        if (burst->cachValid) {
            rptSeen.timeslot = burst->timeslot2 ? 2 : 1;
            rptSeen.busy = burst->accessType != 0;
        }
    }

    const uint32_t now = (uint32_t)getTick();

    /*
     * The frame phase comes from outbound bursts only: the CACH of a base
     * station tells which of the two slots the burst sits in, and a coasted
     * burst has no independent timing of its own (it was decoded on the grid,
     * so feeding it back would just confirm the grid).
     */
    if (rptTracking && dmr_sync_is_bs(burst->sync) && burst->cachValid
        && (!burst->coasted) && burst->slotTypeValid) {
        const int32_t ts1 = (int32_t)burst->slotSample
                          - (burst->timeslot2 ? DMR_RPT_SLOT_SAMPLES : 0);

        dmrRpt_phaseFeed(&rptPhase, ts1, now);
    }

    /* The activation machine listens for the outbound channel to come up */
    if (dmrRptWake_active(&rptWake) && dmr_sync_is_bs(burst->sync)
        && burst->slotTypeValid)
        dmrRptWake_answer(&rptWake);

    /* The calibration pass listens for its own echo */
    if (dmrRptCal_active(&rptCal) && dmr_sync_is_bs(burst->sync)
        && burst->slotTypeValid)
        dmrRptCal_heard(&rptCal, (uint32_t)burst->slotSample,
                        burst->crcChecked && burst->crcOk);
}

/**
 * Transmit the BS_Dwn_Act burst: a slot of PA settle, the CSBK in the slot of
 * the mode, a slot of tail.  Leaves the radio receiving.
 */
bool OpMode_DMR::rptTransmitWake(const rtxStatus_t *status)
{
    uint8_t info[DMR_CSBK_BITS];
    const uint8_t ts = rptTimeslot(status);
    const int at = (ts == 2) ? 1 : 0;

    stopSampling();
    radio_disableRtx();

    if (!startBaseband(status)) {
        stopTx();
        startRx = true;
        return false;
    }

    dmrRpt_csbkDownlinkActivity(info, DMR_RPT_ADDR_ANY, status->dmrSourceId);

    rptRenderHigh = false;
    rptShift = 0;

    for (int i = 0; i < 4; i++) {
        int16_t *out = rptRenderTarget();

        if (i == at)
            dmrTx_dataSlot(&tx, (uint8_t)DMR_DT_CSBK, info, out);
        else
            dmrTx_slot(&tx, DMR_TXB_EMPTY, NULL, out);
        pushSlot(out);
    }

    stopTx();        /* unkey, release the paths, back to half duplex */
    startRx = true;  /* the caller's common path reopens the receiver */
    dmrRptWake_sent(&rptWake, (uint32_t)getTick());
    rptSetState(RPT_UI_WAKING);

    return true;
}

/**
 * One pass of the repeater access: the gate, then the wake machine, then the
 * call.  Called once per receive block while the PTT is down on a repeater
 * channel.
 *
 * @return true when the radio has been taken (transmitting or refused), false
 *         to let the caller keep receiving.
 */
bool OpMode_DMR::rptAccessStep(rtxStatus_t *rtx)
{
    const uint32_t now = (uint32_t)getTick();

    struct dmrRptChannel ch;

    ch.colourCode = rtx->dmrColourCode;
    ch.timeslot = rtx->dmrTsFollow ? 0 : rtx->dmrTimeslot;
    ch.polite = true;

    if (!rptAccess) {
        /*
         * First pass with the PTT down.  The gate comes first: a repeater that
         * speaks another colour code is not ours, and keying at it is
         * interference whatever the rest of the plan says.
         */
        rptAccess = true;
        rptSetGate((int8_t)dmrRpt_gate(&ch, &rptSeen));

        if (rptGate != DMR_RPT_GATE_OK) {
            rptSetState(RPT_UI_REFUSED);
            txBlocked = true; /* not before the PTT is released */
            return true;
        }

        /*
         * A repeater that is already transmitting, with our colour code, and
         * that the receiver is locked to does not need to be woken
         * (TS 102 361-1 clause 5.2.1.3).
         */
        if (dmrPhy_synced() && (rptSeen.colourCode == rtx->dmrColourCode))
            dmrRptWake_skip(&rptWake);
        else
            dmrRptWake_start(&rptWake, DMR_RPT_WAKE_RETRIES, now);
    }

    switch (dmrRptWake_next(&rptWake, now, (enum dmrRptNoAnswer)rtx->dmrNoAnswer)) {
        case DMR_RPT_WAKE_SEND:
            rptTransmitWake(rtx);
            return false;

        case DMR_RPT_WAKE_CALL:
        case DMR_RPT_WAKE_FREERUN: {
            /* The answer may come from another colour code: gate again */
            rptSetGate((int8_t)dmrRpt_gate(&ch, &rptSeen));

            if (rptGate != DMR_RPT_GATE_OK) {
                rptSetState(RPT_UI_REFUSED);
                txBlocked = true;
                rptAccess = false;
                return true;
            }

            stopSampling();
            radio_disableRtx();

            if (startTx(rtx)) {
                rtx->opStatus = TX;
                return true;
            }

            startRx = true;
            rptAccess = false;
            return true;
        }

        case DMR_RPT_WAKE_ABORT:
            rptSetState(RPT_UI_REFUSED);
            txBlocked = true;
            rptAccess = false;
            return true;

        default:
            return false; /* still listening */
    }
}

/**
 * Fill the plan of a call and configure the stage machine with it.
 *
 * The plan is what turns "the repeater's TS1 is at sample p" into "start the
 * burst in the first (or second) slot rendered, delayed by n samples".
 */
void OpMode_DMR::rptCallTiming(const rtxStatus_t *status, uint8_t timeslot)
{
    memset(&rptPlan, 0, sizeof(rptPlan));
    rptPlan.burstFirst = (timeslot == 2) ? 2 : 1;
    rptPlan.leadPairs = (DMR_RPT_LEAD_SLOTS + 1) / 2;

    rptAligned = false;

    if (status->dmrRepeater) {
        const int32_t spread = DMR_RPT_US_TO_SAMPLES(
                                   status->audioLatencySpreadUs);
        int32_t margin = 0;

        /*
         * Refuse to pretend: if the latency is not known well enough for the
         * drift of the transmission, the burst would land outside the slot and
         * the call would fail on the air for a reason nobody can see.
         */
        if (!dmrRpt_fits(spread, 60000, rptPhase.ppm, &margin)) {
            rptSetState(RPT_UI_UNFIT);
        } else {
            dmrRpt_plan(&rptPhase, timeslot, (uint32_t)getTick(),
                        rptLatencySamples(status), DMR_RPT_LEAD_SLOTS,
                        &rptPlan);
            rptAligned = rptPlan.aligned;
        }
    }

    rptShiftBase = rptPlan.aligned ? (uint8_t)rptPlan.shift : 0;
    rptShift = rptShiftBase;
    rptPreSlots = (uint8_t)(rptPlan.leadPairs * 2);
    rptRenderHigh = false;
    rptCallTick = (uint32_t)getTick();

    /* The previous "slot" the delay line would read is silence, not garbage */
    memset(txSamples + DMR_TX_SLOT_SAMPLES, 0,
           DMR_TX_SLOT_SAMPLES * sizeof(stream_sample_t));

    if (!status->dmrRepeater)
        rptSetState(RPT_UI_IDLE);
    else if (rptAligned)
        rptSetState(RPT_UI_ALIGNED);
    else if (rptState != RPT_UI_UNFIT)
        rptSetState(RPT_UI_FREE);
}

/**
 * Track the drift during the call: the delay moves one sample at a time
 * towards what the fitted ppm asks for.
 */
void OpMode_DMR::rptDriftSlew(void)
{
    if (!rptAligned)
        return;

    const int32_t elapsed = (int32_t)((uint32_t)getTick() - rptCallTick);
    int32_t target = rptShiftBase
                   - (int32_t)(((int64_t)rptPhase.ppm * elapsed
                                * DMR_RPT_SAMPLES_PER_MS) / 1000000);

    if (target < 0)
        target += DMR_RPT_SLOT_SAMPLES;
    if (target > DMR_RPT_SLOT_SAMPLES - 1)
        target = DMR_RPT_SLOT_SAMPLES - 1;

    rptShift = (uint8_t)dmrRpt_txShiftStep(rptShift, target, 1);
    dmrStatus_repeater((int8_t)rptState, rptPhase.ppm, rptPhase.quality, rptShift);
}

/**
 * Transmit the synthetic call of one calibration attempt: the voice LC header
 * and two bursts of digital silence, at the offset of this attempt.  Nothing
 * of it comes from the microphone, so two attempts are the same on the air.
 */
bool OpMode_DMR::rptTransmitCal(const rtxStatus_t *status)
{
    static const uint8_t silence[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES] = { { 0 } };
    const uint8_t ts = rptTimeslot(status);

    stopSampling();
    radio_disableRtx();

    if (!startBaseband(status)) {
        stopTx();
        startRx = true;
        return false;
    }

    rptCallTiming(status, ts);

    /* the attempt's offset is an extra delay, folded into the plan's shift */
    rptShift = (uint8_t)((rptShiftBase + dmrRptCal_offset(&rptCal)
                          + DMR_RPT_SLOT_SAMPLES) % DMR_RPT_SLOT_SAMPLES);
    rptShiftBase = rptShift;
    rptAligned = true;

    for (int i = 0; i < DMR_RPT_CAL_SLOTS; i++) {
        int16_t *out = rptRenderTarget();

        if (i == ((ts == 2) ? 1 : 0))
            dmrTx_slot(&tx, DMR_TXB_VOICE_LC, silence, out);
        else if (i > ((ts == 2) ? 1 : 0))
            dmrTx_slot(&tx, DMR_TXB_VOICE, silence, out);
        else
            dmrTx_slot(&tx, DMR_TXB_EMPTY, NULL, out);
        pushSlot(out);
    }

    stopTx();
    startRx = true;

    /*
     * Where the echo should appear: the repeater's own grid for the slot we
     * use.  A parrot re-times to it, so the delay comes out near zero; a
     * repeater that relays with a fixed turnaround shows the alignment error
     * plus that turnaround, which is what the trace reports.
     */
    const uint32_t now = (uint32_t)getTick();
    int32_t grid = dmrRpt_phaseAt(&rptPhase, now);
    uint32_t expected = 0;

    /*
     * Both sides of the comparison are reduced to one 60 ms frame -- the grid
     * comes out of the tracker modulo the frame and the machine reduces the
     * difference again -- so the delay the pass reports is the sub-slot
     * residual of the echo against the repeater's own timing.
     */
    if (grid >= 0)
        expected = (uint32_t)grid
                 + ((ts == 2) ? DMR_RPT_SLOT_SAMPLES : 0);

    dmrRptCal_sent(&rptCal, now, expected);
    rptSetState(RPT_UI_CAL);

    return true;
}

/** Apply the result of a finished calibration pass (or keep what there was). */
void OpMode_DMR::rptCalFinish(rtxStatus_t *rtx)
{
    struct dmrRptCalResult res;

    rtx->rptCalRequest = 0;

    if (!dmrRptCal_result(&rptCal, &res) || (!res.store)) {
        if (dmrRptCal_result(&rptCal, &res))
            dmrStatus_repeaterCal((int)res.valid, res.offset, res.spread);
        rptSetState(RPT_UI_CAL_FAIL);
        return;
    }

    /*
     * The winning offset is a delay, so it adds to the latency the plan
     * assumed.  One persistent number, in microseconds, exactly the one the
     * laboratory measurement writes.
     */
    const int32_t us = DMR_RPT_SAMPLES_TO_US(res.offset);
    int32_t latency = rtx->audioLatencyUs + us;

    if (latency < 0)
        latency = 0;

    rtx->audioLatencyUs = latency;
    rtx->audioLatencySpreadUs = (int16_t)DMR_RPT_SAMPLES_TO_US(res.spread + 1);

    pthread_mutex_lock(&state_mutex);
    state.settings.audio_latency_us = rtx->audioLatencyUs;
    state.settings.audio_latency_spread_us = rtx->audioLatencySpreadUs;
    pthread_mutex_unlock(&state_mutex);

    dmrStatus_repeaterCal((int)res.valid, res.offset, res.spread);
    rptSetState(RPT_UI_CAL_DONE);
}

/** One step of the calibration pass; @return true if it took the radio. */
bool OpMode_DMR::rptCalStep(rtxStatus_t *rtx)
{
    switch (dmrRptCal_next(&rptCal, (uint32_t)getTick())) {
        case DMR_RPT_CAL_SEND:
            rptTransmitCal(rtx);
            return false;

        case DMR_RPT_CAL_FINISH:
            rptCalFinish(rtx);
            return false;

        case DMR_RPT_CAL_ABORT:
            rtx->rptCalRequest = 0;
            rptSetState(RPT_UI_CAL_FAIL);
            return false;

        default:
            return false;
    }
}

/**
 * Measure the audio path delay of this radio: play the chirp into the
 * loudspeaker and capture it with the microphone.  Needs a cable between the
 * two (or a quiet room with the radio on a table), which is what the menu
 * asks for.
 *
 * No buffer of its own: the transmit and receive baseband arrays are idle here
 * (receiver stopped, PA cold), and lending them keeps a 10 KiB measurement from
 * costing 10 KiB of .bss for the whole life of the radio.
 */
bool OpMode_DMR::audioCalibrate(rtxStatus_t *rtx)
{
    int16_t *out = txSamples;                       /* 2 * 480 samples */
    int16_t *in = txSamples + 2 * CAL_BLOCK_SAMPLES;
    int16_t *capture = baseband;                    /* 2400 of 2880 */

    rtx->audioCalRequest = 0;

    audioLatency_marker(out, 48000);
    memset(out + DMR_LAT_MARKER_SAMPLES, 0,
           CAL_BLOCK_SAMPLES * sizeof(int16_t));

    stopSampling();
    radio_disableRtx();

    pathId spkPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_TX);
    if (spkPath < 0) {
        startRx = true;
        return false;
    }

    streamId spk = audioStream_start(spkPath, (stream_sample_t *)out,
                                     2 * CAL_BLOCK_SAMPLES, 48000,
                                     STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    pathId micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_RX);
    streamId mic = (micPath < 0) ? -1
                                 : audioStream_start(micPath,
                                                     (stream_sample_t *)in,
                                                     2 * CAL_BLOCK_SAMPLES,
                                                     48000,
                                                     STREAM_INPUT
                                                     | BUF_CIRC_DOUBLE);

    bool done = false;

    if ((spk >= 0) && (mic >= 0)) {
        size_t got = 0;

        /* one block of marker out, everything the microphone gives back in */
        stream_sample_t *idle = outputStream_getIdleBuffer(spk);
        if (idle != NULL) {
            memcpy(idle, out, CAL_BLOCK_SAMPLES * sizeof(stream_sample_t));
            outputStream_sync(spk, true);
        }

        for (int i = 0; (i < 40) && (got < CAL_CAPTURE_SAMPLES); i++) {
            dataBlock_t block = inputStream_getData(mic);

            if ((block.data == NULL) || (block.len == 0))
                continue;

            size_t n = block.len;

            if (n > CAL_CAPTURE_SAMPLES - got)
                n = CAL_CAPTURE_SAMPLES - got;
            memcpy(capture + got, block.data, n * sizeof(int16_t));
            got += n;
        }

        int32_t us = 0;
        int32_t spread = 0;

        if ((got >= CAL_CAPTURE_SAMPLES)
            && audioLatency_measure(capture, CAL_CAPTURE_SAMPLES, 0, 48000,
                                   &us, &spread)) {
            rtx->audioLatencyUs = us;
            rtx->audioLatencySpreadUs = (int16_t)spread;

            pthread_mutex_lock(&state_mutex);
            state.settings.audio_latency_us = us;
            state.settings.audio_latency_spread_us = (int16_t)spread;
            pthread_mutex_unlock(&state_mutex);

            done = true;
        }
    }

    if (spk >= 0)
        audioStream_stop(spk);
    if (mic >= 0)
        audioStream_terminate(mic);
    audioPath_release(spkPath);
    if (micPath >= 0)
        audioPath_release(micPath);

    startRx = true;
    rptSetState(done ? RPT_UI_CAL_DONE : RPT_UI_CAL_FAIL);

    return done;
}

bool OpMode_DMR::startTx(const rtxStatus_t *status)
{
    struct txGuard *tx = rtx_getTxGuard();

    txg_enter(tx, (uint32_t)getTick());

    if (!dmrVocoder_available()) {
        /* No encoder: the chain would send three silent frames per burst */
        txg_streamsFailed(tx, (uint32_t)getTick());
        return false;
    }

    /*
     * A voice call needs somewhere to go and an identity to come from: with
     * the destination at "none" or the radio ID unset the link control would
     * announce a call to address 0 from address 0, which no receiver lists and
     * no operator can answer.  The mode stays in receive.
     */
    if (!settings_dmrDestValid(status->dmrDestType, status->dmrDestId) ||
        status->dmrDestType == DMR_DEST_NONE ||
        !settings_dmrIdValid(status->dmrSourceId))
        return false;

    if (!startBaseband(status)) {
        txg_streamsFailed(tx, (uint32_t)getTick());
        stopTx();
        return false;
    }

    micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if (micPath < 0) {
        stopTx();
        return false;
    }

    micId = audioStream_start(micPath, micPcm, 2 * VOCODER_PCM_SAMPLES, 8000,
                              STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (micId < 0) {
        stopTx();
        return false;
    }

    frameHead = 0;
    frameCount = 0;
    /*
     * Where the call starts: on a plain channel, the timeslot the operator
     * chose, counted from the moment the PA is keyed.  On a repeater channel,
     * the plan of DmrRpt.h decides instead -- which slot parity puts the burst
     * on the repeater's grid and by how many samples the baseband has to be
     * delayed to land inside it -- and the stage machine starts from that.
     */
    const uint8_t ts = rptTimeslot(status);

    rptCallTiming(status, ts);
    dmrTxFsm_initSlot(&fsm, rptPlan.burstFirst);
    txIsData = false;
    txAcking = false;
    txMsgId = 0;
    txFirstBlock = false;
    txRunning = true;

    /* Both streams are up: from here the supervisor expects slots */
    txg_streamsOpen(tx, (uint32_t)getTick());

    /* Last: the PA, the ramps of the first slot need it settled */
    radio_enableTx();
    txg_note(tx, TXG_EV_KEYED, (uint32_t)getTick());
    return true;
}

/**
 * Leave transmit: stop the streams, release the audio paths and unkey the PA.
 *
 * This is the only way out of a transmission, and it is unconditional.  The
 * two failures this fixes are a stream left half-open when one of the two
 * audioStream_start() calls above fails, and a state machine that stopped
 * calling this at all: the stages used to decide on their own when to leave,
 * so a release of the PTT in the lead time, before the vocoder had produced
 * the first frame, kept the PA keyed for ever.  The stage machines now always
 * end within a bounded number of slots, and whatever the caller does, the PA,
 * the BK4819 and the LED go back to receive here.
 */
void OpMode_DMR::stopTx()
{
    const bool wasRunning = txRunning;

    txRunning = false;
    txFirstBlock = false;
    dmrTxFsm_init(&fsm);
#ifdef CONFIG_DMR_SMS
    dmrTxData_init(&dataFsm, 0);
    txIsData = false;
    txAcking = false;
    txMsgId = 0;
#endif

    if (txId >= 0)
        audioStream_stop(txId);
    if (micId >= 0)
        audioStream_terminate(micId);
    if (txPath >= 0)
        audioPath_release(txPath);
    if (micPath >= 0)
        audioPath_release(micPath);

    txId = micId = -1;
    txPath = micPath = -1;

    /* radio_disableRtx() clears the PA, the TX LED and the BK4819 TX path */
    if (wasRunning)
        radio_disableRtx();

    /*
     * Unconditional: stopTx() is the only way out of a transmission, so this is
     * where every reason to end one lands, including the paths that failed
     * before the PA was ever keyed.
     */
    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_MODE);
}

/**
 * Hand the rendered slot to the baseband stream and wait for it to be played.
 *
 * The blocking sync is what keeps the slot cadence: 1440 samples at 48 kHz are
 * exactly 30 ms and the stream has two buffers, so the caller cannot get ahead
 * of the air. The first slot that makes it through is the moment samples are
 * on the air, and the supervisor's first-block deadline is measured from it.
 *
 * @return false if the stream is gone: cancelled by the supervisor, or failed.
 */
bool OpMode_DMR::putSlot()
{
    struct txGuard *tx = rtx_getTxGuard();

    stream_sample_t *idle = outputStream_getIdleBuffer(txId);
    if (idle == NULL)
        return false;

    /*
     * One half of the stream buffer is exactly one slot.  The two halves of
     * txSamples alternate as the rendering target, so the half that is not
     * being rendered holds the slot before this one: that is what the sample
     * delay of the repeater alignment is built from (DmrRpt.h).  A delay of
     * zero is a plain copy, which is what every non repeater call does.
     */
    const int16_t *cur = txSamples + (rptRenderHigh ? DMR_TX_SLOT_SAMPLES : 0);
    const int16_t *prev = txSamples + (rptRenderHigh ? 0 : DMR_TX_SLOT_SAMPLES);

    if (rptShift == 0)
        memcpy(idle, cur, DMR_TX_SLOT_SAMPLES * sizeof(stream_sample_t));
    else
        dmrRpt_txShift(prev, cur, rptShift, idle, DMR_TX_SLOT_SAMPLES);

    rptRenderHigh = !rptRenderHigh;

    /*
     * A failed sync is a slot that slipped, not a stream that died: the guard
     * counts it as a cadence slip and the call carries on.
     */
    outputStream_sync(txId, true);

    if (!txFirstBlock) {
        txFirstBlock = true;
        txg_firstBlock(tx, (uint32_t)getTick());
    } else {
        txg_block(tx, (uint32_t)getTick());
    }

    return true;
}

/**
 * One 30 ms timeslot of transmit: encode whatever the microphone gave us and
 * render the slot into the baseband stream.  The blocking write to the output
 * stream is what keeps the slot cadence: 1440 samples at 48 kHz are exactly
 * 30 ms, and the stream has two buffers.
 */
bool OpMode_DMR::txSlot(const rtxStatus_t *status)
{
    uint8_t frames[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES];
    struct dmrTxStep step;

    (void)status;

    /*
     * Slots of lead the alignment asked for: silence on the slot clock, which
     * is what lets the PA settle and still keeps the parity of the frame the
     * plan counted on.  They come before the vocoder is read, so a call that
     * spends them does not eat the first frames of speech.
     */
    if (rptPreSlots > 0) {
        int16_t *out = rptRenderTarget();

        dmrTx_slot(&tx, DMR_TXB_EMPTY, NULL, out);
        rptPreSlots--;
        if (!putSlot())
            return true;
        return false;
    }

    rptDriftSlew();

    /*
     * Keep the queue topped up: the microphone delivers a block every 20 ms and
     * the slot clock runs at 30 ms, so a slot has to take one frame or two.
     * Two at most, so the modulator still has time inside the slot; the queue
     * absorbs the jitter and a starved burst carries zero frames instead of
     * slipping the slot.
     */
    for (unsigned i = 0; (i < VOCODER_FRAMES_PER_SLOT)
                         && (frameCount < DMR_TX_QUEUE_FRAMES); i++) {
        dataBlock_t block = inputStream_getData(micId);

        if ((block.data == NULL) || (block.len < VOCODER_PCM_SAMPLES))
            break;

        uint8_t *dst = frameQueue[(frameHead + frameCount) % DMR_TX_QUEUE_FRAMES];

        if (dmrVocoder_encode((const int16_t *)block.data, dst) != 0)
            break;
        frameCount++;
    }

    /*
     * What this slot carries, and whether it is the last one, is decided in one
     * place (DmrTxFsm.h): the stage machine always ends within a fixed number
     * of slots after the PTT is released, whatever stage it was in.
     */
    step = dmrTxFsm_next(&fsm, platform_getPttStatus(), frameCount > 0);

    memset(frames, 0, sizeof(frames));
    if (step.kind == DMR_TXB_VOICE)
        dequeueFrames(frames);
    dmrTx_slot(&tx, step.kind, (const uint8_t (*)[DMR_AMBE_BYTES])frames,
               rptRenderTarget());

    /* A stream that is gone ends the call: nothing more can go on the air */
    if (!putSlot())
        return true;

    return step.finished;
}

#ifdef CONFIG_DMR_SMS
/**
 * Is a queued short message worth a transmission now?
 *
 * The answer is no while the operator is on the air: a voice call owns the
 * timeslot, and the message waits for it to end -- it stays queued, so it goes
 * out as soon as the channel is free.
 */
bool OpMode_DMR::needDataTx(const rtxStatus_t *status) const
{
    if (status->txDisable != 0)
        return false;
    if (txBlocked)
        return false;
    if (platform_getPttStatus())
        return false;
    if ((status->opStatus != RX) && (status->opStatus != OFF))
        return false;

    for (unsigned i = 0; i < DMR_SMS_ACK_QUEUE; i++)
        if (ackQueue[i].used)
            return true;

    return messaging_pending(MSG_TRANSPORT_DMR) > 0;
}

/**
 * Key the transmitter for a short message: no microphone, only the baseband
 * stream.  The first thing that goes out is a pending response, then the
 * oldest queued message.
 */
bool OpMode_DMR::startDataTx(const rtxStatus_t *status)
{
    unsigned blocks = 0;

    if (!startBaseband(status)) {
        stopTx();
        return false;
    }

    micPath = -1;
    micId = -1;
    txBurst = 0;
    txAcking = false;
    txMsgId = 0;

    struct dmrAck *ack = NULL;

    for (unsigned i = 0; i < DMR_SMS_ACK_QUEUE; i++) {
        if (ackQueue[i].used) {
            ack = &ackQueue[i];
            break;
        }
    }

    if (ack != NULL) {
        /* A response is a header plus, for TMS, one data block. */
        memset(&txPacket, 0, sizeof(txPacket));
        memcpy(txPacket.header, ack->header, sizeof(txPacket.header));
        txPacket.blockCount = ack->blocks;
        if (ack->blocks > 0)
            memcpy(txPacket.blocks[0], ack->data, sizeof(ack->data));
        ack->used = false;
        txAcking = true;
        blocks = txPacket.blockCount;
    } else {
        int id = messaging_begin(MSG_TRANSPORT_DMR);

        if (id < 0) {
            stopTx();
            return false;
        }
        const struct message *msg = messaging_find(id);

        if (msg == NULL) {
            messaging_done(id, false, 0, (uint32_t)getTick());
            stopTx();
            return false;
        }

        struct dmrSmsEncode req;

        memset(&req, 0, sizeof(req));
        /*
         * The payload flavour is the operator's choice (settings.sms_format);
         * "auto" is the IP compressed header this firmware always sent, the
         * shortest one, so more text fits in a data call.  The destination is
         * the message's own address; whether it is a talkgroup or a unit
         * follows the mode's destination type.
         */
        req.app = dmrSms_appForFormat(state.settings.sms_format);
        req.src = status->dmrSourceId;
        req.dst = msg->dst;
        req.group = (status->dmrDestType != DMR_DEST_USER);
        req.confirmed = (msg->flags & MSG_FLAG_CONFIRM) != 0;
        req.tmsSeq = 0;
        req.text = msg->text;

        if (dmrSms_encode(&req, &txPacket) != 0) {
            messaging_done(id, false, 0, (uint32_t)getTick());
            stopTx();
            return false;
        }
        txMsgId = id;
        blocks = txPacket.blockCount;
    }

    txIsData = true;

    /*
     * A message on a repeater channel aligns to the downlink frame with the
     * same plan as a voice call.  It does not wake the repeater first: a
     * message goes out when the channel is free and the wake would be a second
     * transmission with its own timing.  Documented in docs/DMR_REPEATER.md.
     */
    rptCallTiming(status, rptTimeslot(status));
    dmrTxData_initSlot(&dataFsm, blocks, rptPlan.burstFirst);
    txRunning = true;

    radio_enableTx();
    return true;
}

/**
 * One timeslot of a short message transmission: the header, the data blocks,
 * the TD_LC that closes the call and a tail slot, one burst per TDMA frame in
 * the timeslot of the mode -- the same cadence as voice.  The sequence and its
 * bound live in dmrTxData_next() (DmrTxFsm.h).
 */
bool OpMode_DMR::dataTxSlot(int16_t *out)
{
    if (rptPreSlots > 0) {
        rptPreSlots--;
        dmrTx_slot(&tx, DMR_TXB_EMPTY, NULL, out);
        return false;
    }

    rptDriftSlew();

    struct dmrTxStep step = dmrTxData_next(&dataFsm);

    if (step.kind == DMR_TXB_DATA) {
        uint8_t dataType = DMR_DT_RATE_12;
        uint8_t info[96];

        memset(info, 0, sizeof(info));

        switch (step.data) {
            case DMR_TXDATA_HEADER:
                (void)dmrSms_packetBurst(&txPacket, 0, &dataType, info);
                txBurst = 1;
                break;

            case DMR_TXDATA_BLOCK:
                (void)dmrSms_packetBurst(&txPacket, txBurst, &dataType, info);
                txBurst++;
                break;

            case DMR_TXDATA_LC:
            default: {
                /* The last block went out: close the call with the TD_LC */
                uint8_t lc[12];

                dmrTx_dataTerminator(lc, tx.cfg.sourceId, txPacket.dst,
                                     txPacket.group);
                for (int i = 0; i < 96; i++)
                    info[i] = (uint8_t)((lc[i >> 3] >> (7 - (i & 7))) & 1u);
                dataType = DMR_DT_TERMINATOR_LC;
                break;
            }
        }

        dmrTx_dataSlot(&tx, dataType, info, out);
    } else {
        dmrTx_slot(&tx, DMR_TXB_EMPTY, NULL, out);
    }

    return step.finished;
}
#endif /* CONFIG_DMR_SMS */
#endif /* CONFIG_DMR_TX */

/**
 * Force the end of the call, from the supervisor thread.
 *
 * Cancels both streams: the baseband one is what the RTX thread waits on, and
 * cancelling it is what makes it return. No audio path is released here -- the
 * RTX thread owns those and does it in stopTx() -- and no attempt is made to
 * stop the vocoder, which has no thread of its own.
 *
 * Defined in every build, because the vtable of the class is: with
 * CONFIG_DMR_TX off there is no stream to cancel and the mode cannot key, so
 * all that is left is the PTT lockout.
 */
void OpMode_DMR::abortTx()
{
    txBlocked = true;

#ifdef CONFIG_DMR_TX
    if (txId >= 0)
        audioStream_terminate(txId);
    if (micId >= 0)
        audioStream_terminate(micId);
#endif
}

void OpMode_DMR::update(rtxStatus_t *const rtx, const bool newCfg)
{
    /* An aborted call does not key again until the operator lets go of the PTT */
    if (platform_getPttStatus() == false)
        txBlocked = false;

    switch (rtx->opStatus) {
        case OFF:
#ifdef CONFIG_DMR_TX
            /* Unconditional: it is idempotent and it is what unkeys the PA */
            stopTx();
#endif
            radio_disableRtx();
            if (startRx) {
                rtx->opStatus = RX;
                return;
            }
            sleepFor(0, 30);
            return;

#ifdef CONFIG_DMR_TX
        case TX: {
            /*
             * A short message owns the transmitter while it lasts: no
             * microphone, its own stage machine.
             */
            bool finished;
            struct txGuard *guard = rtx_getTxGuard();

            /*
             * The over is over when the operator says so: from here the tail
             * (terminator, tail slot, DSP drain) has a deadline of its own, and
             * the safety exit of the stage machine bounds it too.
             */
            if (platform_getPttStatus() == false)
                txg_finishing(guard, (uint32_t)getTick());

#ifdef CONFIG_DMR_SMS
            if (txIsData) {
                finished = dataTxSlot(rptRenderTarget()) || !putSlot();
            } else
#endif
                finished = txSlot(rtx);

            /*
             * The stage machines send the terminator and one tail slot after
             * the last burst; when those are done the radio goes off.
             */
            if (finished) {
#ifdef CONFIG_DMR_SMS
                const bool wasData = txIsData;
                const bool ackOnly = txAcking;
                const int msgId = txMsgId;
                bool delivered = false;

                if ((msgId != 0) && !ackOnly) {
                    const struct message *msg = messaging_find(msgId);

                    /*
                     * A message is "sent" once its last burst is on the air.
                     * When it asked for a confirmation, the delivery response
                     * moves it to "delivered" later; if none arrives the
                     * store's timeout gives up on it.
                     */
                    delivered = (msg == NULL)
                                || ((msg->flags & MSG_FLAG_CONFIRM) == 0);
                }
#endif
                stopTx();
#ifdef CONFIG_DMR_SMS
                if (wasData && (msgId != 0) && !ackOnly)
                    messaging_done(msgId, delivered, 0, (uint32_t)getTick());
#endif
                rtx->opStatus = OFF;
                startRx = true;
            }
            return;
        }
#endif

        case RX:
#ifdef CONFIG_DMR_SMS
            /* The address our answers go out from, whatever the TX options */
            selfId = rtx->dmrSourceId;
#endif
#ifdef CONFIG_DMR_TX
            /* The downlink grid is only worth tracking on a repeater channel */
            rptTracking = (rtx->dmrRepeater != 0);
#endif
#ifdef CONFIG_DMR_TX
            /*
             * The measurement and calibration requests come from the menu and
             * run here, because this thread owns the audio paths and the
             * transmitter.  They only ever start from receive, never on top of
             * a call.
             */
            if (rtx->audioCalRequest) {
                audioCalibrate(rtx);
                return;
            }
            if (rtx->rptCalRequest && (!platform_getPttStatus())
                && (!txRunning)) {
                if (!dmrRptCal_active(&rptCal))
                    dmrRptCal_start(&rptCal, (uint32_t)getTick());
            }

            if (dmrRptCal_active(&rptCal)) {
                if (platform_getPttStatus() || rtx->txDisable) {
                    /* The operator wants the radio back: stop transmitting */
                    dmrRptCal_cancel(&rptCal);
                }
                rptCalStep(rtx);
                break;
            }

            /*
             * A repeater channel owns the PTT: the gate, the activation and
             * the alignment all have to happen before the PA is keyed, and
             * they need the receiver, so this runs while the mode is still
             * receiving.
             */
            if (platform_getPttStatus() && (rtx->txDisable == 0)
                && (txBlocked == false) && rtx->dmrRepeater) {
                if (rptAccessStep(rtx))
                    return;
                break;
            }
            if (!platform_getPttStatus()) {
                rptAccess = false;
                if (rptWake.stage != DMR_RPT_WAKE_IDLE)
                    rptWake.stage = DMR_RPT_WAKE_IDLE;
            }
#endif
            break;

        default:
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

#ifdef CONFIG_DMR_TX
    /*
     * PTT keys the transmitter, unless the operator disabled TX or there is no
     * AMBE+2 encoder linked in.  The receiver has to be stopped first: the
     * BK4819 is half duplex and both use the same audio path.
     */
    if (platform_getPttStatus() && (rtx->txDisable == 0)
        && (txBlocked == false) && (!rtx->dmrRepeater)) {
        /*
         * Key the transmitter even if a station is being received: with PTT
         * pressed the operator wants to talk, and the receiver is stopped
         * first because the BK4819 is half duplex.  A repeater channel never
         * gets here: rptAccessStep() owns its PTT, because the gate and the
         * activation come before the PA.
         */
        stopSampling();
        radio_disableRtx();
        if (startTx(rtx)) {
            rtx->opStatus = TX;
            return;
        }
        startRx = true;
    }

#ifdef CONFIG_DMR_SMS
    /*
     * No voice on the air: a queued message or a delivery response goes out
     * now, one burst per frame, and the receiver comes back afterwards.  The
     * check comes after the PTT branch, so a message never delays the
     * operator.
     */
    if (needDataTx(rtx)) {
        stopSampling();
        radio_disableRtx();
        if (startDataTx(rtx)) {
            rtx->opStatus = TX;
            return;
        }
        startRx = true;
    }
#endif
#endif

    if (startRx) {
        startSampling();
        radio_enableRx();
        startRx = false;
    }

    /* A new frequency is a new channel: start from scratch */
    if (newCfg)
        reset();

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * Wait for the next half buffer of samples.  A failed read means samples
     * were lost: the symbol stream jumped, so the demodulator has to start
     * over, and there is no point in sleeping on top of the delay.
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0) {
            dmrPhy_reset();
            dmrStatus_reset();
        } else {
            sleepFor(0, 10);
        }
        return;
    }
    dataErrors = 0;

    /*
     * Drop what follows a start or a retune: the receiver settles and the
     * level estimates would be taken from its transient.
     */
    if (settle > 0) {
        settle -= (block.len < settle) ? block.len : settle;
        return;
    }

    dmrPhy_push(block.data, block.len, burstTrampoline, this);

    if (dmrPhy_synced())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
