/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Measurement harness for the airband AM audio chain: the same AGC the radio
 * runs (core/agc.c) on AM audio with the level profile of the band.
 *
 * What the BK4819 AM detector hands to the codec is the voice times the carrier
 * amplitude, and the carrier amplitude is the field strength: 60 dB of change
 * across the band and more within seconds when an aircraft passes. This
 * harness generates exactly that -- three tones with a syllable envelope,
 * scaled by a level profile, plus the noise floor of the receiver -- and then
 * measures what the operator ends up hearing:
 *
 *  - the transfer curve: with a steady voice, what comes out for a carrier
 *    from -50 to 0 dBFS, which is the compression the AGC gives;
 *  - a six second capture with a 55 dB climb and two fades, where the figures
 *    are the level spread left at the speaker and the samples the codec would
 *    have clipped without any level control;
 *  - the cost, by building the same file for QEMU with MARK_FRAMES: the TCG
 *    plugin counts the instructions between two markers, and a marker is one
 *    second of audio with nothing running in between but agc_process().
 *
 * Build for the host:
 *
 *   cc -O2 -I openrtx/include tests/platform/c62_airband_agc_sim.c \
 *      openrtx/src/core/agc.c -lm -o /tmp/am_sim && /tmp/am_sim /tmp
 *
 * Build for QEMU (mps2-an505, Cortex-M33) as
 * tests/platform/c62_dmr_phy_cost.c does:
 *
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O2 -DMARK_FRAMES \
 *     -I openrtx/include tests/platform/c62_airband_agc_sim.c \
 *     openrtx/src/core/agc.c -T work/dsp-probe/common/mps2.ld \
 *     -specs=rdimon.specs -lrdimon -lm -o /tmp/am_sim.elf
 *   INSNHIST_MARKER=$(arm-none-eabi-nm /tmp/am_sim.elf |
 *                     awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel /tmp/am_sim.elf \
 *     -plugin file=work/dsp-probe/common/libinsnframe.so -d plugin
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/agc.h"

#ifdef MARK_FRAMES
__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }
#define MARK() frame_marker()
#else
#define MARK() do { } while (0)
#endif

#define RATE 48000

/*
 * COST_ONLY is the QEMU build: one second of noise is enough to count the
 * instructions per sample, and it leaves the transcendental functions and the
 * reports out of an emulation that would otherwise spend minutes in them.
 */
#ifdef COST_ONLY
#define SECONDS 1
#define MARK_INTERVAL 12000
#else
#define SECONDS 6
#define MARK_INTERVAL RATE
#endif

#define SAMPLES (RATE * SECONDS)

/* Frame the AGC runs on: 1 ms, the granularity of the codec worker */
#define FRAME 48

/* Longest block used anywhere here */
#define BLOCK SAMPLES

static int16_t capture[SAMPLES];
static int16_t block[BLOCK];

/* Deterministic noise: the same capture on the host and in QEMU */
static uint32_t noiseState = 0x12345678;

static int16_t noiseSample(void)
{
    noiseState = noiseState * 1664525u + 1013904223u;
    return (int16_t)((int32_t)(noiseState >> 16) - 32768);
}

/**
 * Voice: three tones with a syllable envelope, the shape of a controller
 * reading a clearance.
 */
static double voice(size_t n)
{
    const double t = (double)n / RATE;
    const double syllable = 0.55 + 0.45 * sin(2.0 * M_PI * 2.7 * t);
    const double tones = sin(2.0 * M_PI * 420.0 * t)
                       + 0.7 * sin(2.0 * M_PI * 980.0 * t)
                       + 0.4 * sin(2.0 * M_PI * 2210.0 * t);

    return 0.45 * syllable * tones / 2.1;
}

/**
 * Carrier amplitude of the capture, in dB relative to the reference that fills
 * the converter: a slow 48 dB climb and two fades, one of 25 dB for 40 ms and
 * one of 40 dB for 120 ms, as a passing aircraft makes.
 */
static double captureLevelDb(size_t n)
{
    const double t = (double)n / RATE;
    double db = -48.0 + 48.0 * t / SECONDS;

    const double fade1 = t - 2.0;
    if ((fade1 > 0.0) && (fade1 < 0.04))
        db -= 25.0 * sin(M_PI * fade1 / 0.04);

    const double fade2 = t - 4.0;
    if ((fade2 > 0.0) && (fade2 < 0.12))
        db -= 40.0 * sin(M_PI * fade2 / 0.12);

    return db;
}

