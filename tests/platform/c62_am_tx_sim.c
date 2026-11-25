/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Measurement harness for the airband AM transmit modulator (core/am_tx.c).
 *
 * The modulator turns the microphone into the duty cycle of the PA supply
 * control, so what it "sounds like" is the envelope of the carrier. This
 * harness drives it the way the radio does -- 48 kHz blocks of synthetic
 * speech, key-down and key-up -- and measures what the PA would radiate:
 *
 *  - the audio band, by sweeping a tone through the chain and measuring the
 *    envelope it produces;
 *  - the modulation depth, for the three depths the operator can choose and
 *    three microphone levels, as max and min of the envelope once the level
 *    control has settled;
 *  - the envelope distortion of a 1 kHz tone, as the harmonics of the envelope
 *    against its fundamental (this is what a receiver would hear as muddiness,
 *    and what an analyser sees as extra sidebands);
 *  - the ramps, so that the key-up and the key-down can be checked against the
 *    clicks they are there to avoid;
 *  - the carrier floor: the smallest duty the modulator ever asks for, which
 *    is what says whether the carrier reaches zero (overmodulation) anywhere;
 *  - the cost, built for QEMU with MARK_FRAMES as the other cost harnesses:
 *    the TCG plugin counts the instructions of one second of audio with
 *    nothing running in between but am_tx_process().
 *
 * Build for the host:
 *
 *   cc -O2 -I openrtx/include tests/platform/c62_am_tx_sim.c \
 *      openrtx/src/core/am_tx.c -lm -o /tmp/am_tx_sim && /tmp/am_tx_sim /tmp
 *
 * Build for QEMU (mps2-an505, Cortex-M33) as tests/platform/
 * c62_airband_agc_sim.c documents:
 *
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O2 -DMARK_FRAMES \
 *     -I openrtx/include tests/platform/c62_am_tx_sim.c \
 *     openrtx/src/core/am_tx.c -T work/dsp-probe/common/mps2.ld \
 *     -specs=rdimon.specs -lrdimon -lm -o /tmp/am_tx_sim.elf
 *   INSNHIST_MARKER=$(arm-none-eabi-nm /tmp/am_tx_sim.elf |
 *                     awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel /tmp/am_tx_sim.elf \
 *     -plugin file=work/dsp-probe/common/libinsnframe.so -d plugin
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/am_tx.h"

#ifdef MARK_FRAMES
__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void)
{
    __asm__ volatile("" ::: "memory");
}
#define MARK() frame_marker()
#else
#define MARK() \
    do {       \
    } while (0)
#endif

#define BLOCK 48 /* one millisecond of audio at 48 kHz */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The calibration the harness runs with: 1 W on the factory 1 W duty, which
 * is 33 % of a 1000 count PWM period in the default band calibration. */
static const uint16_t DUTY_CARRIER = 330;

/**
 * Synthetic voice: three tones with a syllable envelope, as in the receive
 * harness, so that both ends of the airband chain are measured on the same
 * signal.
 */
static double voice(double t, int n)
{
    double syllable = 0.55 + 0.45 * sin(2.0 * M_PI * 3.1 * t);
    double v = 0.55 * sin(2.0 * M_PI * 440.0 * t)
             + 0.30 * sin(2.0 * M_PI * 1130.0 * t)
             + 0.15 * sin(2.0 * M_PI * 1970.0 * t);

    (void)n;
    return v * syllable * syllable;
}

/**
 * Run the modulator for a number of blocks and return the duty values it
 * produced through `sink`, if any.
 */
static size_t run(struct amTx *t, const int16_t *in, size_t blocks,
                  uint16_t *sink, size_t sinkMax)
{
    uint16_t duty[BLOCK];
    size_t total = 0;

    for (size_t b = 0; b < blocks; b++) {
        size_t n = am_tx_process(t, in + b * BLOCK, BLOCK, duty);

        for (size_t i = 0; (i < n) && (total < sinkMax); i++)
            sink[total++] = duty[i];
    }

    return total;
}

/**
 * Fill a block of samples from a generator.
 */
