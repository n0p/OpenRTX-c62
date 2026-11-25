/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_P25_VOCODER_H
#define OPENRTX_P25_VOCODER_H

/**
 * @file P25Vocoder.h
 * @brief IMBE 4400 bit/s decoder hook of the P25 receive mode.
 *
 * The demodulator hands over the 88 vocoder parameters of a 20 ms frame,
 * already FEC-corrected, packed MSB first in 11 bytes (P25Phy.h, voice[]).
 * A backend turns them into 160 samples of 8 kHz PCM.
 *
 * No backend shipped in this tree until now. Two exist:
 *   - CONFIG_P25_VOCODER: lib/imbe_vocoder, this tree's own C decoder, a port of
 *     mbelib-neo's IMBE path with the TIA-102.BABA 7.5 spec rendering
 *     (GPL-2.0-or-later core, GPL-3.0-or-later wrapper). Default when the P25
 *     mode is on.
 *   - CONFIG_P25_MBELIB: mbelib-neo (GPL-2.0-or-later) linked in as before,
 *     with its own DVSI 10 ms subframe rendering. Off by default: it costs
 *     9.36 M instructions per frame, more than the 6 M of a 20 ms budget.
 *
 * Both implement the IMBE/AMBE patent family that TIA-102.BABA inherits
 * (US 5,701,390 and US 8,359,197, the latter running to about 2028); this is
 * a statement of fact, not a legal opinion. docs/IMBE.md has the bit
 * allocation, the verification, the cost and the licences.
 *
 * Without a backend the hook reports unavailable, the speech path plays
 * silence, and the status still counts the frames: the mode is complete from
 * the antenna to the parameters, which is what it can prove without one.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Samples of 8 kHz audio per IMBE frame. */
#define P25_VOCODER_PCM_SAMPLES 160
/** Sample rate of the vocoder output. */
#define P25_VOCODER_SAMPLE_RATE 8000

/**
 * @return true if a decoder is linked in.
 */
bool p25Vocoder_available(void);

/**
 * Clear the decoder state. Call when a new transmission starts: the vocoder is
 * stateful (pitch, spectral envelope and phase), and carrying the previous
 * talker's state makes the first frames sound wrong.
 */
void p25Vocoder_reset(void);

/**
 * Decode one IMBE frame.
 *
 * @param params: the 88 parameters, packed MSB first, 11 bytes.
 * @param pcm: destination, P25_VOCODER_PCM_SAMPLES samples.
 * @return 0 on a voice frame, 1 if the frame was repeated or muted (still
 *         played, the way a P25 receiver covers a lost frame), -1 if no
 *         decoder is linked in or the arguments are bad.
 */
int p25Vocoder_decode(const uint8_t params[11], int16_t pcm[P25_VOCODER_PCM_SAMPLES]);

/**
 * Decode one IMBE frame whose channel decoder reported nerr corrected bits.
 *
 * The count drives TIA-102.BABA 7.7's frame repeat and 7.8's muting: a frame
 * with more than five corrected bits is repeated instead of played, and a run
 * of them mutes to 7.8's noise. The P25 PHY reports the corrected bits of a
 * whole LDU, not of each of its nine frames, so the speech path passes 0 for a
 * frame whose FEC succeeded and a value over the threshold for one that failed.
 *
 * @return as p25Vocoder_decode().
 */
int p25Vocoder_decode_errs(const uint8_t params[11], int nerr,
                           int16_t pcm[P25_VOCODER_PCM_SAMPLES]);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_P25_VOCODER_H */
