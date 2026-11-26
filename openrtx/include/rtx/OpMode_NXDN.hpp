/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_OPMODE_NXDN_HPP
#define OPENRTX_OPMODE_NXDN_HPP

/**
 * @file OpMode_NXDN.hpp
 * @brief NXDN receive mode: 4FSK demodulation of the discriminator output,
 * frame sync, LICH, SACCH and voice through the AMBE+2 decoder.
 *
 * Transmit (CONFIG_NXDN_TX): the frame builders of protocols/NXDN/NxdnTx.c fed
 * by the AMBE+2 encoder, shaped by the same 4FSK chain, 40 ms at a time.  The
 * call is a mobile station's: a preamble, the frames of the superframe with
 * their SACCH segments, then a release message.  See docs/TX_NXDN.md.
 */

#include <stdint.h>

#include "core/state.h"
#include "rtx/OpMode.hpp"
#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnFec.h"

/*
 * The transmitter only exists with CONFIG_NXDN_TX: without it the modulator,
 * the baseband stream and the PTT branch are compiled out and PTT in this mode
 * does nothing, exactly as in DMR without CONFIG_DMR_TX.
 */
#ifdef CONFIG_NXDN_TX
#include "protocols/Fsk4Tx.h"
#include "protocols/NXDN/NxdnTx.h"

/** AMBE+2 frames queued for transmission: two frames of margin. */
#define NXDN_TX_QUEUE_FRAMES 8

/** One 20 ms half of the baseband stream: 48 symbols at 48 kHz. */
#define NXDN_TX_HALF_SAMPLES (NXDN_TX_HALF_SYMBOLS * NXDN_TX_SPS)

/** Halves per frame. */
#define NXDN_TX_FRAME_HALVES 4
#endif

class OpMode_NXDN : public OpMode
{
public:
    /**
     * @brief Instantiate the NXDN receive mode.
     *
     * @param channel: 0 for 12.5 kHz, 1 for 6.25 kHz.
     */
    explicit OpMode_NXDN();

    void enable() override;
    void disable() override;
    void update(rtxStatus_t *const status, const bool newCfg) override;
    bool rxSquelchOpen() override;

private:
    static void frameTrampoline(const struct nxdnFrame *frame, void *user);
    void onFrame(const struct nxdnFrame *frame);
    void startSampling();
    void stopSampling();

#ifdef CONFIG_NXDN_TX
    bool startBaseband(const rtxStatus_t *status);
    bool startTx(const rtxStatus_t *status);
    void stopTx();
    bool fillMic(unsigned frames);
    bool putHalf();
    bool txSlot(const rtxStatus_t *status);

    nxdn_tx_t tx;
    fsk4_mod_t txMod;
    int8_t levelsBuf[NXDN_FRAME_SYMBOLS];
    uint8_t frameQueue[NXDN_TX_QUEUE_FRAMES][NXDN_AMBE_BYTES];
    unsigned frameHead;
    unsigned frameCount;
    int txStage;        ///< 0 preamble, 1 voice, 2 release, 3 tail
    int txHalf;         ///< 0..3, quarter of the frame being sent
    int txPreamble;     ///< Preamble halves left
    int txReleaseFrames;///< Frames of the release superframe sent
    int txTail;         ///< Halves of tail sent
    bool txRunning;
    bool txFirstBlock;
    int txPath, txId, micPath, micId;
    stream_sample_t txSamples[2 * NXDN_TX_HALF_SAMPLES];
    int16_t micPcm[2 * 160];
#endif

    int basebandPath;
    int basebandId;
    int16_t settle;
    int dataErrors;
    int lastRan;        ///< RAN of the last superframe, -1 until one decodes
    bool startRx;
};

#endif /* OPENRTX_OPMODE_NXDN_HPP */
