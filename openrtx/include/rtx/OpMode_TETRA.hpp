/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_TETRA_H
#define OPMODE_TETRA_H

#include <stdint.h>
#include <tetra/tetra.h>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "protocols/TETRA/TetraDemod.h"
#include "OpMode.hpp"

/** Samples in one 30 ms half of the baseband stream at 48 kHz */
#define TETRA_HALF_SAMPLES 1440

/**
 * The mode's large state, held in the mode's own object.
 *
 * The modes share one block of memory (rtx/ModeArena.hpp) and only the active
 * one is constructed in it, so everything a mode needs while it runs belongs in
 * its object: this is the 17 KB that used to sit in .bss -- the baseband block
 * the audio stream writes into, libtetra's demodulator and its MAC state --
 * whatever mode was running.
 *
 * None of it may move to PSRAM: `baseband` is a DMA target of
 * audioStream_start() (core/psram_state.h), and libtetra is called from inside
 * the sample loop, where the slow memory would eat the block's budget.
 */
struct tetraModeState
{
    int16_t baseband[2 * TETRA_HALF_SAMPLES];
    struct tetraDemod demod;
    struct tetra_tmo_rx tmo;
};

/**
 * TETRA TMO downlink receive mode: demodulates the radio's flat FM
 * discriminator output (pi/4-DQPSK by integrate-and-dump) and decodes it
 * with libtetra. Receive only: PTT does nothing in this mode.
 *
 * Needs a 48 kHz RX audio input and a radio driver that sets up a wide,
 * flat receive path for OPMODE_TETRA.
 */
class OpMode_TETRA : public OpMode
{
public:
    OpMode_TETRA();
    ~OpMode_TETRA();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_TETRA;
    }

    virtual bool rxSquelchOpen() override
    {
        return false;
    }

private:
    void reset();
    void resync(bool overrun);
    void startSampling();
    void stopSampling();

    bool startRx;
    unsigned dataErrors; // Consecutive failed reads of the sample stream
    size_t settle;       // Samples still to drop after a start or retune
    uint32_t demodJumps; // Demodulator slips and re-acquisitions seen
    pathId basebandPath;
    streamId basebandId;

    /// Baseband block, demodulator and MAC state: lives and dies with the mode
    struct tetraModeState state;
};

#endif /* OPMODE_TETRA_H */
