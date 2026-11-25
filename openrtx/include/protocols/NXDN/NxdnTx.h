/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_NXDN_TX_H
#define OPENRTX_NXDN_TX_H

/**
 * @file NxdnTx.h
 * @brief NXDN48 transmit: the frame of TS 1-A (FSW, LICH, SACCH, four VCHs),
 * the data scrambler, the FEC of the control fields and the 4FSK baseband.
 *
 * One frame is 192 symbols, 80 ms at 2400 symbols/s:
 *
 *   symbol  0..  9  FSW, 10 symbols, hex CDF59, never scrambled
 *   dibit   0..  7  LICH: 7 control bits + even parity
 *   dibit   8.. 37  SACCH: 30 dibits, a rate 1/2 K=5 block punctured to 60
 *                   bits and interleaved in five rows of twelve
 *   dibit  38..181  VCH1..VCH4: 36 dibits each, one AMBE+2 3600x2450 frame
 *                   per 20 ms, packed with the DMR dibit schedule
 *
 * Everything but the FSW is scrambled with the 9-bit LFSR of 4.6 (default
 * seed), which inverts the sign of the symbol.  The dibit convention is the
 * receiver's (NxdnPhy.c, level_to_dibit()): the sign bit is the dibit's high
 * bit and the magnitude bit is the low one, so 0 = -1, 1 = -3, 2 = +1,
 * 3 = +3.  It is *not* the DMR convention, where a set high bit means a
 * negative deviation.
 *
 * The layer 3 messages of the SACCH are the tree's: the 72-bit message split
 * in four 18-bit segments, one per frame of the superframe, with the segment
 * index in bits 7:6 of the SR byte and the RAN in bits 5:0 -- the layout
 * NxdnFec.c and OpMode_NXDN decode, which was checked against a real capture
 * (docs/RX_NXDN.md).
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnFec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Samples per symbol at 48 kHz for 2400 symbols/s. */
#define NXDN_TX_SPS 20

/** Samples of one frame: 192 symbols, 80 ms. */
#define NXDN_TX_FRAME_SAMPLES (NXDN_FRAME_SYMBOLS * NXDN_TX_SPS)

/**
 * Symbols of one write to the output stream: a quarter of a frame, 20 ms.
 * The buffers handed to audioStream_start() are DMA and have to live in SRAM,
 * so the block is kept at the 20 ms of the AMBE frame: two channels of double
 * buffering cost 3.8 KB instead of the 7.7 KB a 40 ms half would.
 */
#define NXDN_TX_HALF_SYMBOLS (NXDN_FRAME_SYMBOLS / 4)

/** A call being built. */
typedef struct {
    uint16_t src;        ///< Own unit ID (16 bits on the air)
    uint16_t dst;        ///< Called unit or talkgroup
    uint8_t ran;         ///< RAN, 0..63, in the SR byte of the SACCH
    uint8_t l3[72];      ///< Current 72-bit layer 3 message, MSB first
    bool scrambler;      ///< 4.6 data scrambler on (conforming) or off
    int seg;             ///< Next SACCH segment, 0..3
    int frames;          ///< Frames built since setup
    bool first;          ///< The next frame is the first of the call
    bool last;           ///< The next frame is the last of the call
} nxdn_tx_t;

/**
 * @brief Start a call: the VCALL message of TS 1-C and the frame state.
 *
 * @param tx call state.
 * @param src own unit ID.
 * @param dst called unit or talkgroup ID.
 * @param ran radio access number, 0..63.
 * @param group true for a group call (call type 0), false for an individual
 *              one (call type 3).
 */
void nxdnTx_setup(nxdn_tx_t *tx, uint16_t src, uint16_t dst, uint8_t ran,
                  bool group);

/** Replace the layer 3 message: 9 octets, octet 0 is the message type. */
void nxdnTx_setMessage(nxdn_tx_t *tx, const uint8_t octets[9]);

/** The transmission release message (TX_REL), for the end of a call. */
void nxdnTx_release(nxdn_tx_t *tx);

/**
 * @brief Build one frame as symbol levels.
 *
 * @param tx call state, advances the SACCH segment and the superframe.
 * @param ambe four canonical AMBE+2 frames (A|B|C, 9 bytes each), or NULL for
 *             a frame with the four VCHs at zero.
 * @param levels 192 symbol levels (+-1, +-3), FSW included.
 */
void nxdnTx_frame(nxdn_tx_t *tx,
                  const uint8_t ambe[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES],
                  int8_t levels[NXDN_FRAME_SYMBOLS]);

/**
 * @brief Build the acquisition preamble as symbol levels.
 *
 * 0x5775FD followed by the +3 +3 -3 -3 pattern of 4.4.3, filling a whole
 * frame's worth of symbols.  Sent before the first frame of a call, unscrambled
 * and with no FSW.
 */
void nxdnTx_preamble(int8_t levels[NXDN_FRAME_SYMBOLS]);

/** @return the level of an NXDN dibit: 0 = -1, 1 = -3, 2 = +1, 3 = +3. */
int8_t nxdnTxDibitLevel(uint8_t dibit);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_NXDN_TX_H */
