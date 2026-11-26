/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/agc.h"

/*
 * Coefficients of agc_defaultConfig(), for 48 kHz.
 *
 * The detector is a peak follower that rises in a few samples and falls in
 * five milliseconds: fast enough that the gain stops falling as soon as the
 * audio is back in the window, slow enough not to follow a 300 Hz waveform
 * cycle by cycle. The gain itself is what sets the two time constants of the
 * AGC, as a fraction of the gain per sample: one 2^n part of the gain is
 * 8.686 / 2^n dB, so n = 9 is 0.017 dB per sample (10 dB in 12 ms) and n = 15
 * is 0.00026 dB per sample (10 dB in 780 ms), whatever the gain happens to be.
 */
static const struct agcConfig defaultConfig = {
    .attackShift      = 0,     /* the level is the peak, as it comes */
    .decayShift       = 8,     /* 256 samples, 5 ms at 48 kHz */
    .openLevel        = 11469, /* 0.35 of full scale */
    .closeLevel       = 8192,  /* 0.25 of full scale, 3 dB below openLevel */
    .gainAttackShift  = 9,
    .gainReleaseShift = 15,
    .gainMax          = 16 * AGC_UNITY, /* +24 dB */
    .gainMin          = AGC_UNITY / 32, /* -30 dB */
    .limitKnee        = 27000,          /* 0.82 of full scale */
};

struct agcConfig agc_defaultConfig(void)
{
    return defaultConfig;
}

void agc_reset(struct agc *a)
{
    /*
     * The detector starts at the top of the window, not at zero: a receiver
     * that opens on silence would otherwise amplify the first samples before
     * it has heard anything.
     */
    a->env = a->cfg.openLevel;
    a->gain = AGC_UNITY;
}

void agc_init(struct agc *a, const struct agcConfig *cfg)
{
    a->cfg = (cfg != NULL) ? *cfg : defaultConfig;
    agc_reset(a);
}

/**
 * Magnitude of a sample, without the 16 bit wrap of -32768.
 *
 * @param s: sample.
 * @return the magnitude, 0 to 32768.
 */
static int32_t magnitude(int32_t s)
{
    return (s < 0) ? -s : s;
}

/**
 * Move the envelope towards an instantaneous magnitude, in one step of its
 * time constant. The step is rounded up so that a constant level is reached
 * exactly instead of stalling one unit below it.
 *
 * @param diff: magnitude minus envelope.
 * @param shift: time constant, in bits.
 * @return the step, at least one unit when diff is not zero.
 */
static int32_t envelopeStep(int32_t diff, uint8_t shift)
{
    return (diff + (1 << shift) - 1) >> shift;
}

/**
 * Soft limiter: linear below the knee, 4:1 above it, and never past full
 * scale. Keeping the last few dB smooth is what stops a loud syllable from
 * turning into the buzz a hard clip makes.
 *
 * @param value: sample after the gain.
 * @param knee: knee of the limiter, sample units.
 * @return the limited sample.
 */
static int32_t limit(int32_t value, int32_t knee)
{
    int32_t mag = magnitude(value);

    if (mag <= knee)
        return value;

    mag = knee + ((mag - knee) >> 2);
    if (mag > 32767)
        mag = 32767;

    return (value < 0) ? -mag : mag;
}

void agc_process(struct agc *a, int16_t *samples, size_t count, size_t stride)
{
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i * stride];

        /*
         * The level the detector follows is the one the listener will hear,
         * before the limiter: a signal that arrives loud has to drive the gain
         * down even while the limiter is flattening it.
         */
        int32_t out = (int32_t)(((int64_t)s * a->gain) >> 16);
        int32_t mag = magnitude(out);

        /* Envelope: fast rise, slow fall */
        if (mag > a->env)
            a->env += envelopeStep(mag - a->env, a->cfg.attackShift);
        else
            a->env -= envelopeStep(a->env - mag, a->cfg.decayShift);

        /*
         * Gain steering, a fraction of the gain per sample so that the time
         * constant is the same number of dB per millisecond at any gain.
         * Inside the window between closeLevel and openLevel the gain holds,
         * which is the hysteresis that keeps a signal sitting on the threshold
         * from pumping.
         */
        if (a->env > a->cfg.openLevel) {
            a->gain -= a->gain >> a->cfg.gainAttackShift;
            if (a->gain < a->cfg.gainMin)
                a->gain = a->cfg.gainMin;
        } else if (a->env < a->cfg.closeLevel) {
            a->gain += a->gain >> a->cfg.gainReleaseShift;
            if (a->gain > a->cfg.gainMax)
                a->gain = a->cfg.gainMax;
        }

        samples[i * stride] = (int16_t)limit(out, a->cfg.limitKnee);
    }
}

int32_t agc_getGain(const struct agc *a)
{
    return a->gain;
}
