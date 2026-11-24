/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_AUDIO_LATENCY_H
#define OPENRTX_AUDIO_LATENCY_H

/**
 * @file audio_latency.h
 * @brief Measure the delay of the audio path with a known marker.
 *
 * A DMR burst has to land inside a 30 ms timeslot of somebody else's frame, so
 * the radio has to know how long a sample takes to travel from the stream it
 * writes to the sample that leaves the loudspeaker (or the modulator).  That
 * delay is not a property of the firmware: it is the stream buffers plus the
 * codec FIFO plus the DSP hop plus the resampler, and on the C62 the DSP is an
 * image that gets replaced (the stock one runs at 16 kHz, the LSF one this
 * firmware needs at 48 kHz), so the number has to be *measured* on the radio it
 * is going to run on, not assumed.
 *
 * The measurement is a correlation: play a chirp into an output path, capture
 * the same signal from an input path, and find the lag of the correlation peak.
 * What the loop captures depends on how the two paths are wired:
 *
 *   SINK_SPK  -> SOURCE_MIC   acoustic or cable loop: codec, DSP, transducers
 *   SINK_RTX  -> SOURCE_RTX   only where the hardware has an internal loop
 *
 * The marker is a linear chirp of DMR_LAT_MARKER_SAMPLES at 48 kHz with a Hann
 * envelope: a chirp correlates to a peak about 1/bandwidth wide, which is what
 * turns "the loudest sample" into a sub-millisecond delay with an uncertainty
 * that can be reported.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Samples of the marker at 48 kHz: 10 ms. */
#define DMR_LAT_MARKER_SAMPLES 480

/** Lowest and highest frequency of the chirp, Hz. */
#define DMR_LAT_CHIRP_LO 400
#define DMR_LAT_CHIRP_HI 4000

/** Amplitude of the marker, Q15: a quarter of full scale, loud but not
 *  clipping the codec. */
#define DMR_LAT_AMPLITUDE 8192

/**
 * Generate the marker: a Hann windowed linear chirp.
 *
 * @param out: DMR_LAT_MARKER_SAMPLES samples.
 * @param sampleRate: rate the marker is played at (48 kHz on the radio).
 */
void audioLatency_marker(int16_t *out, uint32_t sampleRate);

/**
 * Find the marker in a capture.
 *
 * @param capture: samples captured from the input path.
 * @param count: number of samples.
 * @param from: first sample of the capture that can hold the marker: the
 *              number of samples the caller wrote before starting to record
 *              (see the measurement procedure in the mode).
 * @param sampleRate: capture rate.
 * @param delayUs: destination of the delay, microseconds, from the first
 *                 sample the capture could hold.
 * @param spreadUs: destination of the half width of the correlation peak,
 *                  microseconds: how precisely the delay is known.
 * @return false if there is no peak to speak of (nothing was captured).
 */
bool audioLatency_measure(const int16_t *capture, size_t count, size_t from,
                          uint32_t sampleRate, int32_t *delayUs,
                          int32_t *spreadUs);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_AUDIO_LATENCY_H */
