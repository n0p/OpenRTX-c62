/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NOISE_SUPPRESS_H
#define NOISE_SUPPRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * \addtogroup core
 * \{
 */

/**
 * Noise suppression stage for the analog receive path (RNNoise, 48 kHz).
 *
 * The stage runs on 480-sample (10 ms) blocks, which is exactly what RNNoise
 * wants and exactly what the CSK6 audio codec delivers per DSP frame, so it
 * adds no buffering delay of its own: the only latency it contributes is
 * RNNoise's own one-frame algorithmic delay.
 *
 * Everything here is inert while the setting is off: no allocation, no state
 * initialisation and no per-sample work, just one boolean test per 10 ms frame
 * (see docs/NOISE.md).
 */

/** Size of one processing block, in 48 kHz samples (RNNoise frame size). */
#define NOISE_SUPPRESS_BLOCK 480

/**
 * Initialise the stage from the current settings. Safe to call more than once.
 */
void noiseSuppress_init(void);

/**
 * Enable or disable the stage. Enabling initialises RNNoise state lazily on
 * first use; disabling only clears the flag, so the buffers stay allocated for
 * the next enable (they are in PSRAM, not in the internal RAM budget).
 *
 * @param enabled: true to process audio, false to pass it through untouched.
 * @return the state actually in effect: false if RNNoise is not compiled in or
 *         its state does not fit (the caller can then reset the setting).
 */
bool noiseSuppress_setEnabled(bool enabled);

/**
 * @return true if the stage is enabled and initialised.
 */
bool noiseSuppress_enabled(void);

/**
 * Suppress noise in one block of 48 kHz samples, in place.
 *
 * @param block: samples, at least count long.
 * @param count: number of samples; anything other than NOISE_SUPPRESS_BLOCK is
 *               rejected, since the DSP frame size is a hard contract.
 * @return true if the block was processed, false if it was left untouched
 *         (stage disabled, wrong block size, or initialisation failed).
 */
bool noiseSuppress_process(int16_t *block, size_t count);

/**
 * @return the per-frame cost in instruction counts measured on the target, for
 *         the diagnostics screen; negative if unknown.
 */
int32_t noiseSuppress_costEstimate(void);

/** \} */

#endif /* NOISE_SUPPRESS_H */
