/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_P25_H
#define OPMODE_P25_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"

/**
 * P25 Phase 1 receive mode (C4FM, 12.5 kHz channel): takes the radio's flat
 * discriminator output at 48 kHz, demodulates it with protocols/P25/P25Phy.c
 * and decodes the NID, the link control and the TSBK blocks. Voice frames go
 * to the IMBE hook (protocols/P25/P25Vocoder.c) through a playback thread.
 *
 * Receive only: there is no transmitter. PTT does nothing.
 *
 * Needs a 48 kHz RX audio input and a radio driver that sets up a wide, flat
 * receive path for OPMODE_P25 (the same one DMR and TETRA ask for).
 */
class OpMode_P25 : public OpMode
{
public:
    OpMode_P25();
    ~OpMode_P25();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_P25;
    }

    virtual bool rxSquelchOpen() override
    {
        return false;
    }

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

#endif /* OPMODE_P25_H */
