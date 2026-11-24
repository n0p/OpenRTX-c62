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
 * Receive only: System Fusion needs a C4FM transmitter and an AMBE+2 encoder
 * at 2450 bit/s, and PTT in this mode does nothing at all rather than keying
 * a PA with a waveform the radio cannot make.
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

private:
    void reset();
    void startSampling();
    void stopSampling();

    static void frameTrampoline(const struct ysfFrame *frame, void *user);
    void onFrame(const struct ysfFrame *frame);

    bool startRx;
    unsigned dataErrors; ///< Consecutive failed reads of the sample stream
    size_t settle;       ///< Samples still to drop after a start or retune
    pathId basebandPath;
    streamId basebandId;
};

#endif /* OPMODE_YSF_H */
