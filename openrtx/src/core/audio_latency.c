/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file audio_latency.c
 * @brief Delay measurement of the audio path, see audio_latency.h.
 *
 * The correlation is done in fixed point (Q15 in, 64 bit accumulator) so that
 * the measurement itself costs neither the FPU nor a libm call: the marker is
 * generated with sinf(), once per calibration.
 */

#include "core/audio_latency.h"

#include <math.h>
#include <string.h>

void audioLatency_marker(int16_t *out, uint32_t sampleRate)
{
    if (out == NULL)
        return;

    const float n = (float)DMR_LAT_MARKER_SAMPLES;
    const float t = n / (float)sampleRate;
    const float k = ((float)DMR_LAT_CHIRP_HI - (float)DMR_LAT_CHIRP_LO) / t;

    for (int i = 0; i < DMR_LAT_MARKER_SAMPLES; i++) {
        const float ti = (float)i / (float)sampleRate;

        /* Hann envelope: no click at either end, so the correlation peak is
         * the chirp and not the edge of the burst */
        const float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / n);
        const float phase = 2.0f * (float)M_PI
                          * ((float)DMR_LAT_CHIRP_LO * ti + 0.5f * k * ti * ti);
        const float v = (float)DMR_LAT_AMPLITUDE * w * sinf(phase);

        out[i] = (int16_t)((v > 32767.0f) ? 32767 : ((v < -32768.0f) ? -32768
                                                                   : v));
    }
}

/** Correlation of the capture with the marker at one lag, Q15 x Q15. */
static int64_t correlateAt(const int16_t *capture, const int16_t *marker,
                           size_t lag)
{
    int64_t acc = 0;

    for (size_t i = 0; i < DMR_LAT_MARKER_SAMPLES; i++)
        acc += (int64_t)capture[lag + i] * (int64_t)marker[i];

    return acc;
}

bool audioLatency_measure(const int16_t *capture, size_t count, size_t from,
                          uint32_t sampleRate, int32_t *delayUs,
                          int32_t *spreadUs)
{
    if ((capture == NULL) || (count < DMR_LAT_MARKER_SAMPLES)
        || (from + DMR_LAT_MARKER_SAMPLES > count) || (sampleRate == 0))
        return false;

    int16_t marker[DMR_LAT_MARKER_SAMPLES];

    audioLatency_marker(marker, sampleRate);

    /*
     * Coarse search: every lag that still holds a whole marker.  The capture
     * is a few tens of milliseconds, so the O(n*m) cost is a few million
     * multiply-accumulates, once, on a request.
     */
    const size_t last = count - DMR_LAT_MARKER_SAMPLES;
    size_t bestLag = from;
    int64_t best = correlateAt(capture, marker, from);

    for (size_t lag = from + 1; lag <= last; lag++) {
        const int64_t v = correlateAt(capture, marker, lag);

        if (v > best) {
            best = v;
            bestLag = lag;
        }
    }

    /* Nothing was captured: no peak worth the name */
    if (best <= 0)
        return false;

    /*
     * Sub-sample peak: a parabola through the correlation at lag-1, lag and
     * lag+1 places the peak to a fraction of a sample.  The width of the peak
     * at half its height, in samples, is the uncertainty: a chirp of B Hz
     * correlates to a peak of about 1/B seconds, and that is what the caller
     * has to add to its budget.
     */
    const int64_t prev = (bestLag > from)
                       ? correlateAt(capture, marker, bestLag - 1) : best;
    const int64_t next = (bestLag < last)
                       ? correlateAt(capture, marker, bestLag + 1) : best;

    const int64_t den = prev - 2 * best + next;
    int32_t fracQ8 = 0;

    if (den != 0) {
        fracQ8 = (int32_t)((128 * (prev - next)) / den);
        if (fracQ8 > 128)
            fracQ8 = 128;
        if (fracQ8 < -128)
            fracQ8 = -128;
    }

    int32_t half = 1;

    for (int32_t d = 1; d < 64; d++) {
        bool below = false;

        if ((bestLag + (size_t)d) <= last)
            below = correlateAt(capture, marker, bestLag + (size_t)d)
                    < (best / 2);
        if ((bestLag >= from + (size_t)d)
            && (correlateAt(capture, marker, bestLag - (size_t)d) < (best / 2)))
            below = true;
        if (below)
            break;
        half = d;
    }

    const int32_t lagQ8 = (((int32_t)bestLag - (int32_t)from) << 8) + fracQ8;

    if (delayUs != NULL) {
        *delayUs = (int32_t)(((int64_t)lagQ8 * 1000000)
                             / (256 * (int64_t)sampleRate));
    }
    if (spreadUs != NULL)
        *spreadUs = (int32_t)(((int64_t)half * 1000000) / (int64_t)sampleRate);

    return true;
}
