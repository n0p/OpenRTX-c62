/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_P25_TX_H
#define OPENRTX_P25_TX_H

/**
 * @file P25Tx.h
 * @brief P25 Phase 1 transmit chain: HDU, LDU1/LDU2, TSBK and TDU frames, and
 * the C4FM waveform that carries them.
 *
 * Every frame is 1728 bits (180 ms): the 48-bit frame sync, the 64-bit NID
 * (BCH-coded NAC and DUID), 24 status symbols, and a payload that depends on
 * the DUID.  The channel coding is the exact inverse of the receiver in
 * P25Phy.c and P25Fec.c:
 *
 *   HDU   36 x Golay(24,12) words, 18 bits of each on the air, over the
 *         RS(36,20) of the 120-bit header (MI, MFID, ALGID, KID, TGID)
 *   LDU1  9 IMBE codewords (4 Golay + 3 Hamming + 7 raw bits, PN scrambled)
 *         and the link control in 24 Hamming(10,6) words over RS(24,12)
 *   LDU2  the same voice, and an all-zero encryption sync
 *   TSBK  three trellis-coded 196-bit blocks with CRC-16
 *   TDU   the terminator: NID only
 *
 * **No IMBE encoder ships in this tree** and none of the two candidate
 * vocoders (mbelib-neo, op25) has an encoder half, so the transmitter sends
 * voice only when the platform installs a voice source; without one it keys
 * the carrier with HDU + TSBK + TDU, which is a valid, decodable P25
 * transmission with no voice in it.  docs/TX_P25.md has the numbers behind
 * that decision.
 *
 * The modulator is protocols/C4FM/C4fmMod: RRC alpha 0.2, ten samples per
 * symbol at 48 kHz, four levels, and the caller's gain converts a symbol level
 * into deviation.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/P25/P25Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the transmission announces. */
struct p25TxConfig
{
    uint16_t nac;   ///< Network access code, 0x000 to 0xFFF
    uint16_t group; ///< Talkgroup of the call
    uint32_t source;///< Source subscriber unit, 24 bits
    uint8_t service;///< Service options of the link control: 0x40 encrypted
};

/**
 * @brief Voice source: fill one IMBE frame.
 *
 * @param params: 88 vocoder parameters, packed MSB first, 11 bytes (the layout
 *                P25Vocoder.h takes on receive).
 * @return 0 on success, -1 to send silence instead.
 */
typedef int (*p25VoiceSource)(uint8_t params[11], void *user);

/** Install a voice source (NULL removes it: the call becomes HDU+TSBK+TDU). */
void p25Tx_setVoiceSource(p25VoiceSource src, void *user);

/**
 * @brief Data of the idle TSBK blocks (12 bytes, the last two are the CRC).
 *
 * Without a voice source the transmitter repeats this block in a TSBK frame
 * every 180 ms.  The default is all-zero data with a valid CRC-16, i.e. a Null
 * Information block; a platform that wants to signal something installs its
 * own.  Call before p25Tx_begin().
 */
void p25Tx_setTsbk(const uint8_t block[P25_TSBK_BYTES]);

/** Start a transmission: the next samples are the HDU. */
void p25Tx_begin(const struct p25TxConfig *cfg);

/** End it: the next frame is the TDU, then silence. */
void p25Tx_end(void);

/** @return true when the TDU has left the shaper (PTT can drop). */
bool p25Tx_done(void);

/**
 * @brief Produce modulating waveform samples.
 *
 * @param out: destination, signed 16 bit at 48 kHz.
 * @param count: samples wanted; any count, the symbol carry is internal.
 * @param gain: full-scale units per symbol level (outer level = 3 * gain).
 * @return samples written (always count, zero once the TDU is over).
 */
int p25Tx_samples(int16_t *out, int count, float gain);

/** @return frames handed to the modulator since p25Tx_begin(), diagnostics. */
uint32_t p25Tx_frames(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_P25_TX_H */
