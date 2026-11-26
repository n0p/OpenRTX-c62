/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DPMR_TX_H
#define OPENRTX_DPMR_TX_H

/**
 * @file DpmrTx.h
 * @brief dPMR transmit (ETSI TS 102 490): header, superframe and END frames,
 * the CCH of every payload frame and the 4FSK baseband.
 *
 * dPMR is FDMA: one carrier, one call, no timeslots (clause 4.2.1).  A
 * transmission is
 *
 *   header frame   192 symbols: P (36) | FS1 (24) | HI0 (60) | CC (12) | HI1 (60)
 *   superframes    four payload frames of 192 symbols:
 *                    FS2 or CC (12) | CCH (36) | TCH (4 x 36)
 *                  frames 1 and 3 carry the FS2 sync, 2 and 4 the channel code
 *   END frame       48 symbols: FS3 (12) | END (36), which is 72 bits: the
 *                  same 36-bit block twice, each scrambled from the reset state
 *
 * All of it is 2400 symbols/s, so every 192-symbol frame is 80 ms and the
 * header and payload frames last the same.  The dibit convention is table
 * 12.1, the one DpmrPhy.c slices with: 01 = +3, 00 = +1, 10 = -1, 11 = -3,
 * i.e. the high bit of the dibit is the *negative* sign and its low bit marks
 * an outer symbol.  DMR uses the same four levels in the same order.
 *
 * The bits of every field are the decoder's, backwards: CRC-7 (CCH, END) or
 * CRC-8 (HI) over the information bits, shortened Hamming (12,8) blocks, the
 * 12x6 / 12x10 interleavers of clause 7.5 and the X^9+X^5+1 scrambler of
 * clause 7.4 preset to all ones.  The voice is four AMBE+2 3600x2450 frames
 * per frame, carried as four interleaved streams of 18 bits -- the same
 * 72-bit frame DMR sends, so lib/dmr_encoder is the encoder.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/DPMR/DpmrDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Samples per symbol at 48 kHz for 2400 symbols/s. */
#define DPMR_TX_SPS 20

/** Samples of a payload or header frame: 192 symbols, 80 ms. */
#define DPMR_TX_FRAME_SAMPLES (DPMR_FRAME_SYMBOLS * DPMR_TX_SPS)

/** Samples of the END frame: 48 symbols, 20 ms. */
#define DPMR_TX_END_SAMPLES (DPMR_END_SYMBOLS * DPMR_TX_SPS)

/** Symbols of the END frame. */
#define DPMR_TX_END_SYMBOLS DPMR_END_SYMBOLS

/** A call being built. */
typedef struct {
    uint32_t ownId;      ///< 24-bit own address
    uint32_t calledId;   ///< 24-bit called address
    uint16_t channelCode;///< 12-bit channel code of the frequency, clause 6.1.5
    uint8_t mode;        ///< enum dpmrMode
    uint8_t version;     ///< 0 standard, 3 manufacturer
    uint8_t format;      ///< enum dpmrFormat
    int fn;              ///< Next payload frame of the superframe, 0..3
} dpmr_tx_t;

/**
 * @brief Start a transmission: addresses, channel code and the field values
 * every frame repeats.
 *
 * @param tx call state.
 * @param ownId own 24-bit address.
 * @param calledId called 24-bit address.
 * @param channelCode 12-bit channel code of the frequency
 *                    (dpmrDefs_channelCodeBits()), or 0 for none.
 */
void dpmrTx_setup(dpmr_tx_t *tx, uint32_t ownId, uint32_t calledId,
                  uint16_t channelCode);

/** Set the call type: mode (enum dpmrMode), version, format. */
void dpmrTx_setCallType(dpmr_tx_t *tx, uint8_t mode, uint8_t version,
                        uint8_t format);

/**
 * @brief Fill a frame's worth of preamble symbols (clause 6.1.6: the 0101 1111
 * pattern, repeated).
 *
 * A transmission opens with as much preamble as the transmitter wants -- the
 * standard only asks for 72 bits before FS1 -- and a receiver that has to find
 * FS1 by correlation needs a few hundred milliseconds of it to ride out the
 * acquisitions it makes on the preamble itself.  The mode sends two frames of
 * this (160 ms) before the header.
 */
void dpmrTx_preamble(int8_t levels[DPMR_FRAME_SYMBOLS]);

/**
 * @brief Build the header frame: preamble, FS1, HI0, channel code and HI1.
 *
 * @param tx call state.
 * @param ht header type (DPMR_HT_START for the start of a call).
 * @param ci 11-bit call information field.
 * @param levels 192 symbol levels (+-1, +-3).
 */
void dpmrTx_header(dpmr_tx_t *tx, uint8_t ht, uint16_t ci,
                   int8_t levels[DPMR_FRAME_SYMBOLS]);

/**
 * @brief Build the next payload frame of the superframe.
 *
 * @param tx call state, advances the frame number.
 * @param ambe four canonical AMBE+2 frames (A|B|C, 9 bytes each), or NULL for
 *             a frame with the four TCH groups at zero.
 * @param levels 192 symbol levels.
 */
void dpmrTx_frame(dpmr_tx_t *tx,
                  const uint8_t ambe[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES],
                  int8_t levels[DPMR_FRAME_SYMBOLS]);

/**
 * @brief Build the END frame.
 *
 * @param tx call state (unused, kept for symmetry).
 * @param status 5-bit status message, or -1 for none (end type 0).
 * @param levels 48 symbol levels.
 */
void dpmrTx_end(dpmr_tx_t *tx, int status, int8_t levels[DPMR_TX_END_SYMBOLS]);

/** @return the level of a dPMR dibit: 0 = +1, 1 = +3, 2 = -1, 3 = -3. */
int8_t dpmrTxDibitLevel(uint8_t dibit);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DPMR_TX_H */
