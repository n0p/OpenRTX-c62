/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_golay.h
 * @brief Golay(24,12)/Golay(23,12) decoders for the DMR AMBE+2 frame.
 *
 * The 72-bit AMBE+2 voice frame carries two FEC-protected blocks:
 *
 *  - A block (bits 0..23): Golay(24,12), 12 information bits + 12 check bits.
 *  - B block (bits 24..46): Golay(23,12) codeword, XOR-scrambled with a PRNG
 *    mask derived from the decoded A block.
 *  - C block (bits 47..71): 25 unprotected bits (11-bit C2 + 14-bit C3).
 *
 * Both decoders are the same primitive code, Golay(23,12) with
 * g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 (0xC75), and correct up to
 * three bit errors.  The syndrome table in dmr_golay_table.c is generated from
 * the generator polynomial by tools/gen_golay_table.py.
 *
 * This is an independent C implementation of the FEC layer; it carries no code
 * from OpenDMR's C++ wrapper (lib/opendmr/encoder/cgolay24128.cpp) and produces
 * identical results (verified over all 2^23 codewords, see README.md).
 */

#ifndef DMR_GOLAY_H
#define DMR_GOLAY_H

#include <stdint.h>

/** Error pattern (weight <= 3) for each of the 2^11 syndromes. */
extern const uint32_t dmr_golay23127_err[2048];

/**
 * @brief Number of bit errors corrected by the last decode call.
 *
 * Set by dmr_golay23127_decode() and dmr_golay24128_decode().
 */
extern unsigned int dmr_golay_last_errors;

/**
 * @brief Decode a Golay(23,12) codeword.
 *
 * @param code23 23-bit codeword, 12 information bits followed by 11 check bits.
 * @return The 12 information bits, with up to three bit errors corrected.
 */
uint16_t dmr_golay23127_decode(uint32_t code23);

/**
 * @brief Decode a Golay(24,12) codeword.
 *
 * The 24th (parity) bit is dropped before decoding, exactly like the reference
 * OpenDMR implementation.
 *
 * @param code24 24-bit codeword.
 * @return The 12 information bits, with up to three bit errors corrected.
 */
uint16_t dmr_golay24128_decode(uint32_t code24);

#endif /* DMR_GOLAY_H */
