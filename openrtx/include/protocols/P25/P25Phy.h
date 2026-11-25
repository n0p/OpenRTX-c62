/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_P25_PHY_H
#define OPENRTX_P25_PHY_H

/**
 * @file P25Phy.h
 * @brief P25 Phase 1 receive PHY: matched filter, symbol timing, frame sync,
 * NID, and the payload split that the mode consumes.
 *
 * The input is the radio's flat discriminator audio at 48 kHz, the same stream
 * the DMR and TETRA modes take: a C4FM baseband whose carrier offset appears
 * as DC.  The chain is:
 *
 *   RRC alpha 0.2 matched filter, decimated to 24 kHz (5 samples/symbol)
 *     -> symbol clock at 4800 symbol/s, interpolated (4-point Lagrange)
 *     -> 24-symbol frame sync correlation (both polarities, aperture gated)
 *     -> per-frame levels from the sync field, then slicing to dibits
 *     -> NID: BCH(63,16) + DUID parity -> NAC and DUID
 *     -> LDU1/LDU2: 9 IMBE codewords (Golay/Hamming), LCW (Hamming + RS)
 *        TSBK: three trellis blocks, CRC-16, opcode and fields
 *
 * Frames arrive every 180 ms and are contiguous, so once the sync is found the
 * symbol clock is only trimmed frame by frame; a lost frame just costs that
 * frame.  Use p25Phy_reset() on a new frequency.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "protocols/P25/P25Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What one decoded frame carries. */
struct p25Frame
{
    long symbol;              ///< Symbol index of the sync, for traces
    enum p25Duid duid;        ///< Data unit ID of the frame
    uint16_t nac;             ///< Network access code, 0x000 to 0xFFF
    uint16_t spanSymbols;     ///< Symbols cut from the ring, sync included
    int syncErrors;           ///< Sign errors in the 24 sync symbols
    int bchErrors;            ///< Bits corrected in the NID, -1 if undecodable
    bool inverted;            ///< Received with an inverted discriminator
    float evm;                ///< RMS error to the symbol levels, 0..1

    bool lcValid;             ///< Link control of an LDU1 decoded
    int lcRsErrors;           ///< Symbols corrected by the RS(24,12), -1 if bad
    uint8_t lc[9];            ///< The 72-bit link control as nine bytes
    uint8_t lcFormat;         ///< Link control format (0x00 = group voice user)
    uint8_t lcService;        ///< Service options (0x40 = encrypted)
    uint16_t talkgroup;       ///< Group address of the call
    uint32_t source;          ///< Source subscriber unit

    bool hdu;                 ///< Header data unit seen
    uint8_t hduAlgid;         ///< Encryption algorithm of the call, 0x80 = clear
    uint16_t hduKeyid;        ///< Encryption key id
    uint16_t hduGroup;        ///< Talkgroup announced by the header

    uint8_t voiceCount;       ///< IMBE frames decoded (0..9)
    uint8_t voiceBad;         ///< Frames with an uncorrectable word
    int imbeErrors;           ///< Bits corrected over the nine frames
    uint8_t voice[P25_IMBE_PER_LDU][P25_IMBE_BYTES]; ///< 88 parameters each
    bool voiceValid[P25_IMBE_PER_LDU];

    bool tsbkValid;           ///< A TSBK block decoded with a good CRC
    uint8_t tsbk[P25_TSBK_BYTES]; ///< The block as decoded
};

/** Frame callback: called from p25Phy_push(). Keep it short. */
typedef void (*p25FrameHandler)(const struct p25Frame *frame, void *user);

/**
 * Initialise the demodulator.  Call once, before the first push.
 */
void p25Phy_init(void);

/**
 * Drop the state: forget the symbol clock, the sync and the levels.
 */
void p25Phy_reset(void);

/**
 * Feed discriminator audio.
 *
 * @param samples: signed 16-bit samples at 48 kHz.
 * @param count: number of samples.
 * @param handler: called once per decoded frame, may be NULL.
 * @param user: opaque pointer passed to the handler.
 */
void p25Phy_push(const int16_t *samples, size_t count, p25FrameHandler handler,
                 void *user);

/**
 * @return true while a frame has been decoded in the last 300 ms.
 */
bool p25Phy_synced(void);

/**
 * @return the number of frames decoded since the last reset.
 */
uint32_t p25Phy_frames(void);

/**
 * @return how many positions the sync search has examined since the reset.
 *
 * One per candidate symbol position: the demodulator tries a position when it
 * has a whole frame in the ring, so on the grid this is one per frame and off
 * it, one per symbol of ring that has to be walked.  The ratio against
 * p25Phy_frames() is what the frame-length prediction is worth.
 */
uint32_t p25Phy_syncTries(void);

/**
 * @return how many times a frame failed to appear where the previous one
 *         predicted it and the demodulator fell back to scanning the ring.
 */
uint32_t p25Phy_gridMisses(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_P25_PHY_H */
