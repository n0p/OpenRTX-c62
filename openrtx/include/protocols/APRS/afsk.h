/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bell 202 AFSK, 1200 bit/s: modulator and demodulator for APRS.
 *
 * Modulation: binary FSK, mark = 1200 Hz (binary 1 level), space = 2200 Hz
 * (binary 0 level), continuous phase, NRZI line code applied by the caller
 * (hdlc_framer_t produces data bits, afsk_mod_* applies NRZI and the tones).
 *
 * Demodulation: quadrature correlators at both tones with a one-bit sliding
 * window, soft metric = mark energy - space energy, bit clock recovered with a
 * PI loop on the metric's zero crossings, bit value decided by majority of the
 * three thirds of the bit. The internal rate is 24 kHz: 48 kHz input is
 * decimated by two. At 24 kHz one bit is exactly 20 samples, so the whole
 * modem is integer timing only.
 */
#ifndef AFSK_H
#define AFSK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AFSK_MARK_HZ      1200.0f
#define AFSK_SPACE_HZ     2200.0f
#define AFSK_BAUD         1200.0f
#define AFSK_RATE         24000u   /* internal demodulator rate */
#define AFSK_BIT_SAMPLES  (AFSK_RATE / 1200u)   /* 20 */

/*
 * Carrier detect thresholds, from the calibration in docs/APRS.md: a tone at
 * full deviation gives ~6.4e9 of correlator energy after the AGC, white noise
 * at the same peak ~4e8, and the coherence |mark-space|/(mark+space) is above
 * 0.8 for FSK against a mean of ~0.35 for noise.
 */
#ifndef AFSK_DCD_ENERGY
#define AFSK_DCD_ENERGY     1.2e9f
#endif
#ifndef AFSK_DCD_COHERENCE
#define AFSK_DCD_COHERENCE  0.5f
#endif

/* ------------------------------------------------------------------ TX --- */

typedef struct
{
    uint32_t fs;          /**< sample rate, 48000 on the C62 TX path      */
    uint32_t incMark;     /**< phase increment per sample, 2^32 == 360 deg */
    uint32_t incSpace;
    uint32_t phase;       /**< current phase, wraps once per cycle        */
    uint32_t samplesPerBit;
    uint32_t bitPos;      /**< sample index inside the current bit        */
    bool     level;       /**< current NRZI level (true = mark)           */
    bool     levelValid;
    float    preemph;     /**< 0 = flat, 1 = +6 dB/oct pre-emphasis        */
    float    hpPrev, hpState;
    float    gain;        /**< output amplitude, 1.0 = full scale         */
} afsk_mod_t;

/**
 * Initialise a modulator.
 *
 * @param fs: sample rate in Hz.
 * @param preemph: pre-emphasis amount, 0 (none) to 1 (+6 dB/oct above
 *                 ~1200 Hz). The C62's FM TX chain has no pre-emphasis of its
 *                 own, so the AFSK source has to provide it.
 */
void afsk_mod_init(afsk_mod_t *m, uint32_t fs, float preemph);

/** Reset the phase accumulator and the NRZI state, keeping the settings. */
void afsk_mod_reset(afsk_mod_t *m);

/**
 * Set the output amplitude, 0..1 of full scale. Full scale corresponds to the
 * deviation configured in the radio driver.
 */
void afsk_mod_setGain(afsk_mod_t *m, float gain);

/**
 * Generate the samples of one bit. `bit` is the HDLC data bit; NRZI is applied
 * here (a zero bit toggles the level, a one keeps it).
 */
void afsk_mod_bit(afsk_mod_t *m, bool bit, int16_t *out);

/**
 * Generate one bit from an already NRZI-encoded level. Used by the loopback
 * test to compare against the demodulator's level decisions.
 */
void afsk_mod_level(afsk_mod_t *m, bool level, int16_t *out);

/** Generate `ms` of silence (TXDELAY filler). Returns samples written. */
size_t afsk_mod_silence(afsk_mod_t *m, int16_t *out, size_t max, uint32_t ms);

