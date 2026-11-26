/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DPMR_H
#define OPMODE_DPMR_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"
#include "protocols/DPMR/DpmrData.h"
#include "protocols/DPMR/DpmrDefs.h"

#ifdef CONFIG_DPMR_TX
#include "protocols/DPMR/DpmrTx.h"
#include "protocols/Fsk4Tx.h"

/** AMBE+2 frames queued for transmission: two frames of margin. */
#define DPMR_TX_QUEUE_FRAMES 8

/** One 20 ms half of the baseband stream: 48 symbols at 48 kHz. */
#define DPMR_TX_HALF_SAMPLES 960

/** Halves per frame: the END frame is one of them, it is 48 symbols long. */
#define DPMR_TX_FRAME_HALVES 4
#endif

/**
 * dPMR mode (ETSI TS 102 490): receive only.
 *
 * The radio is tuned to a 6,25 kHz channel and its flat FM discriminator output
 * is fed to the dPMR physical layer (protocols/DPMR/DpmrPhy.h): 4FSK at 2400
 * symbols/s, 80 ms frames of 384 bits, superframes of four frames, header frame
 * at the start and end frame at the end.
 *
 * dPMR is FDMA, not TDMA: there are no 40 ms timeslots and no second slot to
 * listen to.  One carrier carries one call at a time.
 *
 * What the mode gives the operator:
 *  - voice: the TCH of every voice frame is four AMBE+2 3600x2450 frames and is
 *    played through the same vocoder the DMR mode uses (DpmrSpeech.h);
 *  - the call's identity and state: the header frame carries the called and own
 *    addresses, the CCH of every frame carries the frame number, the
 *    communication mode and the 12 bit slice of the addresses, and the end
 *    frame carries the 5 bit status message of clause 5.15;
 *  - short data: the 5 bit status of the end frame (clause 5.15) and the two
 *    user bytes per frame of a voice + slow data call (clause 5.9.1) are
 *    decoded; the text of a voice + appended data call (clause 5.9.2) is
 *    reassembled.  Handing it to core/messaging needs a transport id for dPMR,
 *    see docs/RX_DPMR.md.
 *
 * Transmit (CONFIG_DPMR_TX): protocols/DPMR/DpmrTx.c builds the header frame,
 * the superframes and the END frame, the AMBE+2 encoder fills the TCH and the
 * 4FSK modulator the DMR chain uses shapes it, half a frame at a time.  The
 * channel code of the frequency and the frame numbers are the standard's, so
 * this receiver and a reference decoder read the call back whole.  There is no
 * listen-before-talk: the mode keys when the operator does.  See
 * docs/TX_DPMR.md.
 */
class OpMode_DPMR : public OpMode
{
public:
    OpMode_DPMR();
    ~OpMode_DPMR();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    /** Last status message decoded from an END frame (clause 5.15), -1 none. */
    uint8_t lastStatusMessage() const { return lastStatus; }

    /** Communication mode of the current call (clause 5.7), 0xFF if unknown. */
    uint8_t communicationMode() const { return modeCode; }

    /** Header type of the current call (clause 5.11), 0xFF if unknown. */
    uint8_t headerType() const { return htCode; }

    /** Called and own station address from the header frame, 24 bits each. */
    uint32_t calledId() const { return calledAddr; }
    uint32_t ownId() const { return ownAddr; }

    /** 12 bit channel code of the channel in use (clause 6.1.5), 0 unknown. */
    uint16_t channelCode() const { return code; }

    /** Tx wait time of the last END frame, clause 5.14, 0 if none. */
    uint8_t txWait() const { return lastWait; }

    /** Last reassembled slow data message (clause 5.9.1). */
    const uint8_t *slowDataText() const { return sldText; }
    uint8_t slowDataLength() const { return sldLen; }
    bool slowDataTruncated() const { return sldTruncated; }
    uint32_t slowDataMessages() const { return sldMessages; }

    /** Format and length of an appended data message (clause 5.9.2). */
    uint8_t appendedFormat() const { return sldFormat; }
    uint8_t appendedLength() const { return sldLength; }

    /** Counters for the diagnostics screen. */
    uint32_t framesDecoded() const { return framesOk; }
    uint32_t headersDecoded() const { return headersOk; }
    uint32_t endsDecoded() const { return endsOk; }

    /**
     * Squelch: open while a transmission is being followed, which is what the
     * frame grid and the CCH CRC establish (DpmrPhy.h).
     */
    virtual bool rxSquelchOpen() override;

private:
    static void frameTrampoline(const struct dpmrFrame *frame, void *user);
    void onFrame(const struct dpmrFrame *frame);

    void reset();
    void startSampling();
    void stopSampling();

#ifdef CONFIG_DPMR_TX
    bool startBaseband(const rtxStatus_t *status);
    bool startTx(const rtxStatus_t *status);
    void stopTx();
    bool fillMic(unsigned frames);
    bool putHalf();
    bool txSlot(const rtxStatus_t *status);

    dpmr_tx_t tx;
    fsk4_mod_t txMod;
    int8_t levelsBuf[DPMR_FRAME_SYMBOLS];
    uint8_t frameQueue[DPMR_TX_QUEUE_FRAMES][DPMR_AMBE_BYTES];
    unsigned frameHead;
    unsigned frameCount;
    int txStage;      ///< 0 preamble, 1 header, 2 payload, 3 END, 4 tail
    int txHalf;       ///< 0..3, quarter of the frame being sent
    int txTail;       ///< Halves of tail sent
    int txFrames;     ///< Preamble frames sent
    bool txRunning;
    bool txFirstBlock;
    int txPath, txId, micPath, micId;
    stream_sample_t txSamples[2 * DPMR_TX_HALF_SAMPLES];
    int16_t micPcm[2 * 160];
#endif

    pathId basebandPath = -1;
    streamId basebandId = -1;
    bool startRx = false;
    uint32_t settle = 0;

    /* Call state, from the header frame and the CCH of each frame */
    uint32_t calledAddr = 0;
    uint32_t ownAddr = 0;
    uint8_t modeCode = 0xFF;
    uint8_t htCode = 0xFF;
    uint16_t code = 0;

    /* Counters for the diagnostics */
    uint32_t framesOk = 0;
    uint32_t framesLost = 0;
    uint32_t headersOk = 0;
    uint32_t endsOk = 0;
    uint32_t dataFrames = 0;

    /*
     * Short data: a voice + slow data call (mode 001) carries two user bytes
     * per frame behind a continuation flag (clause 5.9.1) and they are
     * reassembled here; a voice + appended data call (mode 101) carries only a
     * header in the SLD (clause 5.9.2), whose format and length are kept.  The
     * bytes of that one travel in the TCH of data frames, which this receiver
     * does not decode; docs/RX_DPMR.md says what that would take.
     */
    struct dpmrSldMessage sldMsg;
    uint8_t sldText[DPMR_SLD_TEXT_BYTES];
    uint8_t sldLen = 0;
    bool sldTruncated = false;
    uint32_t sldMessages = 0;
    uint8_t sldFormat = 0;
    uint8_t sldLength = 0;
    uint8_t lastStatus = 0xFF;  /* 5 bit status of the last END frame */
    uint8_t lastWait = 0;
};

#endif /* OPMODE_DPMR_H */
