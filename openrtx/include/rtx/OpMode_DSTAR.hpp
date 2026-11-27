/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DSTAR_H
#define OPMODE_DSTAR_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"

struct dstarHeader;
struct dstarPhyFrame;

/**
 * D-STAR DV mode: receives the radio's flat FM discriminator output (GMSK at
 * 4800 bit/s in a 6.25 kHz channel) and decodes the radio header, the voice
 * frames and the slow-data channel (20-character message, position sentences),
 * which is where the call signs, the message and the GPS position come from.
 *
 * On PTT it transmits a DV call: bit sync, frame sync, the coded header built
 * from the operator's callsign and the channel's repeater fields, then frames of
 * voice and slow data until the PTT is released, then the end pattern.  The
 * voice payload is the D-STAR null frame unless a 3600x2400 encoder is linked in
 * (dstarVocoder_*), exactly as an Icom radio behaves with no microphone input;
 * header, slow data, framing and modulation are real and verified against the
 * receiver (docs/DSTAR_TX.md).
 *
 * The mode needs `CONFIG_C62_DSTAR` (see platform/targets/c62/Kconfig.board).
 */
class OpMode_DSTAR : public OpMode
{
public:
    OpMode_DSTAR();
    ~OpMode_DSTAR();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;
    virtual bool rxSquelchOpen() override;

    /** Force the end of the transmission (PTT supervisor / mode change). */
    virtual void abortTx() override;

private:
    static void headerTrampoline(const struct dstarHeader *hdr, int syncErrors,
                                 void *user);
    static void frameTrampoline(const struct dstarPhyFrame *frame, void *user);
    static void eotTrampoline(void *user);

    void onHeader(const struct dstarHeader *hdr, int syncErrors);
    void onFrame(const struct dstarPhyFrame *frame);
    void onEot();

    void startSampling();
    void stopSampling();
    bool startTx(const rtxStatus_t *status);
    void stopTx(bool drain = true);
    bool txSlot();

    int basebandPath;
    int basebandId;
    int txPath;
    int txId;

    bool startRx;
    bool txBlocked;
    bool txEnded;
    bool txFirstBlock;  ///< A block of this over has reached the sink
    uint32_t txFrames;

    /*
     * One buffer for both directions: the radio is half duplex, so a receive
     * block and a transmit block can never be in flight at the same time, and
     * sharing it keeps 5,7 kB of .bss out of a RAM budget that is tight.
     */
    int16_t audioBuffer[2 * 1440];
};

#endif /* OPMODE_DSTAR_H */
