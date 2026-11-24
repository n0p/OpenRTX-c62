/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file YsfPhy.c
 * @brief Yaesu System Fusion receive PHY, see YsfPhy.h.
 */

#include "protocols/YSF/YsfPhy.h"
#include "protocols/YSF/YsfMem.h"

#ifdef CONFIG_YSF

#include <string.h>

/** Samples per symbol at 48 kHz: 48000 / 4800, exactly 10. */
#define SPS 10

/** Samples in one frame. */
#define FRAME_SAMPLES (YSF_FRAME_SYMBOLS * SPS)

/**
 * Raw-sample ring: one frame (4800) plus the largest block the audio stream
 * hands out (2400), so a frame is always complete inside it.  Not a power of
 * two -- that would cost 3 KB more for a mask -- so the wrap is a compare.
 *
 * The ring is 14.4 KB, the largest single buffer of the mode, and it goes to
 * PSRAM (YsfMem.h).  It is touched at 48 kHz when it is filled and at symbol
 * rate when it is read, not inside a per-sample loop, and the codec already
 * hands the samples over through PSRAM buffers.
 */
#define RING_LEN 7360

/** Sync allowed to lose this many of its 20 symbols. */
#define SYNC_MAX_ERRORS 3

/** Frames a coasted timing estimate is allowed to carry the receiver. */
#define MAX_COAST 3

/** Frames without a sync before the receiver hunts again (1 s). */
#define SYNC_TIMEOUT_FRAMES 10

/** Coarse hunt step, in samples: a quarter of a symbol. */
#define HUNT_STEP 5

/** Timing refinement: +-8 samples around the expected position, one by one. */
#define TRACK_SPAN 8

/**
 * Sign of every sync symbol, 1 for a negative deviation: dibits
 * 3,1,1,0,1,3,0,1,3,0,2,1,1,2,0,3,1,0,3,1 with 0/1 positive and 2/3 negative.
 */
static const int8_t syncSign[YSF_SYNC_SYMBOLS] = {
    -1, +1, +1, +1, +1, -1, +1, +1, -1, +1,
    -1, +1, +1, -1, +1, -1, +1, +1, -1, +1
};

static struct
{
    long ringIndex;   ///< Total samples written; the last one is ringIndex-1
    float pos;        ///< Next frame's first sync symbol, in samples
    float timing;     ///< Fractional offset applied to the symbol grid
    int hunt;         ///< No lock: scan for a sync
    int coast;        ///< Frames since the last sync correlation succeeded
    long lastSync;    ///< Sample index of the last accepted sync
    uint8_t errors;   ///< Sync errors of the last accepted frame
    struct ysfFrame frame;
} phy;

/**
 * The raw samples themselves, in PSRAM: the only part of the demodulator's
 * state big enough to matter.  The scalars above stay in SRAM because they are
 * touched once per sample.
 */
static int16_t ring[RING_LEN] YSF_EXT_RAM;

/* ------------------------------------------------------------------ */
/* Reading the ring                                                    */
/* ------------------------------------------------------------------ */

/** Ring index for an absolute sample position. */
static inline unsigned ring_slot(long i)
{
    long k = i % RING_LEN;

    return (unsigned)((k < 0) ? (k + RING_LEN) : k);
}

/** Sample at a fractional position, 4-point Lagrange. */
static float sample_at(float at)
{
    long i = (long)at;
    float f = at - (float)i;

    if ((i < phy.ringIndex - RING_LEN + 3) || (i > phy.ringIndex - 3))
        return 0.0f;

    float xm1 = ring[ring_slot(i - 1)];
    float x0 = ring[ring_slot(i)];
    float x1 = ring[ring_slot(i + 1)];
    float x2 = ring[ring_slot(i + 2)];

    if (f == 0.0f)
        return x0;

    /* Lagrange through f = -1, 0, 1, 2, in Horner form */
    float a = (-xm1 + 3.0f * x0 - 3.0f * x1 + x2) * (1.0f / 6.0f);
    float b = (xm1 - 2.0f * x0 + x1) * 0.5f;
    float c = (-2.0f * xm1 - 3.0f * x0 + 6.0f * x1 - x2) * (1.0f / 6.0f);

    return ((a * f + b) * f + c) * f + x0;
}

/**
 * Sync score at a frame start: how many of the 20 sync symbols carry the
 * expected sign.  Comparing signs makes the search independent of level,
 * deviation and channel gain, which is what lets it run before any level
 * estimate exists.
 */
