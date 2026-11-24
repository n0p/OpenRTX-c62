/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_DEFS_H
#define OPENRTX_DMR_DEFS_H

/**
 * @file DmrDefs.h
 * @brief DMR air-interface constants and tables (ETSI TS 102 361-1).
 *
 * Values and tables shared by the DMR receive chain: symbol rate and burst
 * geometry, sync words, the over-the-air voice interleaver and the slot type
 * Golay code.  Table provenance:
 *
 *  - voice de-interleave schedule rW/rX/rY/rZ and the mbelib bit-plane
 *    layout: DSD (src/dmr_voice.c, include/dmr_const.h), ISC licence;
 *  - sync words and Golay(20,8) basis: MMDVM (DMRDefines.h,
 *    DMRSlotType.cpp), GPL-2.0-or-later.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Air-interface geometry, in symbols (each symbol is a dibit). */
enum dmrAir {
    DMR_SYMBOL_RATE = 4800,   ///< Symbols (dibits) per second
    DMR_SLOT_SYMBOLS = 144,   ///< One 30 ms timeslot
    DMR_FRAME_SYMBOLS = 288,  ///< One 60 ms TDMA frame, two slots
    DMR_BURST_SYMBOLS = 132,  ///< 264 bits: 54 + sync 24 + 54
    DMR_SYNC_SYMBOLS = 24,    ///< 48 bit slot sync
    DMR_CACH_SYMBOLS = 12,    ///< 24 bit CACH, before the payload
    DMR_BURST_BITS = 264,     ///< Payload bits of a burst, sync included
    DMR_CACH_BITS = 24,       ///< CACH bits
    DMR_HALF_BITS = 108,      ///< Payload bits on either side of the sync
    DMR_PAYLOAD_BITS = 216,   ///< Three 72-bit AMBE+2 frames
    DMR_INFO_BITS = 196,      ///< Two 98-bit halves, BPTC(196,96) input
    DMR_CSBK_BITS = 96,       ///< CSBK PDU: 80 bits of content + CRC-16
    DMR_AMBE_BITS = 72,       ///< One AMBE+2 voice frame
    DMR_AMBE_BYTES = 9,       ///< Same frame, packed
    DMR_FRAMES_PER_BURST = 3, ///< AMBE+2 frames carried by a voice burst
    DMR_PCM_PER_BURST = 480,  ///< 60 ms of 8 kHz audio
};

/** Received sync word. */
enum dmrSync {
    DMR_SYNC_NONE = 0, ///< No sync
    DMR_SYNC_BS_VOICE, ///< Base station, voice
    DMR_SYNC_BS_DATA,  ///< Base station, data
    DMR_SYNC_MS_VOICE, ///< Mobile station, voice
    DMR_SYNC_MS_DATA,  ///< Mobile station, data
};

/** Burst data types (TS 102 361-1 table 6.6). */
enum dmrDataType {
    DMR_DT_PI_HEADER = 0x0,       ///< Privacy indicator header
    DMR_DT_VOICE_LC_HEADER = 0x1, ///< Voice link control header
    DMR_DT_TERMINATOR_LC = 0x2,   ///< Voice terminator with LC
    DMR_DT_CSBK = 0x3,            ///< Control signalling block
    DMR_DT_MBC_HEADER = 0x4,      ///< Multi-block control header
    DMR_DT_MBC_CONT = 0x5,        ///< Multi-block control continuation
    DMR_DT_DATA_HEADER = 0x6,     ///< Data header
    DMR_DT_RATE_12 = 0x7,         ///< 1/2 rate data
    DMR_DT_RATE_34 = 0x8,         ///< 3/4 rate data
    DMR_DT_IDLE = 0x9,            ///< Idle
    DMR_DT_RATE_1 = 0xA,          ///< Full rate data
    DMR_DT_UNIFIED = 0xB,         ///< Unified single block
};

/** Sync word of each of the four slot formats, MSB first, 48 bits. */
#define DMR_SYNC_WORD_BS_VOICE 0x0000755FD7DF75F7ULL
#define DMR_SYNC_WORD_BS_DATA 0x0000DFF57D75DF5DULL
#define DMR_SYNC_WORD_MS_VOICE 0x00007F7D5DD57DFDULL
#define DMR_SYNC_WORD_MS_DATA 0x0000D5D7F77FD757ULL

/** CACH bit positions of the (7,4) Hamming word {AT,TC,LS1,LS0,H0,H1,H2}. */
extern const uint8_t dmr_cach_tact_pos[7];

/** CACH bit positions of the 17-bit short LC fragment. */
extern const uint8_t dmr_cach_payload_pos[17];

/** Voice de-interleave schedules, 36 dibits per AMBE+2 frame. */
extern const int8_t dmr_rw[36];
extern const int8_t dmr_rx[36];
extern const int8_t dmr_ry[36];
extern const int8_t dmr_rz[36];

/**
 * mbelib @c ambe_fr[4][24] plane to transmission-order bit (0..71), -1 where
 * the plane has no bit: rows 0/1 are the A (24 b) and B (23 b) blocks, rows 2
 * and 3 the 11 + 14 bits of the C block.
 */
extern const int8_t dmr_ambe_layout[4][24];

/** Basis vectors of the Golay(20,8) slot type code. */
extern const uint16_t dmr_golay_basis[8];

/** @return true if the sync word announces a voice burst. */
bool dmr_sync_is_voice(enum dmrSync sync);

/**
 * @return true if the sync word is a base station one: an outbound burst, the
 *         only kind that carries a CACH and therefore a timeslot.
 */
bool dmr_sync_is_bs(enum dmrSync sync);

/**
 * Expand a sync word to its 24 symbol values: the field only carries the two
 * outer levels, so each symbol is +3 or -3.
 *
 * @param sync: sync word.
 * @param out: 24 values of +3 or -3.
 */
void dmr_sync_symbols(enum dmrSync sync, int8_t out[DMR_SYNC_SYMBOLS]);

/**
 * @return a short name for the sync word, for diagnostics.
 */
const char *dmr_sync_name(enum dmrSync sync);

/**
 * @return a short name for the burst data type, for diagnostics.
 */
const char *dmr_data_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_DEFS_H */
