/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DSTAR_FEC_H
#define OPENRTX_DSTAR_FEC_H

/**
 * @file DStarFec.h
 * @brief D-STAR radio header: packing, CRC-CCITT, rate-1/2 K=3 convolutional
 * coding, the 24-column interleaver and the PN scrambler (JARL STD-7 Ap1, Ap2).
 *
 * The header is the only protected part of a DV transmission: 41 bytes
 * (3 flags + RPT2 + RPT1 + UR + MY + suffix + P_FCS) are coded to 660 on-air
 * bits.  Losing it loses the whole call, which is why it gets 2 dB of coding
 * gain and a 24-column interleave against burst errors.
 *
 * Bit order: on air, every byte is transmitted LSB first (STD-7 Ap1.4) and the
 * PN sequence is XORed into the coded bit stream in transmission order.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/DSTAR/DStarDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A decoded (or to-be-encoded) radio header, as text. */
struct dstarHeader {
    uint8_t flags[3];  ///< Flag 1, 2, 3 (STD-7 4.1.1 c..e)
    char rpt2[DSTAR_CALL_LEN + 1]; ///< Destination repeater, spaces trimmed
    char rpt1[DSTAR_CALL_LEN + 1]; ///< Source repeater
    char ur[DSTAR_CALL_LEN + 1];   ///< Other station ("CQCQCQ" for a general call)
    char my[DSTAR_CALL_LEN + 1];   ///< Own callsign
    char sfx[DSTAR_SUFFIX_LEN + 1];///< Own callsign suffix
    bool crcOk;        ///< P_FCS checked out (decode only)
    int8_t viterbi;    ///< Coded bits the Viterbi had to change (decode only)
};

/**
 * @brief CRC-16/X.25 as D-STAR uses it: reflected 0x8408, init 0xFFFF, final
 * inversion (STD-7 4.1.1 k, "P_FCS").
 */
uint16_t dstarCrc16(const uint8_t *data, size_t len);

/**
 * @brief Serialise a header into 41 bytes and append its P_FCS.
 *
 * Callsign fields are cut or space padded to their fixed width and upper
 * cased, as a radio does.
 */
void dstarHeaderPack(const struct dstarHeader *hdr, uint8_t out[DSTAR_HEADER_BYTES]);

/**
 * @brief Parse a 41-byte header into text fields (does not check the CRC).
 */
void dstarHeaderUnpack(const uint8_t in[DSTAR_HEADER_BYTES], struct dstarHeader *hdr);

/**
 * @brief True when the P_FCS of a 41-byte header matches.
 */
bool dstarHeaderCheck(const uint8_t in[DSTAR_HEADER_BYTES]);

/**
 * @brief Code a 41-byte header into the 660 on-air bits.
 *
 * Convolutional rate 1/2, K=3 (generators 7 and 5), 24-column interleave,
 * PN scrambler x^7 + x^4 + 1 from an all-ones seed.
 *
 * @param hdr: 41 bytes with a valid P_FCS.
 * @param bits: 660 on-air bits, one per byte, in transmission order.
 */
void dstarHeaderEncode(const uint8_t hdr[DSTAR_HEADER_BYTES],
                       uint8_t bits[DSTAR_CODED_BITS]);

/**
 * @brief Decode 660 soft bits into a header.
 *
 * The soft values are the matched-filter output of each on-air bit, positive
 * for a 1, so the Viterbi can weigh them (hard decisions are what you get by
 * passing +-1).  A successful return means the P_FCS matched.
 *
 * @param soft: 660 soft values.
 * @param hdr: destination.
 * @return true when the CRC checked out; the struct is filled either way.
 */
bool dstarHeaderDecode(const int8_t soft[DSTAR_CODED_BITS], struct dstarHeader *hdr);

/**
 * @brief The PN sequence of the scrambler, one bit per byte.
 *
 * S(x) = x^7 + x^4 + 1 from an all-ones seed (STD-7 Ap1.1).  pn[0..23] packed
 * LSB first are dstarSlowScrambler, the pattern the slow-data fields use.
 *
 * @param out: destination, DSTAR_CODED_BITS bytes at most.
 * @param len: bit count.
 */
void dstarPnBits(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DSTAR_FEC_H */
