/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_NXDN_PHY_H
#define OPENRTX_NXDN_PHY_H

/**
 * @file NxdnPhy.h
 * @brief NXDN receive PHY: matched filter, symbol timing, frame sync, LICH,
 * SACCH and the voice/data split of a frame.
 *
 * The input is the radio's discriminator audio at 48 kHz, exactly what the DMR
 * mode takes.  NXDN48 is 2400 symbols/s with an RRC of alpha 0.2, which is the
 * *same* pulse shape and the same symbol rate as DMR, so the demodulator is
 * the DMR one: an 81-tap matched filter evaluated at 24 kHz (five samples per
 * symbol), a coarse aperture pre-gate, per-frame fractional timing from the
 * sync correlation, a four-level fit and a quality gate.
 *
 * What changes is the frame: 192 symbols, one 10-symbol frame sync word at the
 * head of every frame (no separate voice and data words, no CACH), 80 ms
 * apart.  A frame is handed to the callback as soon as its last symbol is in,
 * ~80 ms after its sync -- the fourth voice channel is at the end of the frame
 * and there is nothing to emit before it.
 *
 * Use nxdnPhy_reset() on a new frequency or after losing sync.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnFec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What one decoded frame carries. */
struct nxdnFrame {
    long symbol;              ///< Symbol index of the frame sync, for traces
    int syncErrors;           ///< Sign errors in the 10 sync symbols
    float score;              ///< Normalised sync correlation, 0..1
    float aperture;           ///< Fraction of the sync window at outer levels
    float evm;                ///< RMS error to the symbol levels, 0..1
    bool inverted;            ///< Decoded from an inverted discriminator
    bool lichOk;              ///< LICH parity matched
    struct nxdnLich lich;     ///< The LICH fields
    int voiceFirst;           ///< First VCH that carries voice (0 or 2)
    int voiceCount;           ///< VCHs that carry voice (0, 2 or 4)
    uint8_t frameCount;       ///< AMBE+2 frames decoded, 0..4
    uint8_t frame[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES]; ///< Canonical A|B|C
    bool sacchOk;             ///< The SACCH block passed its CRC-6
    uint8_t sacchSr;          ///< The 8 SR bits of the block
    uint8_t sacchBits[18];    ///< Its 18 layer 3 bits
    uint8_t dibits[NXDN_LICH_DIBITS + NXDN_SACCH_DIBITS
                   + NXDN_VCH_PER_FRAME * NXDN_VCH_DIBITS]; ///< Descrambled
};

/**
 * Frame callback.  Called from nxdnPhy_push(), so from the thread that pushes
 * samples: queue the frames, do not decode them here.
 */
typedef void (*nxdnFrameHandler)(const struct nxdnFrame *frame, void *user);

/** Initialise the demodulator and reset its state.  Call once. */
void nxdnPhy_init(void);

/** Drop the state: the sync, the timing phase and the level estimates. */
void nxdnPhy_reset(void);

/**
 * @brief Feed discriminator audio.
 *
 * @param samples signed 16-bit samples at 48 kHz.
 * @param count   number of samples.
 * @param handler called once per decoded frame, may be NULL.
 * @param user    opaque pointer passed to the handler.
 */
void nxdnPhy_push(const int16_t *samples, size_t count, nxdnFrameHandler handler,
                  void *user);

/** @return the coarse sampling phase in use, in 48 kHz samples (0..8, even). */
int nxdnPhy_phase(void);

/** @return true while a frame has been decoded in the last 300 ms. */
bool nxdnPhy_synced(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_NXDN_PHY_H */
