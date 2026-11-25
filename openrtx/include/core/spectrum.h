/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sweep engine of the spectrum analyser.
 *
 * The BK4819 has no I/Q output: what it can be asked for is the RSSI of the
 * frequency it is tuned to, one 0.5 dB step at a time (REG_67), and the audio
 * of the demodulator. A spectrum is therefore built by tuning the receiver
 * across a span, one bin at a time, and keeping the level of each bin: the
 * resolution is the IF filter and the price is one PLL settling per point.
 * This file is the arithmetic of that: the raster, the trace, the peak and the
 * maximum hold. It knows nothing about the radio, the clock or the screen, so
 * the host test can feed it a synthetic front end and check the scaling.
 *
 * The raster is absolute: every bin sits on a multiple of the step, and the
 * centre of the span is always a bin. That makes two sweeps of the same
 * frequency comparable bin by bin (the maximum hold needs it) and means the
 * operator can read a bin's frequency without knowing the sweep started at an
 * arbitrary offset.
 */

/** Bins of one sweep. 160 is the width of the C62 panel in pixels: a wider
 *  raster would be drawn with more than one bin per pixel and lose detail
 *  rather than gain it. */
#define SPECTRUM_MAX_BINS 160

/** Finest step the receiver can tune to: REG_38/39 are 10 Hz words. */
#define SPECTRUM_MIN_STEP 10000U

/** Coarsest step worth sweeping: above this a span has fewer than 9 bins. */
#define SPECTRUM_MAX_STEP 5000000U

/** Largest span the analyser accepts, Hz. */
#define SPECTRUM_MAX_SPAN 96000000U

/**
 * The raster of one sweep.
 *
 * `bins` is what the engine really uses: a requested span/step pair that would
 * need more than SPECTRUM_MAX_BINS bins is served with a coarser step (see
 * spectrum_configure()), so `step` and `span` here are the ones in use, not
 * always the ones requested.
 */
typedef struct
{
    uint32_t centre;  ///< Centre of the span, Hz (a bin)
    uint32_t span;    ///< Span really swept, Hz: (bins - 1) * step
    uint32_t step;    ///< Step really used, Hz
    uint32_t low;     ///< Frequency of bin 0, Hz
    uint32_t high;    ///< Frequency of the last bin, Hz
    uint16_t bins;    ///< Number of bins, odd and >= 3
} spectrumScan_t;

/** One analyser instance: the raster, the trace and the statistics. */
typedef struct
{
    spectrumScan_t scan;
    int16_t trace[SPECTRUM_MAX_BINS];  ///< Level in dBm, averaged
    int16_t hold[SPECTRUM_MAX_BINS];   ///< Maximum held, only with hold on
    int16_t peakDbm;                   ///< Strongest bin of the last sweep
    uint16_t peakBin;                  ///< Its bin
    uint16_t index;                    ///< Bin being swept
    uint32_t sweeps;                   ///< Completed sweeps since the reset
    uint8_t smooth;                    ///< 0 off, else average of 2^n sweeps
    bool holdOn;                       ///< Maximum hold enabled
    bool valid;                        ///< A complete sweep has been stored
} spectrum_t;

/** Clear the trace, the hold and the statistics. */
void spectrum_reset(spectrum_t *s);

/**
 * Set the raster of a sweep.
 *
 * The requested step is raised, in the sequence 10, 12.5, 25, 50, 100, 250,
 * 500 kHz ... up to SPECTRUM_MAX_STEP, until the span fits in
 * SPECTRUM_MAX_BINS bins. The centre is moved down to the nearest multiple of
 * the step so that every bin is an absolute frequency. A span smaller than two
 * steps is widened to three bins.
 *
 * @param s: analyser.
 * @param centre: requested centre, Hz.
 * @param span: requested span, Hz.
 * @param step: requested step, Hz.
 * @return true if a raster was set; false if the request was impossible (a
 *         centre below the first bin's frequency, or a span of zero).
 */
bool spectrum_configure(spectrum_t *s, uint32_t centre, uint32_t span,
                        uint32_t step);

/** Number of bins of the current raster, 0 before the first configure. */
uint16_t spectrum_bins(const spectrum_t *s);

/** Frequency of one bin, Hz. */
uint32_t spectrum_binFreq(const spectrum_t *s, uint16_t bin);

/** Bin that contains a frequency, clamped to the raster. */
uint16_t spectrum_binOf(const spectrum_t *s, uint32_t freq);

/** Start a sweep at bin 0. The trace of the previous one stays on screen. */
void spectrum_startSweep(spectrum_t *s);

/**
 * Store the level of the bin being swept and advance to the next one.
 *
 * The stored value is averaged with the previous sweep's when smoothing is on,
 * the maximum hold is updated from it and the peak of the sweep is tracked, so
 * that a caller only ever has to read one number per bin.
 *
 * @param s: analyser.
 * @param dbm: RSSI of the bin, dBm.
 */
void spectrum_store(spectrum_t *s, int16_t dbm);

/** True once every bin of the current sweep has a value. */
bool spectrum_done(const spectrum_t *s);

/** Averaged level of one bin, dBm. Zero for a bin never swept. */
int16_t spectrum_level(const spectrum_t *s, uint16_t bin);

/** Held maximum of one bin, dBm. */
int16_t spectrum_holdLevel(const spectrum_t *s, uint16_t bin);

/** Level of the strongest bin of the last sweep, dBm. */
int16_t spectrum_peak(const spectrum_t *s);

/** Frequency of that bin, Hz. */
uint32_t spectrum_peakFreq(const spectrum_t *s);

/** Turn the maximum hold on (it starts from the current trace) or off. */
void spectrum_setHold(spectrum_t *s, bool on);

/** Drop the held maxima and start again from the current trace. */
void spectrum_clearHold(spectrum_t *s);

/** Sweeps averaged per bin: 0 off, 1 two sweeps, 2 four, 3 eight. */
void spectrum_setSmooth(spectrum_t *s, uint8_t shift);

#ifdef __cplusplus
}
#endif

#endif /* SPECTRUM_H */
