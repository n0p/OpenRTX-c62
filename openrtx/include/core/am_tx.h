/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AM_TX_H
#define AM_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file am_tx.h
 * @brief Amplitude modulator for a transmitter whose carrier is frequency
 * modulated: the voice becomes the envelope of the PA supply.
 *
 * The BK4819 can only modulate frequency (REG_40 moves a varactor), so a radio
 * built around it has no AM modulator in the chip.  What it does have is a
 * power control line: a PWM whose duty cycle sets the PA bias, the "APC" the
 * factory firmware uses to pick 1 W or 5 W.  That line is an amplitude
 * control, and it is fast enough to carry audio: driving it with the voice
 * envelope modulates the amplitude of an otherwise unmodulated carrier, which
 * is DSB with carrier, the modulation the aeronautical band uses.
 *
 * This block is the audio half of that: band limiting, levelling, limiting,
 * modulation depth and the key-up and key-down ramps.  It knows nothing about
 * registers or PWM channels -- it takes 48 kHz microphone samples and hands
 * back duty-cycle counts, one per PWM period group -- so it runs unchanged in
 * the radio, in the host measurement harness and in QEMU.
 *
 * Fixed point throughout, as in core/agc.h: the audio is int16, the samples
 * inside are Q15 (+-32768 is +-1.0) and the depth is in percent.
 */

/** Sample rate the modulator expects, in Hz. */
#define AMTX_RATE 48000

/** Rate at which a new duty cycle is produced, in Hz. */
#define AMTX_DUTY_RATE 8000

/** Input samples per duty cycle value. */
#define AMTX_DECIM (AMTX_RATE / AMTX_DUTY_RATE)

/** Floor of the modulation depth an operator can choose, in percent. */
#define AMTX_DEPTH_MIN 30

/** Ceiling of the modulation depth an operator can choose, in percent. */
#define AMTX_DEPTH_MAX 90

/** Default modulation depth, in percent. */
#define AMTX_DEPTH_DEFAULT 60

/**
 * Everything the modulator needs to know about the transmitter it drives.
 *
 * The three duty values and the slope are a calibration, not constants: the
 * transfer from APC duty to RF envelope depends on the PA, the band and the
 * factory bias network of the individual radio, and it is neither linear nor
 * the same at 118 MHz and at 137 MHz.  The linear model below is the one the
 * bench procedure of docs/TX_AIRBAND.md fits:
 *
 *   envelope(relative) = 1 + m * x       duty = dutyCarrier + slope * m * x
 *
 * clamped to [dutyMin, dutyMax].  dutyCarrier is the duty that gives the
 * carrier power the operator asked for (1 W in the airband calibration),
 * slope is how many counts of duty one unit of relative envelope takes at that
 * carrier, and the two limits are where the PA stops being usable: below
 * dutyMin the final is at cutoff and the envelope would fall faster than the
 * audio asks for, above dutyMax the PA saturates and the positive peaks would
 * be flattened instead of modulated.
 */
struct amTxConfig {
    uint16_t dutyCarrier; ///< PWM counts of the unmodulated carrier.
    uint16_t dutyMin;     ///< PWM counts at the bottom of the linear window.
    uint16_t dutyMax;     ///< PWM counts at the top of the linear window.
    uint16_t slope;       ///< PWM counts per unit of relative envelope.
    uint8_t depth;        ///< Modulation depth, percent, 30 to 90.
};

/**
 * One biquad section, direct form I: coefficients in Q14, signal in Q15.
 *
 * The outputs are saturated to the Q15 range, which is also what keeps a
 * section whose poles sit at 300 Hz with 16-bit coefficients from running away
 * on a rounding error.
 */
struct amTxBiquad {
    int32_t b0, b1, b2; ///< Feed-forward coefficients, Q14.
    int32_t a1, a2;     ///< Feedback coefficients, Q14, already negated.
    int32_t x1, x2;     ///< Previous inputs, Q15.
    int32_t y1, y2;     ///< Previous outputs, Q15.
};

/**
 * Modulator state: the anti-alias filter, the voice band filter, the level
 * control, the limiter, the depth, the two ramps and the calibration.
 */
