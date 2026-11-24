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
#include "protocols/DMR/DmrTx.h"
#include "protocols/DMR/DmrVocoder.h"
#endif

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
    cfg.destId = status->dmrTalkgroup;
    cfg.group = true;
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

bool OpMode_DMR::startTx(const rtxStatus_t *status)
{
    struct txGuard *tx = rtx_getTxGuard();

    txg_enter(tx, (uint32_t)getTick());

    if (!dmrVocoder_available()) {
        /* No encoder: the chain would send three silent frames per burst */
        txg_streamsFailed(tx, (uint32_t)getTick());
        return false;
    }

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
    dmrTxFsm_init(&fsm);
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
 * Force the end of the call, from the supervisor thread.
 *
 * Cancels both streams: the baseband one is what the RTX thread waits on, and
 * cancelling it is what makes it return. No audio path is released here -- the
 * RTX thread owns those and does it in stopTx() -- and no attempt is made to
 * stop the vocoder, which has no thread of its own.
 */
void OpMode_DMR::abortTx()
{
    txBlocked = true;

    if (txId >= 0)
        audioStream_terminate(txId);
    if (micId >= 0)
        audioStream_terminate(micId);
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

    /* One half of the stream buffer is exactly one slot */
    memcpy(idle, txSamples, DMR_TX_SLOT_SAMPLES * sizeof(stream_sample_t));

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
               txSamples);

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
        req.app = DMR_SMS_APP_IP_HC; /* the shortest header: more text fits */
        req.src = status->dmrSourceId;
        req.dst = msg->dst;
        req.group = (msg->dst == status->dmrTalkgroup);
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
    dmrTxData_init(&dataFsm, blocks);
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
                finished = dataTxSlot(txSamples) || !putSlot();
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
    if (platform_getPttStatus() && (rtx->txDisable == 0) && (txBlocked == false)) {
        /*
         * Key the transmitter even if a station is being received: with PTT
         * pressed the operator wants to talk, and the receiver is stopped
         * first because the BK4819 is half duplex.
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
