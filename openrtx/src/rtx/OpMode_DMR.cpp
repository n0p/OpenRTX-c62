/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include "core/messaging.h"
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DMR/DmrFec.h"
#include "protocols/DMR/DmrPhy.h"
#include "protocols/DMR/DmrSpeech.h"
#include "protocols/DMR/DmrStatus.h"
#include "rtx/OpMode_DMR.hpp"

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
static int16_t micPcm[VOCODER_PCM_SAMPLES];
#endif

#ifdef CONFIG_DMR_SMS
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
}

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
#else
/**
 * Without the short message layer a burst only feeds the status block and the
 * speech queue.
 */
static void onBurstCallback(const struct dmrBurst *burst, void *user)
{
    (void)user;

    dmrStatus_burst(burst);
    if (burst->voice && (burst->frameCount > 0))
        dmrSpeech_push(burst->frame, burst->frameCount);
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
    , micPath(-1)
    , micId(-1)
    , txPath(-1)
    , txId(-1)
    , frameHead(0)
    , frameCount(0)
    , burstSlot(true)
#ifdef CONFIG_DMR_SMS
    , txBurst(0)
    , txMsgId(0)
    , txIsData(false)
    , txAcking(false)
#endif
    , txStage(TX_LEAD)
#endif
{
#ifdef CONFIG_DMR_SMS
    memset(smsRx, 0, sizeof(smsRx));
    memset(ackQueue, 0, sizeof(ackQueue));
    dmrSmsRx_reset(&smsRx[0]);
    dmrSmsRx_reset(&smsRx[1]);
#endif
#ifdef CONFIG_DMR_TX
    memset(&tx, 0, sizeof(tx));
    memset(frameQueue, 0, sizeof(frameQueue));
#ifdef CONFIG_DMR_SMS
    memset(&txPacket, 0, sizeof(txPacket));
#endif
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
#ifdef CONFIG_DMR_SMS
    dmrSmsRx_reset(&smsRx[0]);
    dmrSmsRx_reset(&smsRx[1]);
#endif
}

void OpMode_DMR::enable()
{
    dmrPhy_init();
    reset();
    dmrSpeech_start();
    startRx = true;
}

void OpMode_DMR::disable()
{
    startRx = false;
#ifdef CONFIG_DMR_TX
    stopTx();
#endif
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

bool OpMode_DMR::startTx(const rtxStatus_t *status)
{
    if (!dmrVocoder_available())
        return false;

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
#ifdef CONFIG_DMR_SMS
    selfId = status->dmrSourceId;
    txIsData = false;
    txAcking = false;
#endif

    micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if (micPath < 0)
        return false;

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0) {
        audioPath_release(micPath);
        micPath = -1;
        return false;
    }

    micId = audioStream_start(micPath, micPcm, VOCODER_PCM_SAMPLES, 8000,
                              STREAM_INPUT | BUF_CIRC_DOUBLE);
    txId = audioStream_start(txPath, txSamples, DMR_TX_SLOT_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if ((micId < 0) || (txId < 0)) {
        stopTx();
        return false;
    }

    frameHead = 0;
    frameCount = 0;
    burstSlot = true;
    txStage = TX_LEAD;
    txRunning = true;

    /* Last: the PA, the ramps of the first slot need it settled */
    radio_enableTx();
    return true;
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
 * stream.  The first thing to go out is a pending response, then the oldest
 * queued message.
 */
bool OpMode_DMR::startDataTx(const rtxStatus_t *status)
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
    selfId = status->dmrSourceId;

    txAcking = false;
    txMsgId = 0;
    txBurst = 0;

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
    } else {
        int id = messaging_begin(MSG_TRANSPORT_DMR);

        if (id < 0)
            return false;
        const struct message *msg = messaging_find(id);

        if (msg == NULL) {
            messaging_done(id, false, 0, (uint32_t)getTick());
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
            return false;
        }
        txMsgId = id;
    }

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0)
        return false;

    txId = audioStream_start(txPath, txSamples, DMR_TX_SLOT_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (txId < 0) {
        audioPath_release(txPath);
        txPath = -1;
        return false;
    }

    micPath = -1;
    micId = -1;
    burstSlot = true;
    txStage = TX_DATA;
    txIsData = true;
    txRunning = true;

    radio_enableTx();
    return true;
}

/**
 * One timeslot of a short message transmission: header, data blocks, the
 * terminator of the data call, then one tail slot.  One burst per TDMA frame
 * in the timeslot of the mode, the same cadence as voice.
 *
 * @param out: samples of the slot.
 * @return true when the transmission is over.
 */
bool OpMode_DMR::dataTxSlot(int16_t *out)
{
    uint8_t dataType = DMR_DT_RATE_12;
    uint8_t info[96];
    bool finished = false;

    memset(info, 0, sizeof(info));

    if (burstSlot) {
        switch (txStage) {
            case TX_DATA:
                if (dmrSms_packetBurst(&txPacket, txBurst, &dataType, info)) {
                    dmrTx_dataSlot(&tx, dataType, info, out);
                    txBurst++;
                } else {
                    /* the last block went out: close the data call with TD_LC */
                    uint8_t lc[12];

                    txStage = TX_DATA_END;
                    dmrTx_dataTerminator(lc, tx.cfg.sourceId, txPacket.dst,
                                         txPacket.group);
                    for (int i = 0; i < 96; i++)
                        info[i] = (uint8_t)((lc[i >> 3] >> (7 - (i & 7))) & 1u);
                    dmrTx_dataSlot(&tx, (uint8_t)DMR_DT_TERMINATOR_LC, info, out);
                }
                break;

            case TX_DATA_END:
                dmrTx_slot(&tx, DMR_TXB_EMPTY, NULL, out);
                finished = true;
                break;

            default:
                dmrTx_slot(&tx, DMR_TXB_EMPTY, NULL, out);
                finished = true;
                break;
        }
        burstSlot = false;
    } else {
        dmrTx_slot(&tx, DMR_TXB_EMPTY, NULL, out);
        burstSlot = true;
    }

    return finished;
}
#endif /* CONFIG_DMR_SMS */

void OpMode_DMR::stopTx()
{
    if (!txRunning)
        return;

    txRunning = false;

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
    enum dmrTxBurst kind = DMR_TXB_VOICE;
    bool finished = false;

    (void)status;

#ifdef CONFIG_DMR_SMS
    /*
     * A short message owns the transmitter while it lasts: no microphone and
     * no voice stage machine.
     */
    if (txIsData) {
        finished = dataTxSlot(txSamples);

        stream_sample_t *idle = outputStream_getIdleBuffer(txId);

        if (idle != NULL) {
            memcpy(idle, txSamples, sizeof(txSamples));
            outputStream_sync(txId, true);
        }
        return finished;
    }
#endif

    /* Keep the queue topped up: the microphone delivers a block every 20 ms and
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

    switch (txStage) {
        case TX_LEAD:
            kind = DMR_TXB_EMPTY;
            /*
             * The PA is up but the call does not start until there is voice to
             * send: the LC header announces a call and the first voice burst
             * follows it one slot later, so keying the header with an empty
             * queue would put a header and then silence on the air.  The
             * encoder has already produced the samples by the time the
             * microphone block is read, so this costs one slot at most.
             */
            if (platform_getPttStatus() && (frameCount > 0))
                txStage = TX_LC_HEADER;
            break;

        case TX_LC_HEADER:
            kind = DMR_TXB_VOICE_LC;
            txStage = TX_VOICE;
            break;

        case TX_VOICE:
            /*
             * One burst every other slot.  A DMR call occupies one of the two
             * timeslots of the carrier and a voice burst carries 60 ms of
             * speech, which is exactly what the vocoder produces in those
             * 60 ms; transmitting a burst in every slot would send the speech
             * at twice its speed.  The slot in between is silence, not a
             * shorter burst: a handheld that keys the other slot tramples the
             * station that owns it.
             */
            if (burstSlot) {
                kind = DMR_TXB_VOICE;
                burstSlot = false;
            } else {
                kind = DMR_TXB_EMPTY;
                burstSlot = true;
            }
            if (!platform_getPttStatus())
                txStage = TX_TERMINATOR;
            break;

        case TX_TERMINATOR:
            kind = DMR_TXB_TERMINATOR;
            txStage = TX_TAIL;
            break;

        case TX_TAIL:
            /*
             * One empty slot after the terminator: it carries the tail of the
             * last pulse and lets the PA ramp down inside the guard.
             */
            kind = DMR_TXB_EMPTY;
            finished = true;
            break;

        default:
            kind = DMR_TXB_EMPTY;
            finished = true;
            break;
    }

    memset(frames, 0, sizeof(frames));
    if (kind == DMR_TXB_VOICE)
        dequeueFrames(frames);
    dmrTx_slot(&tx, kind, (const uint8_t (*)[DMR_AMBE_BYTES])frames, txSamples);

    stream_sample_t *idle = outputStream_getIdleBuffer(txId);

    if (idle != NULL) {
        memcpy(idle, txSamples, sizeof(txSamples));
        outputStream_sync(txId, true);
    }

    return finished;
}
#endif /* CONFIG_DMR_TX */

