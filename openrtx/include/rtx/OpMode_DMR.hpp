/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DMR_H
#define OPMODE_DMR_H

#include <stddef.h>
#include <stdint.h>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"

/*
 * The DMR mode of this tree, built on the protocol package (protocols/DMR/,
 * dmr_*.c): ours is only what touches the radio, the settings and the UI.
 * The package's own mode (rtx/OpMode_DMR_RX.cpp) is the reference and needs
 * that tree's non-volatile memory and C62 PA-gating drivers, so it is not
 * built here; docs/DMR_PKG.md maps every call below to the package's.
 *
 * The transmitter only exists with CONFIG_DMR_TX.  Without it everything it
 * needs -- the modulator, the baseband stream, the PTT branch -- is compiled
 * out and PTT in this mode does nothing at all.
 */
#ifdef CONFIG_DMR_TX
#include "protocols/DMR/dmr_enc.h"
#include "protocols/DMR/dmr_tx.h"
#include "rtx/DmrTxPlan.h"

/** AMBE+2 frames queued for transmission: four bursts of margin. */
#define DMR_TX_QUEUE_FRAMES 12
#endif

/** One 30 ms half of the baseband stream at 48 kHz */
#define DMR_HALF_SAMPLES 1440

/**
 * DMR mode: receives the radio's flat FM discriminator output (4FSK,
 * 4800 symbols/s) and hands it to the package's demodulator, which decodes
 * the bursts and the slot state machine (dmr_demod.c, dmr_proto.c) and queues
 * the voice (dmr_voice.c, played by DmrSpeech.c).  On PTT it transmits a
 * group or unit call in the selected timeslot with the package's frame
 * builders (dmr_tx.c) and modulator.
 *
 * Short messages (CONFIG_DMR_SMS): texts the package receives go into the
 * shared store (core/messaging.h) and queued messages leave through
 * dmrSms_send()/dmrSms_txNext(), one burst per TDMA frame.
 *
 * Tier III (CONFIG_DMR_TRUNK): the package's engine follows the system's
 * control channel; the mode feeds it the receiver level and retunes where it
 * asks (dmr_trunk.h).
 */
class OpMode_DMR : public OpMode
{
public:
    OpMode_DMR();
    ~OpMode_DMR();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    /**
     * Force the end of the call from the transmit supervisor: cancel the
     * baseband and microphone streams, which is what releases the RTX thread
     * while it waits for a boundary.  The PA is already off.
     */
    virtual void abortTx() override;

    virtual opmode getID() override
    {
        return OPMODE_DMR;
    }

    virtual bool rxSquelchOpen() override;

private:
    void reset();
    void startSampling();
    void stopSampling();
    void feed(const dataBlock_t &block);

    /** The same idle-until-released lockout the old mode had */
    volatile bool txBlocked;

    /* Receive */
    bool startRx;
    unsigned dataErrors;
    size_t settle;
    pathId basebandPath;
    streamId basebandId;
    /* The two halves of the receive stream (the TX one is separate) */
    stream_sample_t rxSamples[2 * DMR_HALF_SAMPLES];
    /* Ours: the package's counters as last read, for the per-block deltas of
     * the stats page (core/mode_stats.h) */
    uint32_t statsFrames;
    uint32_t statsValid;
    uint32_t statsOverruns;

#ifdef CONFIG_DMR_SMS
    void smsPoll();
    void smsStartTx(const rtxStatus_t *status);
    uint32_t smsSerial;
    uint32_t selfId;
    /** core/messaging entry handed to the package's list, -1 when none */
    int smsTxId;
    /** That list reached the air before the call ended */
    bool smsDataSent;
#endif

#ifdef CONFIG_DMR_TX
    bool startBaseband(const rtxStatus_t *status);
    bool startTx(const rtxStatus_t *status);
    /** Refuse this over: block the PTT until it is released, say why. */
    bool txRefuse(const char *why);
    bool startDataTx(const rtxStatus_t *status);
    void stopTx();
    uint8_t txTimeslot(const rtxStatus_t *status) const;
    bool txSlot(const rtxStatus_t *status);
    bool dataTxSlot(const rtxStatus_t *status);
    bool needDataTx(const rtxStatus_t *status) const;
    bool putHalf();
    bool fillMic(unsigned slots);
    /** Samples of silence still to emit before the first burst (alignment) */
    size_t txPreRoll();

    bool txRunning;
    bool txFirstBlock;
    bool txIsData;
    bool txEnding;
    /** The voice call's counters and the slot machine (rtx/DmrTxPlan.h) */
    dmrTxCall_t txCall;
    /* A data call's own: 0 the list, 1 the tail, and the slots sent */
    unsigned txStage;
    unsigned txVoiceBurst;
    bool txAligned;       ///< Through a repeater: aligned to its TS1
    bool txSaidStream;    ///< The lost-sink line has been printed for this over
    size_t txSilence;     ///< Samples of alignment silence still to write

    pathId micPath;
    streamId micId;
    pathId txPath;
    streamId txId;

    dmr_tx_t tx;  ///< The package's call state (dmr_tx.h)
    dmr_mod_t txMod;
    uint8_t txBurst[DMR_TX_BURST_DIBITS];
    uint8_t frameQueue[DMR_TX_QUEUE_FRAMES][9];
    unsigned frameHead;
    unsigned frameCount;

    /* Half of the baseband stream being rendered, and the other one */
    stream_sample_t txSamples[2 * DMR_HALF_SAMPLES];
    int16_t micPcm[2 * 160];
#endif

#ifdef CONFIG_DMR_TRUNK
    void trunkPoll(rtxStatus_t *const status);
    uint32_t trunkHz;
#endif
};

#endif /* OPMODE_DMR_H */
