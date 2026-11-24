/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_YSF_PHY_H
#define OPENRTX_YSF_PHY_H

/**
 * @file YsfPhy.h
 * @brief Yaesu System Fusion receive PHY: symbol timing, frame sync and the
 * 4-level slicer.
 *
 * The input is the radio's flat discriminator audio at 48 kHz -- the same
 * stream M17 and DMR take -- carrying C4FM: four deviation levels, 4800
 * symbols/s, two bits per symbol, 9600 bit/s in 12.5 kHz.
 *
 *   raw 48 kHz discriminator (10 samples per symbol, exactly)
 *     -> frame sync on the sign pattern of the 20 sync symbols
 *        (0x7FFD... / 0xD471C9634D, tolerant to 3 of 20 symbols)
 *     -> per-frame fractional timing from the sync correlation,
 *        coasting on the last good offset for a few frames
 *     -> per-frame DC, inner and outer levels from the magnitude median,
 *        which is what makes the slicer independent of deviation and gain
 *     -> 480 dibits per 100 ms frame, handed to the callback
 *
 * Nothing here is System Fusion specific beyond the sync word and the frame
 * length, so the block is small: no matched filter (the radio's IF filter
 * already shapes the eye, as in DmrPhy), no AGC, no AFC.
 *
 * Dibit values follow the air interface, not the symbol voltage:
 *   00 -> +1, 01 -> +3, 10 -> -1, 11 -> -3
 * which is the polarity a real Yaesu transmitter uses (verified on an off-air
 * capture, see docs/RX_FUSION.md).  The sign of a sync symbol is therefore the
 * MSB of its dibit, which is what the correlation compares.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Symbols in one frame: 100 ms at 4800 symbols/s. */
#define YSF_FRAME_SYMBOLS 480

/** Dibits in one frame: one per symbol, two bits each. */
#define YSF_FRAME_DIBITS 480

/** Symbols of the frame sync word. */
#define YSF_SYNC_SYMBOLS 20

/** One received frame. */
struct ysfFrame
{
    uint8_t dibits[YSF_FRAME_DIBITS]; ///< 480 dibits, sync first
    uint8_t syncErrors;               ///< Sync symbols that did not match
    uint8_t evm;                      ///< Payload error, percent of the level
};

/**
 * Frame callback.  Runs in the caller's thread (the RTX thread), 100 ms of
 * samples after the frame's sync.
 */
typedef void (*ysfFrameHandler)(const struct ysfFrame *frame, void *user);

/** Initialise the demodulator.  Call once. */
void ysfPhy_init(void);

/** Drop sync and start hunting again. */
void ysfPhy_reset(void);

/** Is a System Fusion carrier being tracked right now? */
bool ysfPhy_synced(void);

/**
 * Push discriminator samples.
 *
 * @param samples: 16-bit samples at 48 kHz.
 * @param count: number of samples.
 * @param handler: called once per decoded frame.
 * @param user: opaque pointer for the handler.
 */
void ysfPhy_push(const int16_t *samples, size_t count, ysfFrameHandler handler,
                 void *user);

#endif /* OPENRTX_YSF_PHY_H */
