/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_C4FM_MOD_H
#define OPENRTX_C4FM_MOD_H

/**
 * @file C4fmMod.h
 * @brief 4-level C4FM modulator: symbols in, 48 kHz modulating waveform out.
 *
 * P25 Phase 1, Yaesu System Fusion and DMR all put four deviation levels on a
 * 12.5 kHz channel at 4800 symbols/s with the same dibit-to-level mapping
 * (TIA-102.BAAA 6.3, and the same table for the other two: dibit 01 is the
 * outer high level).  What changes between them is the pulse shape and the
 * deviation, both of which are arguments here, so there is one shaper and not
 * three.
 *
 * The shaper is a root raised cosine, alpha 0.2, 81 taps at ten samples per
 * symbol, i.e. the exact filter the P25 and DMR receivers run as a matched
 * filter: transmitter RRC times receiver RRC is a raised cosine, which is the
 * Nyquist pulse both specifications describe.  It is the same design the DMR
 * transmitter uses (protocols/DMR/dmr_tx.c, dmr_mod_init), kept separate so
 * that P25 and System Fusion do not have to link the DMR package.
 *
 * Deviation: the caller passes the gain, in Q15 units of full scale per unit
 * of symbol level, exactly like the DMR and D-STAR transmitters -- on the C62
 * that number is the REG_40 deviation calibration, still a bench item.  The
 * symbol levels are +-1 and +-3, so a gain that puts the outer level at
 * 1800 Hz of deviation is what P25 (and System Fusion) ask for.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Samples per symbol at 48 kHz. */
#define C4FM_MOD_SPS 10

/**
 * Pulse the shaper convolves the symbol levels with.
 *
 * RRC is the P25 transmitter filter of TIA-102.BAAA: the receiver runs the same
 * pulse as a matched filter, so transmitter times receiver is a raised cosine
 * and the eye is open at the sampling instants.  System Fusion's receiver in
 * YsfPhy.c has no matched filter (the radio's IF filter is expected to shape
 * the eye), so its transmitter needs a pulse that is already Nyquist on its
 * own: RC.  Same bandwidth, same ten samples per symbol, same deviation.
 */
typedef enum c4fmPulse {
    C4FM_PULSE_RRC = 0, ///< Root raised cosine (P25)
    C4FM_PULSE_RC = 1,  ///< Raised cosine, Nyquist on its own (System Fusion)
} c4fmPulse_t;

/** Taps of the RRC pulse: +-4 symbols at ten samples per symbol. */
#define C4FM_MOD_TAPS 81

/** Modulator state. */
typedef struct c4fmMod {
    float taps[C4FM_MOD_TAPS]; ///< RRC, already scaled by gain
    float hist[16];            ///< Symbol history, power of two: wraps by mask
    unsigned histPos;
    float gain;
    bool invert; ///< Flip the deviation sense (bench option)
} c4fmMod_t;

/**
 * @brief Build the pulse and reset the history.
 *
 * @param mod: state.
 * @param gain: full-scale units per unit of symbol level (see the file note).
 * @param invert: reverse the level polarity.
 * @param pulse: RRC or RC (see c4fmPulse_t).
 */
void c4fmMod_init(c4fmMod_t *mod, float gain, bool invert, c4fmPulse_t pulse);

/**
 * @brief Shape one or more symbols.
 *
 * @param mod: state.
 * @param dibits: n dibits, air order (01 -> +3, 00 -> +1, 10 -> -1,
 *                11 -> -3); NULL sends n symbols of silence.
 * @param n: symbols.
 * @param out: C4FM_MOD_SPS * n samples, signed 16 bit at 48 kHz.
 */
void c4fmMod_dibits(c4fmMod_t *mod, const uint8_t *dibits, int n, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_C4FM_MOD_H */