/*
 * Bit clock loop: proportional and integral gains on the phase error, and the
 * integral clamp, in units of a bit. The clamp (2 per cent of a bit per bit)
 * is the maximum clock offset the receiver can follow: 24 baud on 1200, far
 * more than any crystal mismatch or Doppler at 145 MHz.
 */
 #define AFSK_PLL_KP        0.08f
 #define AFSK_PLL_KI        0.0008f
 #define AFSK_PLL_TRIM_MAX  0.001f

 /* Decision window inside the bit, in three slices.
 *
 * The correlator window is one bit long, so the metric at sample n describes
 * the symbol centred half a bit earlier: it peaks at the very end of a symbol
 * (where the window holds exactly that symbol) and crosses zero at the symbol
 * centre. The bit clock is anchored to that centre, so the accumulator for a
 * symbol spans from its centre to the next one: a whole bit of metric samples,
 * all of which look at the same symbol.
 */
#define AFSK_DEC_WIN_LO  0.05f
#define AFSK_DEC_WIN_HI  0.95f

/* ------------------------------------------------------------------ RX --- */

typedef struct
{
    uint32_t fs;              /**< input sample rate                          */
    uint32_t decim;           /**< 2 for 48 kHz input, 1 for 24 kHz           */

    /* decimation */
    int32_t  decAcc;
    uint32_t decCount;

    /* DC blocker, y[n] = x[n] - x[n-1] + a*y[n-1] */
    float    dcX1, dcY1;

    /* AGC: keeps the correlator magnitudes in a fixed range */
    float    agcEnv;
    float    agcGain;

    /* quadrature correlators: 4 product histories of one bit each */
    float    hist[4][AFSK_BIT_SAMPLES];
    float    sum[4];
    uint8_t  histIdx;
    uint32_t phaseMark;
    uint32_t phaseSpace;
    uint32_t incMark;
    uint32_t incSpace;
    float    metric;
    float    metricPrev;
    bool     crossArmed;      /**< the metric may produce a crossing         */
    bool     pllLocked;       /**< the bit clock has been anchored once      */

    /* bit clock */
    float    mu;              /**< bit phase, 0 at a bit boundary, 1 = next  */
    float    muInc;           /**< nominal 1 / AFSK_BIT_SAMPLES              */
    float    muTrim;          /**< integral correction, clock offset         */
    uint8_t  subWin;          /**< which third of the bit is being summed    */
    uint8_t  subCount;
    float    subAcc[3];
    bool     level;           /**< last decided NRZI level                   */
    float    lastAcc[3];      /**< thirds of the last bit, for diagnostics   */

    /* signal presence */
    float    energy;          /**< smoothed mark+space energy                */
    bool     signal;

    /* statistics */
    uint32_t bitsDecoded;
    uint32_t timingCorrections;
    float    lastError;
} afsk_demod_t;

/** Initialise a demodulator for an input sample rate of 24000 or 48000 Hz. */
bool afsk_demod_init(afsk_demod_t *d, uint32_t fs);

/**
 * Process one input sample. When a bit decision is taken it is returned in
 * *bit (true = one, false = zero) and the function returns true; otherwise it
 * returns false. The flows of levels/ones is the HDLC layer's business.
 */
bool afsk_demod_sample(afsk_demod_t *d, int16_t sample, bool *bit);

/**
 * Process a block of samples, calling `onBit` for each decided bit.
 *
 * @return the number of bits decided.
 */
size_t afsk_demod_block(afsk_demod_t *d, const int16_t *samples, size_t count,
                        void (*onBit)(bool bit, void *user), void *user);

/** True while a signal with enough correlator energy is present. */
static inline bool afsk_demod_signal(const afsk_demod_t *d)
{
    return d->signal;
}

/** Peak-to-peak amplitude of the last block, for a level meter. */
int afsk_level_percent(const int16_t *samples, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* AFSK_H */
