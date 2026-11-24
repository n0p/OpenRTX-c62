/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_P25_FEC_H
#define OPENRTX_P25_FEC_H

/**
 * @file P25Fec.h
 * @brief Forward error correction of the P25 Phase 1 air interface.
 *
 * Codes used (TIA-102.BAAA, .BAAC, .BABA-A):
 *   - Golay(24,12)/Golay(23,12): IMBE header words, HDU and TDU payloads
 *   - Hamming(15,11): the IMBE amplitude words
 *   - Hamming(10,6): the low-speed data symbols that carry the link control
 *   - BCH(63,16) + parity: the NID (NAC and DUID)
 *   - Reed-Solomon (24,12) and (36,20) over GF(64): link control, HDU
 *   - rate-1/2 four-state trellis: TSBK and PDU blocks
 *
 * The Golay and Hamming decoders exploit the linearity of the code instead of
 * a polynomial division: the syndrome is the XOR of the parity contributions
 * of the set data bits, and every error pattern of weight <= t has its own
 * syndrome, so the pattern is found by matching XORs of the 23 single-bit
 * syndromes.  No table, 46 bytes of state, exact up to t errors.
 *
 * Decoders return the number of corrected symbols (0 = clean) or -1 when the
 * word is not decodable.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encode 12 information bits as a Golay(23,12) codeword.
 *
 * @param info: the 12 bits.
 * @return the codeword, bit 22 is the first transmitted bit.
 */
uint32_t p25Fec_golayEncode(uint32_t info);

/**
 * Correct up to three bit errors in a Golay(23,12) codeword, in place.
 *
 * @param cw: codeword, bit 22 is the first transmitted bit.
 * @return errors corrected, or -1 if the word is not decodable.
 */
int p25Fec_golayDecode(uint32_t *cw);

/**
 * Encode 11 information bits as a Hamming(15,11) codeword.
 *
 * @param info: the 11 bits.
 * @return the codeword, data in bits 14..4, parity in bits 3..0.
 */
uint16_t p25Fec_hamEncode(uint16_t info);

/**
 * Correct a single bit error in a Hamming(15,11) codeword, in place.
 *
 * @param cw: codeword.
 * @return errors corrected, or -1.
 */
int p25Fec_hamDecode(uint16_t *cw);

/**
 * Decode one Hamming(10,6) word.
 *
 * @param data: the six data bits.
 * @param parity: the four parity bits.
 * @return the corrected six data bits, or -1 on two errors.
 */
int p25Fec_ham10Decode(uint32_t data, uint32_t parity);

/**
 * Correct a BCH(63,16) codeword, in place.
 *
 * @param cw: bit i of the value is the coefficient of x^i, i.e. the first
 *            transmitted bit is bit 62.
 * @return errors corrected, or -1.
 */
int p25Fec_bchDecode(uint64_t *cw);

/**
 * Correct one shortened Reed-Solomon codeword over GF(64), in place.
 *
 * @param sym: n symbols, the first one is the highest power.
 * @param n: block length (24 or 36).
 * @param k: information symbols (12 or 20).
 * @return errors corrected, or -1.
 */
int p25Fec_rsDecode(uint8_t *sym, int n, int k);

/**
 * @param sym: n symbols.
 * @param n: block length.
 * @param k: information symbols.
 * @return 0 if the block is a valid codeword, -1 otherwise.
 */
int p25Fec_rsCheck(const uint8_t *sym, int n, int k);

/**
 * De-interleave and trellis-decode one 196-bit TSBK or PDU block.
 *
 * @param bits: the 196 transmitted bits, in order.
 * @param out: the 12 decoded bytes.
 * @return the number of channel bits the Viterbi path had to correct (an
 *         error indicator, not a guarantee), or -1.
 */
int p25Fec_trellisDecode(const uint8_t *bits, uint8_t out[12]);

/**
 * @param data: twelve bytes of a TSBK.
 * @param bytes: number of bytes, 12.
 * @return the CRC-16 of the block; zero means the block is valid.
 */
uint16_t p25Fec_crc16(const uint8_t *data, int bytes);

/* ------------------------------------------------------------------------- */
/* Encoders.  Each one is the exact inverse of the decoder above it, and the  */
/* host test tests/host/p25_tx encodes, transmits, receives and compares, so  */
/* an encoder and its decoder cannot drift apart.                            */
/* ------------------------------------------------------------------------- */

/**
 * Encode 16 information bits as a BCH(63,16) codeword: the NAC and the DUID of
 * the NID.
 *
 * The generator polynomial is the product of the minimal polynomials of
 * alpha^1..alpha^22 over GF(64) (x^6 + x + 1), degree 47, which is the code
 * p25Fec_bchDecode() corrects: every codeword it produces has zero syndromes
 * for those 22 roots.
 *
 * @param info: (NAC << 4) | DUID.
 * @return the 63-bit codeword, bit i = coefficient of x^i, so the first
 *         transmitted bit is bit 62.
 */
uint64_t p25Fec_bchEncode(uint16_t info);

/**
 * Systematic Reed-Solomon encoder over GF(64).
 *
 * @param info: k information symbols, the first one is the highest power.
 * @param k: information symbols (12 or 20).
 * @param cw: destination, n symbols; the first k are the information.
 * @param n: block length (24 or 36).
 */
void p25Fec_rsEncode(const uint8_t *info, int k, uint8_t *cw, int n);

/**
 * Encode one 196-bit TSBK or PDU block: the rate-1/2 four-state trellis, then
 * the block interleaver, exactly what p25Fec_trellisDecode() inverts.
 *
 * @param in: the 12 bytes (the last two are the CRC).
 * @param bits: the 196 transmitted bits, in order.
 */
void p25Fec_trellisEncode(const uint8_t in[12], uint8_t bits[196]);

/**
 * Encode six information bits as a Hamming(10,6) word.
 *
 * @param data: the six bits.
 * @return the word: data in bits 9..4, parity in bits 3..0.
 */
uint16_t p25Fec_ham10Encode(uint32_t data);

/**
 * Encode one header-data-unit word: the 18 bits on the air that carry a
 * six-bit Reed-Solomon symbol.
 *
 * TIA-102.BAAC protects each HDU symbol with the extended Golay(24,12) code
 * used as a (18,6) code: six information bits on the air followed by twelve
 * parity bits (the eleven check bits of the (23,12) code with the six implicit
 * zero data bits, plus the overall parity bit).  The receiver decodes it by
 * nearest codeword, which is exact for a code of minimum distance 8.
 *
 * @param hex: the six-bit symbol, bit 5 is the first transmitted.
 * @param air: destination, 18 bits, one per byte.
 */
void p25Fec_golay18Encode(uint32_t hex, uint8_t air[18]);

/**
 * Decode one 18-bit header-data-unit word.
 *
 * @param air: the 18 bits, one per byte.
 * @param hex: receives the six information bits (bit 5 is the first).
 * @return the number of bits corrected (0..3), or -1 if the word is not within
 *         the correction radius.
 */
int p25Fec_golay18Decode(const uint8_t air[18], uint32_t *hex);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_P25_FEC_H */