struct amTx {
    struct amTxBiquad highPass; ///< 300 Hz, transmit band bottom.
    struct amTxBiquad lowPassA; ///< 3 kHz, Butterworth section 1 of 2.
    struct amTxBiquad lowPassB; ///< 3 kHz, Butterworth section 2 of 2.
    int32_t env;                ///< Peak envelope of the voice, sample units.
    int32_t gain;               ///< Levelling gain, Q16, 65536 = 0 dB.
    int32_t decimSum;           ///< Accumulator of the decimation boxcar.
    uint8_t decimCount;         ///< Samples in the accumulator.
    uint16_t depthRamp;         ///< Current depth, percent * 256.
    uint16_t carrierRamp;       ///< Carrier ramp, 0 to 256.
    uint8_t depthEff;           ///< Depth the PA window allows, percent.
    bool keyed;                 ///< true from key-down until the ramps end.
    bool closing;               ///< true while the key-up ramp is running.
    struct amTxConfig cfg;      ///< Calibration and depth in use.
};

/**
 * Default configuration for an uncalibrated radio: 1 W of carrier on the
 * factory 1 W duty, a linear duty-to-envelope slope equal to the carrier duty,
 * and a window from half to one and a half times that duty.  It is a starting
 * point for the bench, not a calibration: see docs/TX_AIRBAND.md.
 *
 * @param dutyCarrier: PWM counts of the carrier, from the band calibration.
 * @return the configuration.
 */
struct amTxConfig am_tx_defaultConfig(uint16_t dutyCarrier);

/**
 * Initialise a modulator.  The key is up and the ramps are at zero.
 *
 * @param t: instance.
 * @param cfg: configuration, NULL for am_tx_defaultConfig(500).
 */
void am_tx_init(struct amTx *t, const struct amTxConfig *cfg);

/**
 * Reset the filters, the level control and the ramps, keeping the calibration
 * and the depth.  Called when a transmission starts, so that no state of the
 * previous one leaks into the first syllable.
 *
 * @param t: instance.
 */
void am_tx_reset(struct amTx *t);

/**
 * Set the modulation depth, clamped to AMTX_DEPTH_MIN..AMTX_DEPTH_MAX.
 *
 * @param t: instance.
 * @param percent: depth in percent.
 */
void am_tx_setDepth(struct amTx *t, uint8_t percent);

/**
 * Change the carrier duty, for a power change while the mode is running.
 *
 * @param t: instance.
 * @param counts: PWM counts of the unmodulated carrier.
 */
void am_tx_setCarrier(struct amTx *t, uint16_t counts);

/**
 * Current modulation depth, percent.
 *
 * @param t: instance.
 * @return the depth the modulator is ramping to.
 */
uint8_t am_tx_getDepth(const struct amTx *t);

/**
 * Start a transmission: the carrier ramps up and then the modulation depth
 * does.  Nothing of the envelope reaches the PA before the carrier is up.
 *
 * @param t: instance.
 */
void am_tx_keyDown(struct amTx *t);

/**
 * End a transmission: the depth ramps to zero and then the carrier does.  The
 * modulator keeps producing duty values until am_tx_busy() turns false.
 *
 * @param t: instance.
 */
void am_tx_keyUp(struct amTx *t);

/**
 * Whether the modulator is still producing a carrier, that is, whether the
 * key-up ramp has not finished yet.
 *
 * @param t: instance.
 * @return true while the PA must stay enabled.
 */
bool am_tx_busy(const struct amTx *t);

/**
 * Turn a block of 48 kHz microphone samples into duty-cycle values.
 *
 * One value comes out per AMTX_DECIM input samples, so a block of 48 samples
 * gives 8 values at 8 kHz.  The caller writes each one to the APC PWM in the
 * order they are produced; the values already carry the ramps, the depth, the
 * DC that keeps the carrier from reaching zero and the clamping to the
 * calibrated window.
 *
 * @param t: instance.
 * @param in: first microphone sample of the block.
 * @param n: number of samples in the block, at least AMTX_DECIM.
 * @param duty: output buffer of n / AMTX_DECIM values.
 * @return the number of duty values written.
 */
size_t am_tx_process(struct amTx *t, const int16_t *in, size_t n,
                     uint16_t *duty);

/**
 * Relative envelope of one sample, for the measurement harnesses: the same
 * filtering, levelling and limiting am_tx_process() applies, without the
 * decimation and the duty mapping.
 *
 * @param t: instance.
 * @param sample: input sample.
 * @return the envelope in Q15, 32768 meaning a modulation depth of 1.0.
 */
int32_t am_tx_envelope(struct amTx *t, int16_t sample);

#ifdef __cplusplus
}
#endif

#endif /* AM_TX_H */
