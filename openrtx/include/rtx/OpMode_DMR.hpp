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

/**
 * DMR receive mode: demodulates the radio's flat FM discriminator output
 * (4FSK, 4800 symbols/s) and decodes voice bursts with the AMBE+2 vocoder.
 * Receive only: PTT does nothing in this mode.
 *
 * Needs a 48 kHz RX audio input and a radio driver that sets up a wide, flat
 * receive path for OPMODE_DMR, as the TETRA mode does.
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

    bool startRx;
    unsigned dataErrors; // Consecutive failed reads of the sample stream
    size_t settle;       // Samples still to drop after a start or retune
    pathId basebandPath;
    streamId basebandId;
};

#endif /* OPMODE_DMR_H */
