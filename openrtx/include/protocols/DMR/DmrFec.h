/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_FEC_H
#define OPENRTX_DMR_FEC_H

/**
 * @file DmrFec.h
 * @brief DMR channel coding: CACH Hamming(7,4), slot type Golay(20,8),
 * BPTC(196,96), CRC-16 and the voice de-interleaver.
 *
 * Parity definitions follow ETSI TS 102 361-1 annex B; the bit-exact
 * equations and the BPTC interleaver @c (a*181) mod 196 come from op25
 * (hamming.cc, bptc19696.cc), GPL-2.0-or-later / GPL-3.0-or-later.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode and correct the 24-bit CACH in place.
 *
 * @param cach: 24 bits, 0 or 1 per element.
 * @return the number of corrected bits, or -1 if the syndrome is not a single
 * error.
 */
int dmr_fec_cach_decode(uint8_t cach[24]);

/**
 * Compute the three parity bits of a 24-bit CACH in place.
 *
 * @param cach: 24 bits; the {AT,TC,LS1,LS0} positions must be filled.
 */
void dmr_fec_cach_encode(uint8_t cach[24]);

/**
 * Encode 8 slot type bits as a 20-bit Golay(20,8) codeword.
 *
 * @param info: colour code in the high nibble, data type in the low one.
 * @return the 20-bit codeword, information bits first.
 */
uint32_t dmr_fec_golay_encode(uint8_t info);

/**
 * Decode a 20-bit slot type codeword by nearest codeword.
 *
 * @param codeword: 20 received bits.
 * @param info: destination of the 8 decoded bits.
 * @return the Hamming distance to the nearest codeword (0 on success); 20 or
 * more means the word is too damaged to be trusted.
 */
int dmr_fec_golay_decode(uint32_t codeword, uint8_t *info);

/**
 * Decode a BPTC(196,96) block: 15x13 product code, rows Hamming(15,11,3),
 * columns Hamming(13,9,3), iterating until every check closes.
 *
 * @param in: 196 bits in on-air order.
 * @param data: destination of the 96 information bits.
 * @return true if every row and column check closed after correction.
 */
bool dmr_fec_bptc_decode(const uint8_t in[196], uint8_t data[96]);

/**
 * CRC-16 of DMR data blocks, x^16 + x^12 + x^5 + 1, MSB first.
 *
 * @param bits: bits, 0 or 1 per element.
 * @param nbits: number of bits.
 * @return the CRC; a block whose CRC field has been masked leaves zero.
 */
uint16_t dmr_fec_crc16(const uint8_t *bits, int nbits);

/**
 * Undo the over-the-air interleaving of one 72-bit voice frame and pack it in
 * the canonical A|B|C order dmr_vocoder_decode() expects.
 *
 * @param payload: 72 bits in transmission order (36 dibits, MSB first).
 * @param frame: destination, 9 bytes.
 */
void dmr_fec_voice_deinterleave(const uint8_t payload[72], uint8_t frame[9]);

/**
 * Inverse of dmr_fec_voice_deinterleave(), for tests and the bit-exact
 * comparison against the host prototype.
 *
 * @param frame: 9-byte frame in canonical order.
 * @param payload: destination, 72 bits in transmission order.
 */
void dmr_fec_voice_interleave(const uint8_t frame[9], uint8_t payload[72]);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_FEC_H */