static void fill(int16_t *dst, size_t blocks, double (*gen)(double, int),
                 double amplitude, double phase, double *tEnd)
{
    static int seed = 1;

    for (size_t i = 0; i < blocks * BLOCK; i++) {
        double t = phase + (double)i / (double)AMTX_RATE;
        double v = amplitude * gen(t, seed) * 32767.0;

        dst[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }

    *tEnd = phase + (double)(blocks * BLOCK) / (double)AMTX_RATE;
    seed++;
}

/** A 1 kHz tone, for the distortion and ramp measurements. */
static double tone1k(double t, int n)
{
    (void)n;
    return sin(2.0 * M_PI * 1000.0 * t);
}

/** A single tone at an arbitrary frequency, for the band sweep. */
static double sweepFreq;
static double sweepTone(double t, int n)
{
    (void)n;
    return sin(2.0 * M_PI * sweepFreq * t);
}

/**
 * Magnitude of one DFT bin of a real signal, by the Goertzel algorithm.
 */
static double goertzel(const double *x, size_t n, double freq)
{
    double w = 2.0 * M_PI * freq / (double)AMTX_DUTY_RATE;
    double coeff = 2.0 * cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;

    for (size_t i = 0; i < n; i++) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    double re = s1 - s2 * cos(w);
    double im = s2 * sin(w);

    return sqrt(re * re + im * im) * 2.0 / (double)n;
}

/**
 * Audio band: a tone at each frequency through the whole modulator, measured
 * as the amplitude of that frequency in the envelope it produces, relative to
 * the 1 kHz reading.
 */
static void measureBand(FILE *out, uint16_t dutyCarrier)
{
    static int16_t in[BLOCK * 40];
    static uint16_t duty[BLOCK * 40];
    const double freqs[] = { 100,  200,  300,  500,  1000, 1500, 2000,
                             2500, 2700, 3000, 3500, 4000, 6000 };
    static double mags[sizeof(freqs) / sizeof(freqs[0])];
    double ref = 1.0;

    fprintf(out, "band:\n");

    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        struct amTx t;
        double tEnd = 0.0;
        double env[BLOCK * 40 / AMTX_DECIM];
        size_t n;

        sweepFreq = freqs[i];
        fill(in, 40, sweepTone, 0.30, 0.0, &tEnd);

        am_tx_init(&t, NULL);
        t.cfg.dutyCarrier = dutyCarrier;
        t.cfg.dutyMin = dutyCarrier / 10;
        t.cfg.dutyMax = (uint16_t)((dutyCarrier * 19) / 10);
        t.cfg.slope = dutyCarrier;
        am_tx_reset(&t);
        am_tx_setDepth(&t, 50);
        am_tx_keyDown(&t);

        n = run(&t, in, 40, duty, sizeof(duty) / sizeof(duty[0]));

        /* Steady state only: the last 25 ms, once the ramps and the AGC are
         * settled, mean removed so that the carrier does not bias the bin. */
        size_t skip = n > 200 ? n - 200 : 0;
        double mean = 0.0;

        for (size_t k = skip; k < n; k++)
            mean += duty[k];
        mean /= (double)(n - skip);
        for (size_t k = skip; k < n; k++)
            env[k - skip] = (double)duty[k] - mean;

        mags[i] = goertzel(env, n - skip, freqs[i]);
        if (freqs[i] == 1000.0)
            ref = mags[i];
    }

    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++)
        fprintf(out, "  %5.0f Hz %7.2f dB\n", freqs[i],
                20.0 * log10(mags[i] / ref));
}

/**
 * Modulation depth: max and min of the duty once settled, for a voice-like
 * signal at three microphone levels.
 *
 * The depth the operator sets is the depth the modulator asks the PA for; what
 * a receiver measures is that number times the levelling error and divided by
 * the transfer of the PA, which is what the bench calibration of
 * docs/TX_AIRBAND.md is for. Here the PA is a wire, so what comes out is what
 * the modulator did.
 */