/**
 * Fill a buffer with voice at the given carrier level, plus the receiver noise
 * floor at -42 dB relative to full scale.
 *
 * @param v: destination.
 * @param samples: samples to fill.
 * @param levelDb: carrier level, dBFS.
 */
static void fill(int16_t *v, size_t samples, double levelDb)
{
    const double amplitude = pow(10.0, levelDb / 20.0);

    for (size_t n = 0; n < samples; n++) {
        const double noise = (double)noiseSample() / 32768.0 * 0.008;
        double s = voice(n) * amplitude + noise;

        s = (s > 1.0) ? 1.0 : ((s < -1.0) ? -1.0 : s);
        v[n] = (int16_t)lround(s * 32767.0);
    }
}

/**
 * Peak level of a block, in dB relative to full scale.
 *
 * @param v: samples.
 * @param samples: block length.
 * @return the peak, dBFS.
 */
static double peakDb(const int16_t *v, size_t samples)
{
    int32_t peak = 0;

    for (size_t i = 0; i < samples; i++) {
        int32_t s = (v[i] < 0) ? -v[i] : v[i];
        peak = (s > peak) ? s : peak;
    }

    return 20.0 * log10(((peak > 0) ? peak : 1) / 32768.0);
}

/**
 * Run the AGC over a block in 1 ms frames.
 *
 * @param a: instance, already initialised.
 * @param v: samples, processed in place.
 * @param samples: block length.
 */
static void run(struct agc *a, int16_t *v, size_t samples)
{
    for (size_t n = 0; n + FRAME <= samples; n += FRAME)
        agc_process(a, &v[n], FRAME, 1);
}

/**
 * Transfer curve: steady voice at each carrier level, long enough for the AGC
 * to settle, and the level it leaves at the speaker.
 */
static void transferCurve(void)
{
    static const double levels[] = { -50, -45, -40, -35, -30, -25,
                                     -20, -15, -10, -5, 0 };

    printf("transfer curve (steady voice, settled level)\n");
    printf("  carrier   input    output     gain\n");

    for (size_t i = 0; i < (sizeof(levels) / sizeof(levels[0])); i++) {
        struct agc a;
        agc_init(&a, NULL);

        const size_t tail = RATE / 10;

        /* Two seconds: long enough for +24 dB of release to settle */
        const size_t length = 2 * RATE;

        fill(block, length, levels[i]);
        double in = peakDb(&block[length - tail], tail);

        run(&a, block, length);
        double out = peakDb(&block[length - tail], tail);

        printf("  %5.0f dB  %6.1f  %6.1f  %6.1f\n", levels[i], in, out,
               out - in);
    }
}

int main(int argc, char *argv[])
{
    const char *outDir = (argc > 1) ? argv[1] : NULL;

#ifdef COST_ONLY
    /* Noise with a fading envelope, no libm: the per-sample work is the same */
    for (size_t n = 0; n < SAMPLES; n++)
        capture[n] = noiseSample() >> ((n / (RATE / 8)) & 7);
#else
    /* The capture, as the AM detector would hand it over */
    for (size_t n = 0; n < SAMPLES; n++) {
        const double amplitude = pow(10.0, captureLevelDb(n) / 20.0);
        const double noise = (double)noiseSample() / 32768.0 * 0.008;
        double s = voice(n) * amplitude + noise;

        s = (s > 1.0) ? 1.0 : ((s < -1.0) ? -1.0 : s);
        capture[n] = (int16_t)lround(s * 32767.0);
    }
#endif

    if (outDir != NULL) {
        char path[256];
        snprintf(path, sizeof(path), "%s/am_in.raw", outDir);
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(capture, sizeof(int16_t), SAMPLES, f);
            fclose(f);
        }
    }

    int16_t *in = malloc(SAMPLES * sizeof(int16_t));
    memcpy(in, capture, SAMPLES * sizeof(int16_t));

    /*
     * The AGC alone, in the frame size the codec worker uses, with a marker
     * every MARK_INTERVAL samples: that is the interval the QEMU plugin counts.
     */
    struct agc a;
    agc_init(&a, NULL);

#ifndef COST_ONLY
    /*
     * Pre-roll: two seconds on the weakest carrier of the capture, so that the
     * run below measures tracking and not the two seconds the loop needs to
     * climb from unity to its ceiling. Those are in the transfer curve.
     */
    fill(block, 2 * RATE, captureLevelDb(0));
    run(&a, block, 2 * RATE);
