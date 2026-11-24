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
 * No backend ships in this tree. The two candidates and their terms are in
 * docs/RX_P25.md: mbelib-neo (GPL-2.0-or-later, the one the host prototype in
 * work/p25-rx decodes with) and the fixed-point imbe_vocoder of op25
 * (GPL-3.0-or-later). Both are compatible with this firmware's GPL-3.0-or-later
 * and both implement the IMBE/AMBE patent family that TIA-102.BABA inherits
 * (US 5,701,390 and US 8,359,197, the latter running to about 2028); this is
 * a statement of fact, not a legal opinion, and the decision to link one is the
 * user's.
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
 * @return 0 on success, -1 if no decoder is linked in or the frame is bad.
 */
int p25Vocoder_decode(const uint8_t params[11], int16_t pcm[P25_VOCODER_PCM_SAMPLES]);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_P25_VOCODER_H */
