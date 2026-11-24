/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTOCOLS_DPMR_DPMRPHY_H
#define PROTOCOLS_DPMR_DPMRPHY_H

/**
 * @file DpmrPhy.h
 * @brief dPMR physical layer: 4FSK at 2400 symbols/s, 6,25 kHz channel.
 *
 * The receiver takes the radio's flat FM discriminator output (signed 16 bit
 * samples at 48 kHz, the same stream the DMR and M17 modes use), applies the
 * square root raised cosine matched filter of ETSI TS 102 490 clause 12.2.2.2
 * (alpha = 0.2) decimated to 4 samples per symbol, finds the frame syncs of
 * clause 6.1 (FS1, FS2, FS3) and hands whole frames to the mode.
 *
 * There is no timeslot structure to keep: dPMR is FDMA.  A transmission is a
 * header frame, then superframes of four 80 ms frames, then an end frame, all
 * of them 384 bits (192 symbols) long except the 96 bit end frame.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "protocols/DPMR/DpmrDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Input sample rate of dpmrPhy_push(): 48 kHz, like every flat RX mode. */
#define DPMR_PHY_IN_RATE 48000

/** Samples per symbol at the demodulator's internal rate (4 sps). */
#define DPMR_PHY_SPS 4

/** Internal rate: 2400 symbols/s * 4. */
#define DPMR_PHY_RATE (2400 * DPMR_PHY_SPS)

/** Frame handler: called from the thread that pushes samples. */
typedef void (*dpmrFrameHandler)(const struct dpmrFrame *frame, void *user);

/** Counters of the demodulator, for the diagnostics screen and the tests. */
struct dpmrPhyStats
{
    uint32_t frames;            /**< Frames emitted                    */
    uint32_t cchOk;             /**< Frames whose CCH CRC-7 closed     */
    uint32_t cchFail;           /**< Frames whose CCH CRC-7 did not    */
    uint32_t voice;             /**< Frames that carried AMBE+2 frames */
    uint32_t headers;           /**< Header frames with a valid HI0    */
    uint32_t ends;              /**< End frames with a valid half      */
    uint32_t syncLost;          /**< Times the frame grid was dropped  */
    uint32_t resyncs;           /**< Sync acquisitions after the first */
    float    evm;               /**< Mean normalised level error       */
};

void dpmrPhy_init(void);
void dpmrPhy_reset(void);

/**
 * Push baseband samples.
 *
 * @param samples discriminator output, signed 16 bit, 48 kHz
 * @param count   number of samples
 * @param handler called once per decoded frame, may be NULL
 * @param user    opaque pointer passed to the handler
 */
void dpmrPhy_push(const int16_t *samples, size_t count,
                  dpmrFrameHandler handler, void *user);

/**
 * True while a transmission is being followed: set by the first decoded frame
 * and cleared when the frame grid has been lost for more than 400 ms.  This is
 * the squelch of the mode.
 */
bool dpmrPhy_synced(void);

/** Snapshot of the counters. */
void dpmrPhy_stats(struct dpmrPhyStats *out);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOLS_DPMR_DPMRPHY_H */