static void measureDepth(FILE *out, uint16_t dutyCarrier, uint8_t depth)
{
    static int16_t in[BLOCK * 2000];
    static uint16_t duty[BLOCK * 2000];
    const double levels[] = { -40.0, -20.0, -5.0 };

    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        struct amTx t;
        double tEnd = 0.0;
        double amp = pow(10.0, levels[i] / 20.0);

        fill(in, 2000, voice, amp, 0.0, &tEnd);
        am_tx_init(&t, NULL);
        t.cfg.dutyCarrier = dutyCarrier;
        t.cfg.dutyMin = dutyCarrier / 10;
        t.cfg.dutyMax = (uint16_t)((dutyCarrier * 19) / 10);
        t.cfg.slope = dutyCarrier;
        am_tx_reset(&t);
        am_tx_setDepth(&t, depth);
        am_tx_keyDown(&t);

        size_t n = run(&t, in, 2000, duty, sizeof(duty) / sizeof(duty[0]));
        size_t skip = n - 800; /* last 100 ms */
        uint16_t lo = 0xFFFF, hi = 0;
        double sum = 0.0;

        for (size_t k = skip; k < n; k++) {
            if (duty[k] < lo)
                lo = duty[k];
            if (duty[k] > hi)
                hi = duty[k];
            sum += duty[k];
        }

        double dc = sum / (double)(n - skip);
        double m = (hi - lo) / (2.0 * dc);

        fprintf(out,
                "depth %u%% at %+.0f dBFS: duty %u..%u dc %.0f "
                "-> depth %.0f%% (min %.2f of carrier)\n",
                depth, levels[i], lo, hi, dc, 100.0 * m, (double)lo / dc);
    }
}

/**
 * Envelope distortion of a 1 kHz tone: harmonics 2 to 5 against the
 * fundamental. A perfect modulator of a sine has nothing there; what puts
 * anything there is the limiter, the AGC moving within the tone period (it
 * should not: the detector is slow) and the quantisation of the duty.
 */
static void measureTone(FILE *out, uint16_t dutyCarrier, uint8_t depth)
{
    static int16_t in[BLOCK * 100];
    static uint16_t duty[BLOCK * 100];
    double env[BLOCK * 100 / AMTX_DECIM];
    struct amTx t;
    double tEnd = 0.0;

    fill(in, 100, tone1k, 0.30, 0.0, &tEnd);
    am_tx_init(&t, NULL);
    t.cfg.dutyCarrier = dutyCarrier;
    t.cfg.dutyMin = 1;
    t.cfg.dutyMax = 1000;
    t.cfg.slope = dutyCarrier;
    am_tx_reset(&t);
    am_tx_setDepth(&t, depth);
    am_tx_keyDown(&t);

    size_t n = run(&t, in, 100, duty, sizeof(duty) / sizeof(duty[0]));
    size_t skip = n - 400;
    double mean = 0.0;
    double h[6] = { 0 };

    for (size_t k = skip; k < n; k++)
        mean += duty[k];
    mean /= (double)(n - skip);
    for (size_t k = skip; k < n; k++)
        env[k - skip] = (double)duty[k] - mean;

    for (int i = 1; i <= 5; i++)
        h[i] = goertzel(env, n - skip, 1000.0 * (double)i);

    double thd = 0.0;
    for (int i = 2; i <= 5; i++)
        thd += h[i] * h[i];
    thd = sqrt(thd) / h[1];

    fprintf(out,
            "1 kHz at %u%%: fundamental %.1f counts, THD %.1f%% "
            "(-%.1f dB)\n",
            depth, h[1], 100.0 * thd, -20.0 * log10(thd));
}

/**
 * Ramps: how long the carrier takes to come up and the depth to reach the
 * setting, and the same on the way down, plus the smallest duty of the whole
 * transmission.
 */
