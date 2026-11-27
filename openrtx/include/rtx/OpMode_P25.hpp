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
 * Transmit: with CONFIG_P25_TX the chain in protocols/P25/P25Tx.c keys the
 * radio with the header, the voice frames (when a voice source is installed)
 * and the terminator; without an IMBE encoder it sends HDU + TSBK + TDU, which
 * is a valid transmission with no voice in it (docs/TX_P25.md).
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

    virtual void abortTx() override;

private:
    void reset();
    void startSampling();
    void stopSampling();

#ifdef CONFIG_P25_TX
    bool startTx(const rtxStatus_t *status);
    void stopTx(bool drain = true);
    bool txSlot();
#endif

    bool startRx;
    unsigned dataErrors; // Consecutive failed reads of the sample stream
    size_t settle;       // Samples still to drop after a start or retune
    pathId basebandPath;
    streamId basebandId;

#ifdef CONFIG_P25_TX
    pathId txPath;      ///< Modulating waveform to the transmitter
    streamId txId;
    bool txBlocked;     ///< PTT was pressed and the chain refused to start
    bool txEnded;       ///< The terminator has been asked for
    bool txFirstBlock;  ///< A slot of this over has reached the sink
#endif
};

#endif /* OPMODE_P25_H */