static int sync_score(float start)
{
    int good = 0;

    for (int i = 0; i < YSF_SYNC_SYMBOLS; i++)
    {
        float y = sample_at(start + (float)(i * SPS));

        if (y * (float)syncSign[i] > 0.0f)
            good++;
    }

    return good;
}

/** Dibits of the 20 sync symbols: 0xD471C9634D, two bits per symbol. */
static const uint8_t syncDibits[YSF_SYNC_SYMBOLS] = {
    3, 1, 1, 0, 1, 3, 0, 1, 3, 0, 2, 1, 1, 2, 0, 3, 1, 0, 3, 1
};

/** Median of n values, insertion sort (n is 20 here, so it is free). */
static float median_of(float *v, int n)
{
    for (int i = 1; i < n; i++)
    {
        float key = v[i];
        int j = i - 1;

        while ((j >= 0) && (v[j] > key))
        {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }

    return 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/**
 * Full sync check: slice the 20 sync symbols against levels taken from the
 * sync itself and compare the 40 bits with the expected pattern.
 *
 * The sign-only score above is fine for tracking but far too weak to acquire
 * on: at 9600 candidate positions per second a 17-of-20 sign match happens by
 * chance often enough to lock onto noise, and a false lock then wanders for
 * about a second before the receiver finds the real carrier (measured: the
 * first 13 frames of the reference capture decoded nowhere).  Forty bits make
 * a false alarm essentially impossible.  The levels come from the sync symbols
 * themselves -- the median of their magnitudes splits the inner and the outer
 * cluster -- so the check works before any frame-wide estimate exists.
 *
 * @return the number of differing bits (0..40), or -1 if the levels cannot be
 *         separated (not a 4-level signal).
 */
static int sync_verify(float start)
{
    float y[YSF_SYNC_SYMBOLS];
    float mag[YSF_SYNC_SYMBOLS];
    float v[YSF_SYNC_SYMBOLS];

    for (int i = 0; i < YSF_SYNC_SYMBOLS; i++)
        y[i] = sample_at(start + (float)(i * SPS));

    memcpy(v, y, sizeof(v));
    float centre = median_of(v, YSF_SYNC_SYMBOLS);

    for (int i = 0; i < YSF_SYNC_SYMBOLS; i++)
        mag[i] = (y[i] < centre) ? (centre - y[i]) : (y[i] - centre);

    memcpy(v, mag, sizeof(v));
    float split = median_of(v, YSF_SYNC_SYMBOLS);

    float sumInner = 0.0f, sumOuter = 0.0f;
    int nInner = 0, nOuter = 0;

    for (int i = 0; i < YSF_SYNC_SYMBOLS; i++)
    {
        if (mag[i] > split)
        {
            sumOuter += mag[i];
            nOuter++;
        }
        else
        {
            sumInner += mag[i];
            nInner++;
        }
    }

    if ((nInner == 0) || (nOuter == 0))
        return -1;

    float inner = sumInner / (float)nInner;
    float outer = sumOuter / (float)nOuter;

    if ((inner <= 0.0f) || (outer < 1.5f * inner))
        return -1;

    float threshold = 0.5f * (inner + outer);
    int errors = 0;

    for (int i = 0; i < YSF_SYNC_SYMBOLS; i++)
    {
        float d = y[i] - centre;
        float a = (d < 0.0f) ? -d : d;
        uint8_t high = (a > threshold);
        uint8_t got = (d >= 0.0f) ? (high ? 1 : 0) : (high ? 3 : 2);

        errors += ((got >> 1) != ((syncDibits[i] >> 1) & 1));
        errors += ((got & 1) != (syncDibits[i] & 1));
    }

    return errors;
}

/** Histogram median of |x| over n values, in the sample domain. */
static float magnitude_median(const float *x, int n)
{
    float peak = 0.0f;

    for (int i = 0; i < n; i++)
        if (x[i] < 0.0f ? -x[i] > peak : x[i] > peak)
            peak = x[i] < 0.0f ? -x[i] : x[i];

    if (peak <= 0.0f)
        return 0.0f;

    /* 64 bins, two passes: count, then find the bin holding the middle */
    uint16_t bins[64];

    memset(bins, 0, sizeof(bins));
    float scale = 64.0f / peak;

    for (int i = 0; i < n; i++)
    {
        float a = (x[i] < 0.0f) ? -x[i] : x[i];
        unsigned bin = (unsigned)(a * scale);

        if (bin > 63)
            bin = 63;
        bins[bin]++;
    }

    unsigned half = (unsigned)n / 2;
    unsigned acc = 0;
    unsigned bin = 0;

    for (bin = 0; bin < 64; bin++)
    {
        if (acc + bins[bin] >= half)
            break;
        acc += bins[bin];
    }

    return ((float)bin + 0.5f) / scale;
}

/** Mean of |x| over the values on one side of a threshold. */
static float magnitude_mean(const float *x, int n, float threshold, int above,
                           float fallback)
{
    float sum = 0.0f;
    int count = 0;

    for (int i = 0; i < n; i++)
    {
        float a = (x[i] < 0.0f) ? -x[i] : x[i];

        if ((above && (a > threshold)) || (!above && (a <= threshold)))
        {
            sum += a;
            count++;
        }
    }

    return (count > 0) ? (sum / (float)count) : fallback;
}

/* ------------------------------------------------------------------ */
/* Frame emission                                                      */
/* ------------------------------------------------------------------ */

static void emit(float start, uint8_t syncErrors, ysfFrameHandler handler,
                 void *user)
{
    float sym[YSF_FRAME_SYMBOLS];
    float dc = 0.0f;
    float inner, outer;
    float mid, outerGate;
    float error = 0.0f;
    int outerCount = 0;

    for (int i = 0; i < YSF_FRAME_SYMBOLS; i++)
        sym[i] = sample_at(start + (float)(i * SPS));

    /* DC is the carrier offset seen by the discriminator */
    for (int i = 0; i < YSF_FRAME_SYMBOLS; i++)
        dc += sym[i];
    dc /= (float)YSF_FRAME_SYMBOLS;

    for (int i = 0; i < YSF_FRAME_SYMBOLS; i++)
        sym[i] -= dc;

    /*
     * The frame is a near 50/50 mix of inner and outer symbols, so the median
     * of the magnitude sits between the two levels: split there, and each side
     * gives the level of its cluster.
     */
    float median = magnitude_median(sym, YSF_FRAME_SYMBOLS);

    inner = magnitude_mean(sym, YSF_FRAME_SYMBOLS, median, 0, 0.0f);
    outer = magnitude_mean(sym, YSF_FRAME_SYMBOLS, median, 1, inner * 3.0f);

    if ((inner <= 0.0f) || (outer <= inner))
    {
        inner = (inner > 0.0f) ? inner : 1.0f;
        outer = inner * 3.0f;
    }

    mid = 0.5f * (inner + outer);
    outerGate = mid;

    phy.frame.syncErrors = syncErrors;

    for (int i = 0; i < YSF_FRAME_SYMBOLS; i++)
    {
        float y = sym[i];
        float a = (y < 0.0f) ? -y : y;
        bool high = (a > outerGate);
        float nominal = high ? outer : inner;
        uint8_t dibit;

        /* 0/1 are the positive deviations, 2/3 the negative ones */
        dibit = (y >= 0.0f) ? (high ? 1 : 0) : (high ? 3 : 2);

        phy.frame.dibits[i] = dibit;

        if (i >= YSF_SYNC_SYMBOLS)
        {
            float want = (y >= 0.0f) ? nominal : -nominal;

            error += (y - want) * (y - want);
            outerCount++;
        }
    }

    if (outerCount > 0)
    {
        /* EVM relative to the nominal level of each symbol, in percent */
        float rms = error / (float)outerCount;
        float reference = 0.5f * (inner + outer);

        if (reference <= 0.0f)
            reference = 1.0f;
        rms /= (reference * reference);
        phy.frame.evm = (rms >= 1.0f) ? 100 : (uint8_t)(100.0f * rms + 0.5f);
    }
    else
    {
        phy.frame.evm = 100;
    }

    if (handler != NULL)
        handler(&phy.frame, user);
}

/* ------------------------------------------------------------------ */
/* Hunt and track                                                      */
/* ------------------------------------------------------------------ */

/**
 * While hunting: find the best sync in the samples now in the ring, starting
 * HUNT_STEP samples after the last position tried, and refine it sample by
 * sample.  On success the lock is taken and the frame that starts there is
 * emitted by the track path once its last symbol is in.
 */
static void hunt(void)
{
    long limit = phy.ringIndex - FRAME_SAMPLES;

    while (phy.pos <= (float)limit)
    {
        int score = sync_score(phy.pos);

        if (score >= (YSF_SYNC_SYMBOLS - SYNC_MAX_ERRORS))
        {
            float best = phy.pos;
            int bestScore = score;

            for (int d = -TRACK_SPAN; d <= TRACK_SPAN; d++)
            {
                int s = sync_score(phy.pos + (float)d);

                if (s > bestScore)
                {
                    bestScore = s;
                    best = phy.pos + (float)d;
                }
            }

            if (sync_verify(best) > (2 * SYNC_MAX_ERRORS))
            {
                /* Signs matched but the levels or the bits did not: noise. */
                phy.pos += (float)HUNT_STEP;
                continue;
            }

            phy.pos = best;
            phy.hunt = 0;
            phy.coast = 0;
            phy.lastSync = (long)best;
            return;
        }

        phy.pos += (float)HUNT_STEP;
    }
}

/** While locked: correlate the next sync around its expected position. */
static void track(ysfFrameHandler handler, void *user)
{
    float expected = phy.pos;
    float best = expected;
    int bestScore = sync_score(expected);

    for (int d = -TRACK_SPAN; d <= TRACK_SPAN; d++)
    {
        int s = sync_score(expected + (float)d);

        if (s > bestScore)
        {
            bestScore = s;
            best = expected + (float)d;
        }
    }

    if (bestScore >= (YSF_SYNC_SYMBOLS - SYNC_MAX_ERRORS))
    {
        phy.coast = 0;
        phy.lastSync = (long)best;
        phy.errors = (uint8_t)(YSF_SYNC_SYMBOLS - bestScore);
    }
    else
    {
        /* No sync where it should be: coast on the frame cadence for a while */
        phy.coast++;
        best = expected;
        phy.errors = (uint8_t)(YSF_SYNC_SYMBOLS - bestScore);
    }

    emit(best, phy.errors, handler, user);
    phy.pos = best + (float)FRAME_SAMPLES;

    if ((phy.coast > MAX_COAST) ||
        ((phy.ringIndex - phy.lastSync) > (long)(SYNC_TIMEOUT_FRAMES * FRAME_SAMPLES)))
    {
        /*
         * Drop the lock and hunt forward from here, never backwards: rewinding
         * to the last good sync would re-emit frames that are already out.
         */
        phy.hunt = 1;
        phy.pos = (float)phy.ringIndex;
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                             */
/* ------------------------------------------------------------------ */

void ysfPhy_init(void)
{
    memset(&phy, 0, sizeof(phy));
    memset(ring, 0, sizeof(ring));
    phy.hunt = 1;
}

void ysfPhy_reset(void)
{
    memset(ring, 0, sizeof(ring));
    phy.ringIndex = 0;
    phy.pos = 0.0f;
    phy.timing = 0.0f;
    phy.hunt = 1;
    phy.coast = 0;
    phy.lastSync = -1000000;
}

bool ysfPhy_synced(void)
{
    return (phy.hunt == 0) &&
           ((phy.ringIndex - phy.lastSync) < (long)(2 * FRAME_SAMPLES));
}

/** Append samples to the ring. */
static void append(const int16_t *samples, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        ring[ring_slot(phy.ringIndex)] = samples[i];
        phy.ringIndex++;
    }
}

void ysfPhy_push(const int16_t *samples, size_t count, ysfFrameHandler handler,
                 void *user)
{
    size_t done = 0;

    if (samples == NULL)
        return;

    /*
     * Serve the frame clock without a branch per sample: while hunting, take
     * the whole block and scan it; once locked, take exactly as many samples
     * as the current frame still needs, so track() runs the moment the frame
     * is complete -- the ring only holds one frame plus a sync of margin, and
     * a late frame would read samples that have already wrapped.
     */
    while (done < count)
    {
        if (phy.hunt)
        {
            append(&samples[done], count - done);
            hunt();
            break;
        }

        long want = (long)phy.pos + FRAME_SAMPLES - phy.ringIndex;

        if (want <= 0)
        {
            track(handler, user);
            continue;
        }

        size_t n = ((size_t)want < (count - done)) ? (size_t)want : (count - done);

        append(&samples[done], n);
        done += n;

        if (phy.ringIndex >= (long)phy.pos + FRAME_SAMPLES)
            track(handler, user);
    }
}

#else /* CONFIG_YSF */

void ysfPhy_init(void) {}
void ysfPhy_reset(void) {}
bool ysfPhy_synced(void) { return false; }
void ysfPhy_push(const int16_t *samples, size_t count, ysfFrameHandler handler,
                 void *user)
{
    (void)samples;
    (void)count;
    (void)handler;
    (void)user;
}

#endif /* CONFIG_YSF */
