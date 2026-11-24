/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AGC_H
#define AGC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file agc.h
 * @brief Feed-forward automatic gain control and soft limiter for amplitude
 * modulated audio.
 *
 * An envelope detector that feeds a frequency modulated receiver carries the
 * same average level whatever the field strength is: the limiter behind it
 * throws the amplitude away.  An AM receiver keeps it, so the audio level
 * tracks the signal level, and in the aeronautical band that level moves by
 * tens of dB in seconds (an aircraft on approach, a tower answered from a
 * different distance, a fading carrier).  This is the block that takes that
 * away: it holds the audio at a constant level, reducing the gain quickly when
 * the signal rises and letting it back slowly when the signal falls, so that a
 * burst of noise or a heterodyne does not pump the receiver and a fade does not
 * take the gain with it.
 *
 * The gain is in Q16: AGC_UNITY is unity, so a gain of AGC_UNITY / 2 is -6 dB
 * and 16 * AGC_UNITY is +24 dB.  The default configuration is tuned for 48 kHz
 * mono or stereo audio and is documented in agc_defaultConfig().
 *
 * The block is sample-rate agnostic in the sense that nothing is hardcoded for
 * 48 kHz, but the time constants of agc_defaultConfig() are expressed in
 * samples and assume it.
 */

/** Unity gain, in Q16. */
#define AGC_UNITY 65536

/**
 * Configuration of an AGC instance, in samples and Q16 gain units.
 *
 * All of these are plain numbers so that the block can be tested and measured
 * with other time constants; agc_defaultConfig() has the values used on the
 * radio.
 */
struct agcConfig
{
    uint8_t  attackShift;  ///< Envelope rise, in samples (2^n); 0 for none.
    uint8_t  decayShift;   ///< Envelope fall time constant, in samples (2^n).
    int32_t  openLevel;    ///< Level above this reduces the gain.
    int32_t  closeLevel;   ///< Level below this increases the gain.
    uint8_t  gainAttackShift;  ///< Gain attack rate, 2^n part of the gain.
    uint8_t  gainReleaseShift; ///< Gain release rate, 2^n part of the gain.
    int32_t  gainMax;      ///< Q16 ceiling of the gain.
    int32_t  gainMin;      ///< Q16 floor of the gain.
    int32_t  limitKnee;    ///< Soft limiter knee, sample units.
};

/**
 * AGC state: the envelope detector, the gain and the configuration it runs
 * with.
 */
struct agc
{
    int32_t env;     ///< Peak envelope of the input, sample units.
    int32_t gain;    ///< Linear gain, Q16.
    struct agcConfig cfg; ///< Coefficients in use.
};

/**
 * Default configuration: 48 kHz, 0.35 and 0.25 of full scale as the level
 * window (about 3 dB of hysteresis), 10 dB of attack in 12 ms, 10 dB of release
 * in 780 ms, +24 dB to -30 dB of gain, limiter knee at 0.82 of full scale.
 *
 * The gain moves by a fraction of itself per sample, not by a constant, so
 * both time constants are the same number of dB per millisecond at any gain:
 * the attack is fast enough to catch an aircraft that suddenly appears over the
 * hill, and the release is slow enough not to bring the noise of a fade up
 * between syllables.
 *
 * The numbers are a starting point measured on synthetic AM with fading (see
 * docs/RX_AIRBAND.md), not a standard: aviation AM audio is a 300-2500 Hz
 * voice channel and the level that sounds right is the one the operator sets on
 * the volume knob, so this block only has to keep the level it hands over
 * steady.
 *
 * @return the configuration.
 */
struct agcConfig agc_defaultConfig(void);

/**
 * Initialise an AGC instance. The gain starts at unity.
 *
 * @param a: instance.
 * @param cfg: configuration, NULL for agc_defaultConfig().
 */
void agc_init(struct agc *a, const struct agcConfig *cfg);

/**
 * Reset the detector and the gain to their initial state.
 *
 * @param a: instance.
 */
void agc_reset(struct agc *a);

/**
 * Process a block of interleaved or strided samples in place.
 *
 * @param a: instance.
 * @param samples: first sample of the block.
 * @param count: number of samples of this channel in the block.
 * @param stride: distance between consecutive samples of the channel.
 */
void agc_process(struct agc *a, int16_t *samples, size_t count, size_t stride);

/**
 * Current gain.
 *
 * @param a: instance.
 * @return the linear gain in Q16, AGC_UNITY for unity.
 */
int32_t agc_getGain(const struct agc *a);

#ifdef __cplusplus
}
#endif

#endif /* AGC_H */
