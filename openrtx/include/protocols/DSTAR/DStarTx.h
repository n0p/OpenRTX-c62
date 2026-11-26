/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DSTAR_TX_H
#define OPENRTX_DSTAR_TX_H

/**
 * @file DStarTx.h
 * @brief D-STAR DV transmit chain: the bit stream of JARL STD-7 and the GMSK
 * modulator that feeds the FM path.
 *
 * What goes on air, in order:
 *
 *   64 bits "1010..."                      bit sync      (STD-7 4.1.1 a)
 *   15 bits 111011001010000                frame sync    (STD-7 4.1.1 b)
 *   660 coded header bits                  FEC + interleave + scrambler
 *   N x 96 bits: 72 voice + 24 slow data   the resync word replaces the slow
 *                                          field of every 21st frame
 *   48 bits end pattern                    32 x "10" + 0001001101011110
 *
 * The modulator is the exact inverse of the receiver in DStarPhy.c: NRZ
 * (+-1 = the two deviations), Gaussian BT 0.5 at 10 samples per symbol, output
 * as the modulating waveform an FM exciter wants (48 kHz, signed 16 bit).  The
 * radio's discriminator and the exciter's deviation are scaled by the
 * transceiver calibration, so the caller passes the amplitude in Q15 units of
 * full scale; on the C62 that is the same `REG_40` deviation register the DMR
 * transmitter uses, and it is still a bench item (docs/TX_DSTAR.md).
 *
 * Voice: the 72-bit frame is an AMBE-2020 (3600x2400) channel frame.  Without
 * an encoder the chain sends the D-STAR null frame, which is what an Icom radio
 * transmits when the operator is silent, so header, slow data and framing are
 * all exercised on air.  A voice source hook lets a platform (or a test) supply
 * real frames.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/DSTAR/DStarDefs.h"
#include "protocols/DSTAR/DStarFec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One transmission: header, message, and whether to repeat the header. */
struct dstarTxConfig {
    struct dstarHeader hdr; ///< Flags, RPT1/RPT2, UR, MY, suffix
    const char *text;       ///< Slow-data message, 20 characters, may be NULL
    bool repeatHeader;      ///< Emit header copies in the slow data (late entry)
    bool useNullVoice;      ///< Send the null AMBE frame instead of the hook
};

/**
 * @brief Voice source: fill one 72-bit AMBE channel frame (9 bytes, air order).
 *
 * @return 0 on success, -1 to fall back to the null frame.
 */
typedef int (*dstarVoiceSource)(uint8_t frame[DSTAR_VOICE_BYTES], void *user);

/** Install a voice source (NULL restores the null frame). */
void dstarTx_setVoiceSource(dstarVoiceSource src, void *user);

/** Start a transmission: build the header, restart the slow-data stream. */
void dstarTx_begin(const struct dstarTxConfig *cfg);

/** End the transmission: next samples carry the end pattern, then silence. */
void dstarTx_end(void);

/** @return true when the end pattern has been emitted (PTT can drop). */
bool dstarTx_done(void);

/**
 * @brief Produce modulating waveform samples.
 *
 * @param out: destination, signed 16 bit at 48 kHz.
 * @param count: samples wanted.
 * @param amplitude: Q15 full-scale factor for the deviations.
 * @return samples written (always count, zero once the transmission ended).
 */
int dstarTx_samples(int16_t *out, int count, int amplitude);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DSTAR_TX_H */
