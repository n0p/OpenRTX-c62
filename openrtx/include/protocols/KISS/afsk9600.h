/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 9600 bit/s G3RUH baseband FSK: modulator and demodulator.
 *
 * What "9600 baud packet" is on FM, and what this file implements:
 *
 *  - The data bits of the HDLC frame (no NRZI, unlike 1200) are scrambled by
 *    the 17-bit polynomial 1 + x^12 + x^17 (G3RUH, "9600 Baud Packet Radio
 *    Modem Design", 1992). The scrambler is what removes the DC component and
 *    guarantees the transitions the receiver clock needs.
 *  - The scrambled bits are sent as a two-level NRZ signal, +-1, at 9600 baud,
 *    shaped so the spectrum stays inside what a voice FM channel passes. Here
 *    the shaping is a raised-cosine transition one bit long, which is what
 *    direwolf's transmitter does (gen_tone.c) and what its receiver expects.
 *  - On the air this baseband is the *deviation* of the carrier: the radio's
 *    modulator adds the carrier, the receiver's discriminator gives the
 *    baseband back. The modem therefore only ever sees audio, exactly like
 *    the 1200 path, and one sample rate change (48 kHz) is all the timing
 *    there is: 5 samples per bit.
 *
 * The receiver, mirroring direwolf's demod_9600.c so that the two can be
 * compared on the same audio: DC blocker, one-bit low pass (cosine window,
 * cut-off at the baud rate), AGC on the peaks, and a bit clock that is a
 * phase accumulator nudged by the zero crossings of the filtered signal, with
 * the bit decided by integrating the middle half of the bit.
 *
 * Everything is in float and allocation free: no tables, one FIR of `spb`
 * taps computed at init.
 */
#ifndef AFSK9600_H
#define AFSK9600_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G3RUH_BAUD      9600u
#define G3RUH_MAX_TAPS  16u     /* 48 kHz: 5 taps, 96 kHz: 10            */

/* Bit clock tuning.
 *
 * The baseband only changes state on a bit boundary, so a transition is the
 * clock: the grid of cell centres free-runs at one bit and every transition
 * pulls its phase towards the boundary that was observed. These are the two
 * numbers that matter, both in bits.
 *
 *  G3RUH_PLL_KP        weight of one boundary observation (0 = ignore them)
 *  G3RUH_DEC_HALF_WIN  half width of the integration window of one cell
 *  G3RUH_DEC_MARGIN    how far in the past a cell must be before it is
 *                      decided, so its whole window is available
 */
#define G3RUH_PLL_KP         0.30f
#define G3RUH_DEC_HALF_WIN   0.30f
#define G3RUH_DEC_MARGIN     0.40f

/* ------------------------------------------------------------------ TX --- */

typedef struct
{
    uint32_t fs;        /**< sample rate, 48000 on the C62 TX path        */
    uint32_t spb;       /**< samples per bit: fs / 9600 (5 at 48 kHz)     */
    uint32_t bitPos;    /**< sample index inside the current bit          */
    float    level;     /**< current NRZ level, -1 or +1                  */
    float    prev;      /**< previous level, for the transition shaping   */
    uint32_t lfsr;      /**< scrambler, 17 bits                           */
    uint32_t nrzi;      /**< NRZI level, 0 or 1                           */
    float    gain;      /**< 0..1 of full scale                           */
} g3ruh_mod_t;

/**
 * Initialise a modulator.
 *
 * @param fs: sample rate; must be a multiple of 9600 (48000 works, 24000 does
 *            not: 2.5 samples per bit is not enough for the shaping).
 * @return false if the sample rate cannot carry 9600 baud.
 */
bool g3ruh_mod_init(g3ruh_mod_t *m, uint32_t fs);

/** Reset the scrambler and the level, keeping the settings. */
void g3ruh_mod_reset(g3ruh_mod_t *m);

/** Set the output amplitude, 0..1 of full scale. */
void g3ruh_mod_setGain(g3ruh_mod_t *m, float gain);

/**
 * Generate the samples of one HDLC data bit: scrambles it, shapes the
 * transition and writes `spb` samples to `out`.
 */
void g3ruh_mod_bit(g3ruh_mod_t *m, bool bit, int16_t *out);

/** Generate `ms` of silence (TXDELAY filler). Returns samples written. */
size_t g3ruh_mod_silence(g3ruh_mod_t *m, int16_t *out, size_t max,
                         uint32_t ms);

/** The scrambler step alone, for the tests: one input bit in, one out. */
static inline bool g3ruh_scramble(uint32_t *lfsr, bool bit)
{
    uint32_t s = *lfsr;
    bool out = bit ^ (bool)(((s >> 11) ^ (s >> 16)) & 1u);
    *lfsr = ((s << 1) | (out ? 1u : 0u)) & 0x1FFFFu;
    return out;
}

/* ------------------------------------------------------------------ RX --- */

/** Samples of filtered signal the clock keeps, must exceed 8 bits * spb. */
#define G3RUH_RING  64u

typedef struct
{
    uint32_t fs;
    uint32_t spb;

    /* DC blocker, y[n] = x[n] - x[n-1] + a*y[n-1] */
    float    dcX1, dcY1;

    /* one-bit low pass, `spb` taps */
    float    fir[G3RUH_MAX_TAPS];
    float    hist[G3RUH_MAX_TAPS];
    uint8_t  firIdx;

    /* AGC on the peaks of the filtered signal */
    float    peak, valley, gain;

    /*
     * Bit clock: `toNext` is the distance to the next cell centre, in samples,
     * positive into the future and negative into the past. The grid runs on
     * its own at one bit per bit; transitions only correct its phase. `now`
     * indexes the ring of filtered samples that the decision looks into.
     */
    uint32_t now;
    float    ring[G3RUH_RING];
    bool     clockRunning;
    float    toNext;
    float    lastError;    /**< phase error of the last transition, in bits */

    float    prevY;
    float    lastY;
    bool     primed;

    /* descrambler and the NRZI decoder behind it */
    uint32_t lfsr;
    bool     lastBit;       /**< level decided, before the descrambler    */
    bool     nrziValid;
    bool     prevDescram;

    /* signal presence and statistics */
    float    energy;
    bool     signal;
    uint32_t bitsDecoded;
    uint32_t transitions;
} g3ruh_demod_t;

/** Initialise a demodulator for an input sample rate of 48000 Hz or above. */
bool g3ruh_demod_init(g3ruh_demod_t *d, uint32_t fs);

/**
 * Process one input sample, calling `onBit` for every data bit it decides
 * (zero, one or several: a bit is decided once the middle of its cell is in
 * the past).
 *
 * @return the number of bits decided.
 */
size_t g3ruh_demod_sample(g3ruh_demod_t *d, int16_t sample,
                          void (*onBit)(bool bit, void *user), void *user);

/**
 * Process a block of samples, calling `onBit` for each decided bit.
 * @return the number of bits decided.
 */
size_t g3ruh_demod_block(g3ruh_demod_t *d, const int16_t *samples, size_t count,
                         void (*onBit)(bool bit, void *user), void *user);

/** True while a signal with enough energy is present. */
static inline bool g3ruh_demod_signal(const g3ruh_demod_t *d)
{
    return d->signal;
}

/** The descrambler step alone, for the tests: one received bit in, one out. */
static inline bool g3ruh_descramble(uint32_t *lfsr, bool bit)
{
    uint32_t s = *lfsr;
    bool out = bit ^ (bool)(((s >> 11) ^ (s >> 16)) & 1u);
    *lfsr = ((s << 1) | (bit ? 1u : 0u)) & 0x1FFFFu;
    return out;
}

#ifdef __cplusplus
}
#endif

#endif /* AFSK9600_H */