static void measureRamps(FILE *out, uint16_t dutyCarrier, uint8_t depth)
{
    static int16_t in[BLOCK * 200];
    static uint16_t up[BLOCK * 200], down[BLOCK * 20];
    struct amTx t;
    double tEnd = 0.0;

    /* Full scale tone: the worst case for the bottom of the envelope */
    fill(in, 200, tone1k, 1.0, 0.0, &tEnd);
    am_tx_init(&t, NULL);
    t.cfg.dutyCarrier = dutyCarrier;
    t.cfg.dutyMin = dutyCarrier / 10;
    t.cfg.dutyMax = (uint16_t)((dutyCarrier * 19) / 10);
    t.cfg.slope = dutyCarrier;
    am_tx_reset(&t);
    am_tx_setDepth(&t, depth);
    am_tx_keyDown(&t);

    size_t n = run(&t, in, 200, up, sizeof(up) / sizeof(up[0]));

    /* First sample at 99 % of the carrier, and the first 20 ms window whose
     * peak to peak has reached 90 % of the depth the modulator settled on. */
    size_t iFull = n, iDepth = n;
    const size_t win = (20 * AMTX_DUTY_RATE) / 1000;

    for (size_t k = 0; k < n; k++) {
        if ((iFull == n) && (up[k] >= (uint16_t)((dutyCarrier * 99) / 100)))
            iFull = k;
        if ((iDepth == n) && (k >= win)) {
            uint16_t hi = 0, lo = 0xFFFF;
            for (size_t j = k - win; j < k; j++) {
                if (up[j] > hi)
                    hi = up[j];
                if (up[j] < lo)
                    lo = up[j];
            }
            uint16_t target =
                (uint16_t)(am_tx_getDepth(&t) * 2 * dutyCarrier / 100);
            if ((hi - lo) >= (uint16_t)(target * 9 / 10))
                iDepth = k;
        }
    }

    am_tx_keyUp(&t);
    size_t d = run(&t, in, 20, down, sizeof(down) / sizeof(down[0]));
    size_t iZero = d;
    for (size_t k = 0; k < d; k++) {
        if (down[k] == 0) {
            iZero = k;
            break;
        }
    }

    /*
     * Carrier floor once settled: the lowest duty of the last 100 ms, which
     * is the bottom of the modulation. A transmitter whose carrier reaches
     * zero overmodulates, and the receiver hears a rectified envelope instead
     * of the voice.
     */
    uint16_t floor = 0xFFFF;
    for (size_t k = (n > 800 ? n - 800 : 0); k < n; k++)
        if (up[k] < floor)
            floor = up[k];

    fprintf(out,
            "ramps: carrier up in %.1f ms, depth in %.1f ms, "
            "down in %.1f ms, settled floor %u counts (%.0f%% of "
            "carrier)\n",
            1000.0 * (double)iFull / AMTX_DUTY_RATE,
            1000.0 * (double)iDepth / AMTX_DUTY_RATE,
            1000.0 * (double)iZero / AMTX_DUTY_RATE, floor,
            100.0 * (double)floor / (double)dutyCarrier);
}

/**
 * Cost: one second of audio through the modulator, with a marker at each end
 * of the second so the QEMU plugin can count what happens in between.
 */
static void measureCost(uint16_t dutyCarrier)
{
    static int16_t in[BLOCK * 1000];
    static uint16_t duty[BLOCK * 1000];
    struct amTx t;
    double tEnd = 0.0;

    fill(in, 1000, voice, 0.3, 0.0, &tEnd);
    am_tx_init(&t, NULL);
    t.cfg.dutyCarrier = dutyCarrier;
    am_tx_reset(&t);
    am_tx_setDepth(&t, 60);
    am_tx_keyDown(&t);

    MARK();
    volatile size_t n = run(&t, in, 1000, duty, sizeof(duty) / sizeof(duty[0]));
    MARK();

    printf("cost: %u duty values for 1 s of audio (%u samples)\n", (unsigned)n,
           (unsigned)(BLOCK * 1000));
}

int main(int argc, char **argv)
{
#ifdef MARK_FRAMES
    /* In QEMU the only thing worth measuring is the cost of one second */
    measureCost(DUTY_CARRIER);
    return 0;
#endif
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char path[256];

    snprintf(path, sizeof(path), "%s/am_tx_measure.txt", dir);
    FILE *out = fopen(path, "w");
    if (out == NULL)
        out = stdout;

    fprintf(out, "AM transmit modulator, carrier %u counts of 1000\n\n",
            DUTY_CARRIER);

    measureBand(out, DUTY_CARRIER);

    const uint8_t depths[] = { 30, 60, 90 };
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
        fprintf(out, "\n");
        measureDepth(out, DUTY_CARRIER, depths[i]);
    }

    fprintf(out, "\n");
    measureTone(out, DUTY_CARRIER, 60);
    measureTone(out, DUTY_CARRIER, 90);
    measureRamps(out, DUTY_CARRIER, 60);
    measureRamps(out, DUTY_CARRIER, 90);

    printf("measurements written to %s\n", path);
    if (out != stdout)
        fclose(out);

    measureCost(DUTY_CARRIER);

    return 0;
}
