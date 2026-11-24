/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_NXDN_FEC_H
#define OPENRTX_NXDN_FEC_H

/**
 * @file NxdnFec.h
 * @brief NXDN channel coding: data scrambler, LICH parity, SACCH convolutional
 * block and the voice-frame bit mapping.
 *
 * The voice channel itself has no FEC of ours to undo: a VCH is 72 bits of
 * AMBE+2 at 3600 bit/s, and those 72 bits already are one AMBE+2 3600x2450
 * frame -- 12 + 12 + 25 information bits with the Golay(24,12), Golay(23,12)
 * protection the vocoder decodes internally (NXDN TS 1-A 4.5.2: "channel
 * coding on VCH is described in another section", i.e. it is part of the
 * vocoder specification, not of the air interface).  What differs from DMR is
 * only where the 36 dibits sit in the frame: they are contiguous, one VCH per
 * 20 ms, with no burst-level interleaving to undo.
 *
 * Everything else has its own coding:
 *  - LICH: 8 coding bits sent as one dibit each, the data bit in the sign; one
 *    even parity bit over the 4 MSBs (4.5.3, Figure 4.5-7).
 *  - SACCH: 26 information bits (8 of SR + 18 of layer 3), 6-bit CRC, four
 *    zero tail bits, rate 1/2 K=5 convolutional code punctured to 60 bits and
 *    interleaved in five 12-bit blocks (4.5.2.1, Figure 4.5-4).
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/NXDN/NxdnDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Undo the data scrambler of TS 1-A 4.6 on the 182 payload dibits.
 *
 * The scrambler inverts the sign of a symbol, which is the high bit of its
 * dibit, so this is a XOR with 0x2 at the positions nxdnScramble[] marks.
 *
 * @param inverted the discriminator is inverted: negate every symbol as well.
 *                 A negated symbol is its dibit's sign bit flipped, which is
 *                 the same operation as the scrambler, so the two compose and
 *                 one pass does both.  In place.
 */
void nxdnFecDescramble(uint8_t dibits[NXDN_LICH_DIBITS + NXDN_SACCH_DIBITS
                                      + NXDN_VCH_PER_FRAME * NXDN_VCH_DIBITS],
                       bool inverted);

/**
 * @brief Apply the data scrambler of TS 1-A 4.6 (the transmitting half).
 *
 * The scrambler is an XOR of the sign bit at the positions nxdnScramble[]
 * marks, so it is its own inverse and this is nxdnFecDescramble() by another
 * name; the separate entry point is what the transmitter calls, and the
 * `inverted` argument means the same thing (an inverted discriminator).
 */
void nxdnFecScramble(uint8_t dibits[NXDN_LICH_DIBITS + NXDN_SACCH_DIBITS
                                     + NXDN_VCH_PER_FRAME * NXDN_VCH_DIBITS],
                     bool inverted);

/**
 * @brief Build the 8 coding bits of a LICH from its 7 control bits.
 *
 * @param control the 7 control bits, bit 6 first, as nxdnLichParse() takes
 *                them.
 * @param dibits the 8 dibits of the field: the data bit in the sign, the
 *               magnitude bit set (every LICH symbol is an outer one).
 */
void nxdnFecLichEncode(uint8_t control, uint8_t dibits[NXDN_LICH_DIBITS]);

/**
 * @brief Encode one 60-bit SACCH block (the transmitting half of
 * nxdnFecSacch()).
 *
 * @param info 26 information bits, one per byte: 8 of SR, then 18 of L3.
 * @param bits 60 bits, one per byte, in transmission order.
 */
void nxdnFecSacchEncode(const uint8_t info[26], uint8_t bits[NXDN_SACCH_BITS]);

/**
 * @brief Pack one canonical AMBE+2 frame into the 36 dibits of a VCH.
 *
 * The inverse of nxdnFecVchToAmbe(): the DMR dibit schedule that turns
 * mbelib's ambe_fr[4][24] planes into 36 symbols, run backwards.
 */
void nxdnFecVchFromAmbe(const uint8_t frame[NXDN_AMBE_BYTES],
                        uint8_t dibits[NXDN_VCH_DIBITS]);

/**
 * @brief Read the LICH out of its 8 dibits (already descrambled).
 *
 * @return false if the even parity over the 4 MSBs does not match.
 */
bool nxdnFecLich(const uint8_t dibits[NXDN_LICH_DIBITS], struct nxdnLich *lich);

/**
 * @brief Decode one 60-bit SACCH block.
 *
 * @param bits  60 bits, one per byte, in transmission order.
 * @param out   26 information bits, one per byte: 8 of SR, then 18 of L3.
 * @return true if the 6-bit CRC of the block matches.
 */
bool nxdnFecSacch(const uint8_t bits[NXDN_SACCH_BITS], uint8_t out[26]);

/**
 * @brief Map the 36 dibits of a VCH to one canonical AMBE+2 frame.
 *
 * The 72 on-air bits go through the same dibit schedule DMR uses (a private
 * table in NxdnFec.c since the DMR package took over protocols/DMR/), which is
 * the mbelib ambe_fr[4][24] plane order: NXDN and DMR carry the same AMBE+2
 * frame, they only pack it into the burst differently.  The result is the 9-byte
 * A(24) | B(23) | C(25) layout dmr_vocoder_decode() takes.
 */
void nxdnFecVchToAmbe(const uint8_t dibits[NXDN_VCH_DIBITS],
                      uint8_t frame[NXDN_AMBE_BYTES]);

/** One layer 3 message carried by the SACCH of a superframe. */
struct nxdnSacchMessage {
    uint8_t bits[72];  ///< The 72 bits, 18 per frame, MSB first
    uint8_t octets[9]; ///< The same, as octets
    uint8_t type;      ///< Message type: the first octet
    uint8_t ccOption;  ///< VCALL: call control option
    uint8_t callType;  ///< VCALL: call type (3 bits)
    uint16_t source;   ///< VCALL/TX_REL/DISC: source unit ID
    uint16_t dest;     ///< VCALL/TX_REL/DISC: destination or talkgroup
    bool valid;        ///< All four segments had a good CRC
};

/** SACCH layer 3 message types that a voice call uses (TS 1-C Table). */
enum nxdnMsgType {
    NXDN_MSG_VCALL = 0x01,    ///< Voice call
    NXDN_MSG_VCALL_IV = 0x03, ///< Voice call, ciphering IV
    NXDN_MSG_TX_REL_EX = 0x07, ///< Transmission release (extended)
    NXDN_MSG_TX_REL = 0x08,   ///< Transmission release
    NXDN_MSG_DISC = 0x11,     ///< Disconnect
    NXDN_MSG_ALIAS = 0x35,    ///< Alias (text) segment
};

/**
 * @brief Parse the 72 bits of an assembled SACCH message.
 *
 * Only the fields a receiver shows are decoded (type, source, destination);
 * the ciphering and the alias segments are recognised but not interpreted.
 */
void nxdnFecSacchParse(const uint8_t bits[72], struct nxdnSacchMessage *msg);

/**
 * @brief Name of a SACCH message type, for the screen and the logs.
 */
const char *nxdnFecMsgName(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_NXDN_FEC_H */
