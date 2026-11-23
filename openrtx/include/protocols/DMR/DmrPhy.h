/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_PHY_H
#define OPENRTX_DMR_PHY_H

/**
 * @file DmrPhy.h
 * @brief DMR receive PHY: matched filter, symbol timing, sync, CACH and the
 * voice/data burst split.
 *
 * The input is the radio's discriminator audio at 48 kHz, the same stream the
 * TETRA mode takes: a 4FSK baseband whose carrier offset appears as DC.  The
 * chain is the one validated by the host prototype (work/dmr-rx):
 *
 *   RRC a=0.2 matched filter, decimated to 2 samples/symbol
 *     -> sync search on the 4 slot words (24 symbols, outer levels)
 *     -> per-burst fractional timing from the data eye
 *     -> levels and DC from the 24 sync symbols
 *     -> CACH (Hamming 7,4), slot type (Golay 20,8), BPTC(196,96)
 *     -> voice: three 72-bit AMBE+2 frames, de-interleaved
 *
 * The demodulator is streaming: push whatever the audio stream gives.  Each
 * burst is handed to a callback as soon as its last symbol is in, ~16 ms after
 * its sync.  Use dmrPhy_reset() on a new frequency or after losing sync.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/DMR/DmrDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What one decoded burst carries. */
struct dmrBurst {
    long symbol;                  ///< Symbol index of the sync, for traces
    enum dmrSync sync;            ///< Sync word found
    bool voice;                   ///< Voice burst (BS/MS voice sync)
    int syncErrors;               ///< Sign errors in the 24 sync symbols
    float evm;                    ///< RMS error to the symbol levels, 0..1
    bool cachValid;               ///< CACH decoded with at most one error
    uint8_t accessType;           ///< CACH AT
    bool timeslot2;               ///< CACH TC: false = TS1, true = TS2
    bool lateEntry;               ///< CACH LS
    bool slotTypeValid;           ///< Colour code and data type are valid
    uint8_t colourCode;           ///< Colour code of the burst
    uint8_t dataType;             ///< Burst data type (data bursts only)
    bool crcChecked;              ///< Data burst with a known CRC mask
    bool crcOk;                   ///< ... and it passed
    uint8_t frameCount;           ///< AMBE+2 frames (voice bursts), 0..3
    uint8_t frame[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES]; ///< Canonical A|B|C
    uint8_t lc[12];               ///< Data bursts: the 96-bit block
};

/**
 * Burst callback.  Called from dmrPhy_push(), so from the thread that pushes
 * samples: keep it short (queue the frames, do not decode them here).
 */
typedef void (*dmrBurstHandler)(const struct dmrBurst *burst, void *user);

/**
 * Initialise the demodulator and reset its state.  Call once, before the
 * first push.
 */
void dmrPhy_init(void);

/**
 * Drop the state: forget the sync, the timing phase and the level estimates.
 */
void dmrPhy_reset(void);

/**
 * Feed discriminator audio.
 *
 * @param samples: signed 16-bit samples at 48 kHz.
 * @param count: number of samples.
 * @param handler: called once per decoded burst, may be NULL.
 * @param user: opaque pointer passed to the handler.
 */
void dmrPhy_push(const int16_t *samples, size_t count, dmrBurstHandler handler,
                 void *user);

/**
 * @return the coarse sampling phase in use, in 48 kHz samples (0..8, even).
 */
int dmrPhy_phase(void);

/**
 * @return true while a burst has been decoded in the last 300 ms.
 */
bool dmrPhy_synced(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_PHY_H */
