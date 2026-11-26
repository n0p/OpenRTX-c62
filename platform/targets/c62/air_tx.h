/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AIR_TX_H
#define AIR_TX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file air_tx.h
 * @brief Amplitude modulation of the C62 transmitter through its APC line.
 *
 * The BK4819 modulates frequency only: REG_40 moves a varactor and there is no
 * envelope modulator anywhere in the chip. What the radio does have is the
 * power control of the PA, a 100 kHz PWM on GPIO_A_03 whose duty cycle sets the
 * PA bias -- the "APC" the factory firmware uses to select 1 W or 5 W from its
 * band calibration. That line is an amplitude control with a useful bandwidth,
 * so the airband transmitter is built as DSB with carrier by driving it with
 * the voice envelope while the BK4819 transmits a bare carrier:
 *
 *   mic -> codec ADC (48 kHz) -> core/am_tx.c -> duty values (8 kHz)
 *       -> ring -> GPT channel 0 tick -> CHx_RELOAD of the APC channel
 *
 * The 8 kHz pacing cannot come from the audio worker: the codec hands it a
 * millisecond of audio at a time, and writing 48 duty values as fast as the
 * worker can, once a millisecond, would time-compress the envelope instead of
 * playing it. A second GPT channel runs as a periodic timer at exactly
 * AMTX_DUTY_RATE and takes one value from the ring per period, so the envelope
 * keeps its time base whatever the audio thread is doing. A value is never
 * older than 2 ms and the ring absorbs the jitter of the worker.
 *
 * Without that timer -- a build or a board where the counter is not available
 * -- the mode still keys, but the envelope is written once per audio block
 * (1 kHz), which cannot carry a 2.7 kHz voice: air_tx_init() reports it and the
 * documentation says so instead of pretending otherwise.
 */

/**
 * Prepare the APC line and the modulator. Reads the PWM period back from the
 * timer so that the duty values are in the units the hardware uses, and starts
 * the pacing channel.
 *
 * @return 0 on success, -ENODEV if the APC PWM is not there (no transmitter),
 * -ENOTSUP if the pacing timer is missing (transmit at the block rate only).
 */
int air_tx_init(void);

/**
 * Set the calibration of the carrier and of the linear window, in percent of
 * the APC PWM period -- the unit the factory band calibration stores.
 *
 * @param carrierPct: duty that gives the carrier power the operator asked for.
 * @param minPct: duty at the bottom of the window where the PA is still linear.
 * @param maxPct: duty at the top, where it starts to saturate.
 * @param slopePct: duty swing per unit of relative envelope. Equal to
 *                  carrierPct under the assumption that the PA envelope is
 *                  proportional to the duty; the bench measures what it is.
 */
void air_tx_setCalibration(uint8_t carrierPct, uint8_t minPct, uint8_t maxPct,
                           uint8_t slopePct);

/**
 * Set the modulation depth, clamped to the window the calibration allows.
 *
 * @param percent: depth in percent, 30 to 90.
 */
void air_tx_setDepth(uint8_t percent);

/**
 * Modulation depth the transmitter will actually produce, percent.
 *
 * @return the depth, which is the requested one unless the PA window is
 * narrower.
 */
uint8_t air_tx_depth(void);

/**
 * Key or unkey the amplitude modulator.
 *
 * Keying takes over the microphone to radio path (the voice becomes the
 * envelope and the BK4819 gets silence), starts the ramps and leaves the APC
 * line at the carrier duty. Unkeying ramps the carrier to zero in 16 ms and
 * returns the path: it blocks the caller for that ramp, which is the only place
 * in the transmit path that waits, and it is what keeps the PA switch from
 * being the thing that cuts the carrier.
 *
 * @param down: true to key, false to unkey.
 */
void air_tx_key(bool down);

/**
 * Whether the modulator is still producing a carrier, that is, whether the
 * key-up ramp has not finished. The PA must stay enabled and the APC line must
 * not be released until this turns false.
 *
 * @return true while a ramp is running.
 */
bool air_tx_busy(void);

/**
 * Stop at once: the APC line goes to zero and the path is given back. For the
 * transmit watchdog and for leaving the mode, where waiting for a 36 ms ramp is
 * not worth the risk of leaving the PA keyed.
 */
void air_tx_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* AIR_TX_H */
