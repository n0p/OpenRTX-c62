/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_P25_DEFS_H
#define OPENRTX_P25_DEFS_H

/**
 * @file P25Defs.h
 * @brief P25 Phase 1 air-interface constants and frame tables.
 *
 * Phase 1 is a 12.5 kHz FDMA channel: C4FM at 4800 symbols/s (9600 bit/s),
 * root-raised-cosine with alpha = 0.2, four deviations (+-1800 Hz and
 * +-600 Hz).  A frame is 1728 bits (180 ms): a 24-symbol frame sync, a 64-bit
 * NID carrying the 12-bit NAC and the 4-bit DUID, one status symbol every 36
 * symbols, and a payload that depends on the DUID.
 *
 * Everything here is used by both the receiver (P25Phy.c) and the host
 * prototype in work/p25-rx, which is where the numbers were validated against
 * dsd-neo and op25 on real captures.  Sources: TIA-102.BAAA (modulation and
 * symbol mapping), TIA-102.BAAC (NID, framing), TIA-102.BABA (IMBE), and the
 * public implementations listed in docs/RX_P25.md.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Air-interface geometry. */
enum p25Geometry
{
    P25_SYMBOL_RATE = 4800,      ///< Symbols per second
    P25_FRAME_BITS = 1728,       ///< One 180 ms voice frame
    P25_FRAME_SYMBOLS = 864,     ///< Same frame in symbols
    P25_SYNC_SYMBOLS = 24,       ///< Frame sync field
    P25_SYNC_BITS = 48,          ///< Same field in bits
    P25_NID_BITS = 64,           ///< NAC + DUID + BCH parity + parity bit
    P25_NID_SYMBOLS = 32,        ///< Same field in symbols
    P25_SYNC_NID_SYMBOLS = 57,   ///< Sync, NID and the status symbol inside it
    P25_STATUS_SYMBOLS = 24,     ///< One per 36 symbols, never payload
    P25_IMBE_BITS = 144,         ///< One IMBE codeword on the air (7200 bit/s)
    P25_IMBE_PARAMS = 88,        ///< Vocoder parameters inside it (4400 bit/s)
    P25_IMBE_PER_LDU = 9,        ///< 9 x 20 ms = the 180 ms of an LDU
    P25_IMBE_BYTES = 11,         ///< The 88 parameter bits, packed
    P25_LC_HEXBITS = 24,         ///< Low-speed-data symbols in an LDU
    P25_TSBK_BITS = 196,         ///< Trellis-coded block as transmitted
    P25_TSBK_BYTES = 12,         ///< Decoded block, CRC included
    P25_PCM_PER_IMBE = 160,      ///< 20 ms of 8 kHz audio per IMBE frame
};

/** Frame synchronization word and its bit inverse (inverted discriminator). */
#define P25_SYNC_WORD 0x5575F5FF77FFULL
#define P25_SYNC_WORD_INV 0xAA8A0A008800ULL

/** Data unit ID, carried in the NID. */
enum p25Duid
{
    P25_DUID_HDU = 0x0,     ///< Header data unit (encryption sync, talkgroup)
    P25_DUID_TDU = 0x3,     ///< Terminator without link control
    P25_DUID_LDU1 = 0x5,    ///< Voice + link control
    P25_DUID_TSBK = 0x7,    ///< Trunking signalling block (control channel)
    P25_DUID_LDU2 = 0xA,    ///< Voice + encryption sync
    P25_DUID_PDU = 0xC,     ///< Packet data unit
    P25_DUID_TDU15 = 0xF,   ///< Terminator with link control
    P25_DUID_UNKNOWN = 0xFF,
};

/** Bit positions of each field inside the 1728-bit frame (P25Tables.c). */
extern const uint16_t p25_imbe_nid_bits[64];
extern const uint16_t p25_status_bits[48];
extern const uint16_t p25_ldu_ls_bits[240];
extern const uint16_t p25_hdu_bits[658];
extern const uint16_t p25_voice_bits[9][144];

/**
 * Expand the frame sync into 24 symbol signs.
 *
 * The field only carries the two outer levels, so every symbol is +1 or -1.
 *
 * @param out: destination, 24 values of +1 or -1.
 */
void p25_sync_symbols(int8_t out[P25_SYNC_SYMBOLS]);

/**
 * @param duid: data unit ID.
 * @return a short name, for the diagnostics.
 */
const char *p25_duid_name(enum p25Duid duid);

/**
 * @param duid: data unit ID.
 * @return true if it announces a voice frame (LDU1 or LDU2).
 */
bool p25_duid_is_voice(enum p25Duid duid);

/**
 * @param duid: data unit ID.
 * @return the symbols the frame occupies on the air, sync included.
 *
 * A P25 frame is not always 864 symbols: 864 is the voice frame (an LDU is
 * 1728 bits), while a control-channel TSDU is 720 bits (360 symbols), a header
 * 790 bits plus its status symbols (396) and the terminators 144 or 432 bits.
 * The demodulator has to cut the frame it found, and predict the next sync,
 * with this length and not with the longest frame's; see P25Phy.c.  The values
 * below are the ones the reference captures show between two syncs, to within
 * the one symbol the symbol clock drifts over a frame.
 */
int p25_frame_symbols(enum p25Duid duid);

/**
 * Gather bits at the given frame positions into a value, MSB first.
 *
 * @param bits: the frame as one byte per bit, as sliced.
 * @param pos: positions, in transmission order.
 * @param n: number of bits, at most 32.
 * @return the packed value.
 */
uint32_t p25_gather(const uint8_t *bits, const uint16_t *pos, int n);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_P25_DEFS_H */
