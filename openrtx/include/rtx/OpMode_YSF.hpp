/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_YSF_H
#define OPMODE_YSF_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"

/**
 * Yaesu System Fusion (C4FM) receive mode.
 *
 * Takes the radio's flat discriminator output, decodes the 100 ms frames --
 * FICH, data channel, voice -- and plays the AMBE+2 voice of V/D mode 1 and
 * V/D mode 2.  Voice FR (the full-rate "wide" mode) needs a 7200 bit/s IMBE
 * decoder this build does not have, so those calls are counted and skipped
 * instead of being played as noise.
 *
 * Transmit: with CONFIG_YSF_TX the chain in protocols/YSF/YsfTx.c keys the
 * radio with the header, V/D mode 1 voice frames and the terminator.  The
 * voice source is the AMBE+2 encoder the DMR side already carries
 * (lib/dmr_encoder, 1.43 M instructions per 20 ms frame), read from the
 * microphone; without that encoder the voice slots go out as zeros, which is
 * a valid frame with no speech in it (docs/TX_YSF.md).
 */
class OpMode_YSF : public OpMode
{
public:
    OpMode_YSF();
    ~OpMode_YSF();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_YSF;
    }

    virtual bool rxSquelchOpen() override;

    virtual void abortTx() override;

private:
    void reset();
    void startSampling();
    void stopSampling();

    static void frameTrampoline(const struct ysfFrame *frame, void *user);
    void onFrame(const struct ysfFrame *frame);

#ifdef CONFIG_YSF_TX
    bool startTx(const rtxStatus_t *status);
    void stopTx();
    bool txSlot();
    static int voiceTrampoline(uint8_t frame[9], void *user);
    bool fillMic(unsigned slots);
#endif

    bool startRx;
    unsigned dataErrors; ///< Consecutive failed reads of the sample stream
    size_t settle;       ///< Samples still to drop after a start or retune
    pathId basebandPath;
    streamId basebandId;

#ifdef CONFIG_YSF_TX
    pathId txPath;      ///< Modulating waveform to the transmitter
    streamId txId;
    pathId micPath;     ///< Microphone, 8 kHz, for the vocoder
    streamId micId;
    bool txBlocked;
    bool txEnded;
#endif
};

#endif /* OPMODE_YSF_H */
