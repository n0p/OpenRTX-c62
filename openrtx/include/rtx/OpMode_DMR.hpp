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

/*
 * The transmitter only exists in the build with CONFIG_DMR_TX (Kconfig, off by
 * default until there is an AMBE+2 encoder).  Everything it needs -- the
 * chain, the audio paths, the 3 KiB baseband buffer -- is compiled out
 * otherwise, and so is the PTT branch: without the option, PTT in this mode
 * does nothing at all instead of failing at link time.
 */
#ifdef CONFIG_DMR_TX
#include "protocols/DMR/DmrTx.h"

/** AMBE+2 frames queued for transmission: four voice bursts of margin. */
#define DMR_TX_QUEUE_FRAMES 12
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

    /* Transmit */
#ifdef CONFIG_DMR_TX
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
    stream_sample_t txSamples[DMR_TX_SLOT_SAMPLES];

    uint8_t frameQueue[DMR_TX_QUEUE_FRAMES][DMR_AMBE_BYTES];
    unsigned frameHead;
    unsigned frameCount;
    bool burstSlot;      // this slot carries a burst (TDMA: every other one)

    enum dmrTxStage
    {
        TX_LEAD,
        TX_LC_HEADER,
        TX_VOICE,
        TX_TERMINATOR,
        TX_TAIL
    } txStage;
#endif
};

#endif /* OPMODE_DMR_H */