void OpMode_DMR::update(rtxStatus_t *const rtx, const bool newCfg)
{
    switch (rtx->opStatus) {
        case OFF:
#ifdef CONFIG_DMR_TX
            if (txRunning)
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
        case TX:
            /*
             * The stage machine sends the terminator and one tail slot after
             * the PTT is released; when those are done the radio goes off.
             */
            if (txSlot(rtx)) {
                const bool wasData = txIsData;

                stopTx();
#ifdef CONFIG_DMR_SMS
                if (wasData) {
                    if ((!txAcking) && (txMsgId != 0)) {
                        const struct message *msg = messaging_find(txMsgId);

                        /*
                         * A message is "sent" once its last burst is on the
                         * air.  When it asked for a confirmation, the delivery
                         * response moves it to "delivered" later; if none
                         * arrives the store's timeout gives up on it.
                         */
                        messaging_done(txMsgId,
                                       (msg != NULL)
                                           && ((msg->flags & MSG_FLAG_CONFIRM) == 0),
                                       0, (uint32_t)getTick());
                    }
                    txMsgId = 0;
                    txIsData = false;
                    txAcking = false;
                }
#else
                (void)wasData;
#endif
                rtx->opStatus = OFF;
                startRx = true;
            }
            return;
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
    if (platform_getPttStatus() && (rtx->txDisable == 0)) {
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

#ifdef CONFIG_DMR_SMS
    dmrPhy_push(block.data, block.len, burstTrampoline, this);
#else
    dmrPhy_push(block.data, block.len, onBurstCallback, NULL);
#endif

    if (dmrPhy_synced())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