#endif

    int32_t peakGain = 0;
    int32_t minGain = 0x7fffffff;
    double secondIn[SECONDS], secondOut[SECONDS];

    for (size_t n = 0; n < SAMPLES; n += FRAME) {
        if ((n % MARK_INTERVAL) == 0)
            MARK();

#ifndef COST_ONLY
        if ((n % RATE) == 0)
            secondIn[n / RATE] = peakDb(&in[n], RATE);
#endif

        agc_process(&a, &capture[n], FRAME, 1);

#ifndef COST_ONLY
        if (((n + FRAME) % RATE) == 0)
            secondOut[(n + FRAME) / RATE - 1] =
                peakDb(&capture[n + FRAME - RATE], RATE);
#endif

        int32_t g = agc_getGain(&a);
        peakGain = (g > peakGain) ? g : peakGain;
        minGain = (g < minGain) ? g : minGain;
    }

#ifdef COST_ONLY
    printf("airband AGC cost run: %d samples, gain %.1f dB\n", SAMPLES,
           20.0 * log10((double)peakGain / AGC_UNITY));
    return 0;
#else

    /* Level spread, over the windows that carry a syllable */
    const size_t window = RATE / 10;
    double inMin = 1e9, inMax = -1e9, outMin = 1e9, outMax = -1e9;
    double outSum = 0.0, outSumSq = 0.0;
    size_t windows = 0, clippedIn = 0, clippedOut = 0;

    for (size_t start = 0; (start + window) <= SAMPLES; start += window) {
        double i = peakDb(&in[start], window);

        /*
         * Below this the AGC is at its ceiling and the level follows the
         * carrier: out of the range this measures.
         */
        if (i < -30.0)
            continue;

        /*
         * The two fades are the carrier disappearing, not a level to hold:
         * what the loop does there is reported separately, so keep them out of
         * the spread.
         */
        const double t = (double)start / RATE;
        if (((t > 1.9) && (t < 2.2)) || ((t > 3.9) && (t < 4.4)))
            continue;

        double o = peakDb(&capture[start], window);
        inMin = (i < inMin) ? i : inMin;
        inMax = (i > inMax) ? i : inMax;
        outMin = (o < outMin) ? o : outMin;
        outMax = (o > outMax) ? o : outMax;
        outSum += o;
        outSumSq += o * o;
        windows++;
    }

    for (size_t n = 0; n < SAMPLES; n++) {
        if ((in[n] >= 32767) || (in[n] <= -32767))
            clippedIn++;
        if ((capture[n] >= 32767) || (capture[n] <= -32767))
            clippedOut++;
    }

    const double mean = outSum / (double)windows;
    const double var = (outSumSq / (double)windows) - mean * mean;

    printf("airband AGC, %d s at %d Hz, %d samples\n", SECONDS, RATE, SAMPLES);
    for (size_t s = 0; s < SECONDS; s++)
        printf("  %u s: carrier %6.1f dBFS, speaker %6.1f dBFS\n",
               (unsigned)s, secondIn[s], secondOut[s]);
    printf("level spread, carrier above -30 dBFS, %u windows: input "
           "%.1f..%.1f dBFS (%.1f dB), output %.1f..%.1f dBFS (%.1f dB)\n",
           (unsigned)windows, inMin, inMax, inMax - inMin, outMin, outMax,
           outMax - outMin);
    printf("output level mean %.1f dBFS, standard deviation %.1f dB\n", mean,
           sqrt((var > 0) ? var : 0));
    printf("gain range %.1f..%.1f dB\n",
           20.0 * log10((double)minGain / AGC_UNITY),
           20.0 * log10((double)peakGain / AGC_UNITY));
    printf("clipped samples: input %u, output %u of %d\n",
           (unsigned)clippedIn, (unsigned)clippedOut, SAMPLES);

    /* What the 40 dB fade of the capture does, and how long it takes to go */
    double worst = 1e9;
    size_t worstAt = 0;
    for (size_t start = 3 * RATE; (start + window) <= (5 * RATE);
         start += window) {
        double o = peakDb(&capture[start], window);
        if (o < worst) {
            worst = o;
            worstAt = start;
        }
    }
    double back = -1.0;
    for (size_t start = worstAt; (start + window) <= SAMPLES; start += window) {
        if (peakDb(&capture[start], window) > (worst + 3.0)) {
            back = (double)(start - worstAt) / RATE;
            break;
        }
    }
    printf("40 dB fade at 4.00 s: output falls to %.1f dBFS, back within "
           "3 dB after %.0f ms\n", worst, back * 1000.0);

    transferCurve();
#endif /* COST_ONLY */

    if (outDir != NULL) {
        char path[256];
        snprintf(path, sizeof(path), "%s/am_out.raw", outDir);
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(capture, sizeof(int16_t), SAMPLES, f);
            fclose(f);
        }
    }

    free(in);
    return 0;
}
