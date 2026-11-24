/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DSTAR_PHY_H
#define OPENRTX_DSTAR_PHY_H

/**
 * @file DStarPhy.h
 * @brief D-STAR DV receive PHY: GMSK demodulation of discriminator audio,
 * bit and frame synchronisation, header and slow-data extraction.
 *
 * The input is the radio's discriminator audio at 48 kHz, the same stream the
 * M17, TETRA and DMR modes take: a GMSK baseband, i.e. filtered NRZ whose
 * positive deviation is a 1 (JARL STD-7 Ap1.5).  The chain is:
 *
 *   Gaussian matched filter, BT 0.5, decimated to 24 kHz (5 samples/symbol)
 *     -> block symbol timing: 5 phases x 3 fractional offsets, the widest eye
 *        wins (64 symbols per block, so the clock is re-measured every 13 ms)
 *     -> slice + soft value per bit
 *     -> 15-bit frame sync hunt (Hamming distance 2 on hard bits)
 *     -> 660 soft bits -> header (de-scramble, de-interleave, Viterbi, P_FCS)
 *     -> 96-bit frames: 72 voice bits + 24 slow bits, the resync word every
 *        21st frame in place of the slow data, the end pattern as terminator
 *
 * The demodulator is streaming: push whatever the audio stream gives.  The
 * callbacks run in the pushing thread, so they must be short (queue, do not
 * decode).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/DSTAR/DStarDefs.h"
#include "protocols/DSTAR/DStarFec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One decoded DV frame. */
struct dstarPhyFrame {
    uint32_t index;    ///< Frame number since the header, 0 is the first
    bool sync;         ///< Carried the resync word instead of slow data
    uint8_t voice[DSTAR_VOICE_BYTES]; ///< 72 voice bits, air order (LSB first)
    uint8_t slow[DSTAR_SLOW_BYTES];   ///< 24 slow-data bits, de-scrambled, air order
    int8_t level;      ///< Mean |soft| of the frame, a rough SNR proxy (0..127)
};

/**
 * @brief Header callback: a radio header decoded with a good P_FCS.
 *
 * @param hdr: the header (text fields filled, crcOk true).
 * @param syncErrors: bit errors in the 15-bit frame sync.
 * @param user: opaque pointer from dstarPhy_push().
 */
typedef void (*dstarHeaderHandler)(const struct dstarHeader *hdr, int syncErrors,
                                   void *user);

/**
 * @brief Frame callback: one 96-bit DV frame.
 */
typedef void (*dstarFrameHandler)(const struct dstarPhyFrame *frame, void *user);

/**
 * @brief End-of-transmission callback: the end pattern was found or the
 * carrier went away.
 */
typedef void (*dstarEotHandler)(void *user);

/** Initialise the demodulator.  Call once, before the first push. */
void dstarPhy_init(void);

/** Drop the state: forget the sync, the timing and the frame counter. */
void dstarPhy_reset(void);

/**
 * @brief Feed discriminator audio.
 *
 * @param samples: signed 16-bit samples at 48 kHz.
 * @param count: number of samples.
 * @param hh: header callback, may be NULL.
 * @param fh: frame callback, may be NULL.
 * @param eh: end-of-transmission callback, may be NULL.
 * @param user: opaque pointer passed to the callbacks.
 */
void dstarPhy_push(const int16_t *samples, size_t count, dstarHeaderHandler hh,
                   dstarFrameHandler fh, dstarEotHandler eh, void *user);

/** @return true while a transmission has been decoded in the last 200 ms. */
bool dstarPhy_synced(void);

/** @return the demodulator state as a number, for diagnostics. */
int dstarPhy_state(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DSTAR_PHY_H */
