/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DMR_H
#define OPMODE_DMR_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"

struct dmrBurst;

/*
 * The transmitter only exists in the build with CONFIG_DMR_TX (Kconfig, off by
 * default until there is an AMBE+2 encoder).  Everything it needs -- the
 * chain, the audio paths, the 3 KiB baseband buffer -- is compiled out
 * otherwise, and so is the PTT branch: without the option, PTT in this mode
 * does nothing at all instead of failing at link time.
 */
#ifdef CONFIG_DMR_TX
#include "protocols/DMR/DmrTx.h"
#include "protocols/DMR/DmrTxFsm.h"

/** AMBE+2 frames queued for transmission: four voice bursts of margin. */
#define DMR_TX_QUEUE_FRAMES 12
#endif

#ifdef CONFIG_DMR_SMS
#include "protocols/DMR/DmrSms.h"

/** Delivery responses waiting for the next free slot. */
#define DMR_SMS_ACK_QUEUE 2
#endif

/**
 * DMR mode: receives the radio's flat FM discriminator output (4FSK,
 * 4800 symbols/s) and decodes voice bursts with the AMBE+2 vocoder; on PTT it
 * transmits a group call in the selected timeslot with the transmit chain of
 * DmrTx.c (voice LC header, voice bursts, terminator, PA envelope).
 *
 * Transmission needs an AMBE+2 encoder: it arrives through the
 * dmrVocoder_encode() hook (see DmrVocoder.h).  Without one the mode refuses
 * to key the PA instead of sending silence.
 *
 * With CONFIG_DMR_SMS the mode also carries short messages (DmrSms.h): data
 * bursts of a data call are reassembled per timeslot into the shared message
 * store (core/messaging.h), and a queued message is transmitted burst by
 * burst in the timeslot of the mode, one burst per TDMA frame, only when the
 * operator is not on the air.  A message that asks for a delivery response
 * gets one automatically, as does a TMS message whose sender asked for its
 * application acknowledgement.  Voice and short message calls are both driven
 * by the pure stage machines of DmrTxFsm.h, so every call ends on its own
 * within a bounded number of slots.
 *
 * The slot layout is DMR_TX_LAYOUT_MS: the 132 symbols of a burst are centred
 * in the 30 ms timeslot and the 2,5 ms of guard around them hold the PA ramps.
 * Receiving in the other timeslot while transmitting needs the radio to switch
 * in 2,5 ms; radio_C62.cpp documents what the BK4819 does there.
 */
class OpMode_DMR : public OpMode
{
public:
    OpMode_DMR();
    ~OpMode_DMR();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_DMR;
    }

    virtual bool rxSquelchOpen() override;

private:
    void reset();
    void startSampling();
    void stopSampling();

    /* Receive */
    bool startRx;
    unsigned dataErrors; // Consecutive failed reads of the sample stream
    size_t settle;       // Samples still to drop after a start or retune
    pathId basebandPath;
    streamId basebandId;

    /*
     * One receive path in every build: the burst goes to the status block and
     * to the speech queue and, with CONFIG_DMR_SMS, also to the short message
     * reassembler of its timeslot.  It is a member because the reassemblers
     * belong to the mode instance, which dmrPhy_push() gets as user pointer.
     */
    static void burstTrampoline(const struct dmrBurst *burst, void *user);
    void onBurst(const struct dmrBurst *burst);

#ifdef CONFIG_DMR_SMS
    /** A short one to three burst transmission queued by the receiver: a
     *  delivery response (header only) or a TMS acknowledgement. */
    struct dmrAck {
        uint8_t header[12];  ///< 96 bits of the data header
        uint8_t data[12];    ///< its single rate 1/2 block, if any
        uint8_t blocks;      ///< 0 for a C_RHEAD, 1 for a TMS record
        bool used;
    };

    void onMessage(const struct dmrSmsMessage *msg, bool wantsResponse);
    void onResponse(const struct dmrSmsMessage *msg);
    void queueResponse(uint32_t dst, uint32_t src, uint8_t sap, uint8_t respClass,
                       uint8_t respType, uint8_t status);
    void queueTmsAck(uint32_t dst, uint32_t src, uint8_t seq);
    struct dmrAck *freeAck(void);

    struct dmrSmsRx smsRx[2];        ///< One reassembler per timeslot
    struct dmrAck ackQueue[DMR_SMS_ACK_QUEUE];
    uint32_t selfId;      ///< Our radio ID, from the configuration
    uint32_t smsReceived; ///< Messages stored, for diagnostics
    uint32_t smsDropped;  ///< Data bursts discarded (bad CRC, no handler)
#endif

    /* Transmit */
#ifdef CONFIG_DMR_TX
    bool startBaseband(const rtxStatus_t *status);
    bool startTx(const rtxStatus_t *status);
    void stopTx();
    bool txSlot(const rtxStatus_t *status);
    unsigned dequeueFrames(uint8_t frames[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES]);

    bool txRunning;
    pathId micPath;      // MIC -> MCU, raw 8 kHz PCM
    streamId micId;
    pathId txPath;       // MCU -> RTX, 48 kHz baseband
    streamId txId;

    struct dmrTxState tx;

    /*
     * Baseband output buffer.  An audio stream started with BUF_CIRC_DOUBLE is
     * given the whole two-half buffer, and hands out one half at a time: this
     * has to be 2 * DMR_TX_SLOT_SAMPLES, or every write of a 30 ms slot lands
     * half of itself in the other half of the buffer and half of it past the
     * end of this object.
     */
    stream_sample_t txSamples[2 * DMR_TX_SLOT_SAMPLES];

    uint8_t frameQueue[DMR_TX_QUEUE_FRAMES][DMR_AMBE_BYTES];
    unsigned frameHead;
    unsigned frameCount;

    struct dmrTxFsm fsm; // voice stage machine, see DmrTxFsm.h

#ifdef CONFIG_DMR_SMS
    bool startDataTx(const rtxStatus_t *status);
    bool dataTxSlot(int16_t *out);
    bool needDataTx(const rtxStatus_t *status) const;

    struct dmrSmsPacket txPacket; ///< Message being transmitted
    struct dmrTxData dataFsm;     ///< Data call machine, see DmrTxFsm.h
    unsigned txBurst;             ///< Next burst of the packet
    int txMsgId;                  ///< Message id in the store, 0 if none
    bool txIsData;                ///< The running transmission is a message
    bool txAcking;                ///< ... and it is a delivery response
#endif
#endif
};

#endif /* OPMODE_DMR_H */
