/* SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * 8 kHz <-> 48 kHz polyphase resampler cost and response harness, Cortex-M33
 * under QEMU.
 *
 * It runs the shipping kernels, not a copy of them: the filter, the history
 * management and the arithmetic are whatever platform/drivers/audio/
 * csk6_resample.h contains, which is also what csk6_codec.c compiles.
 *
 *   -DONLY_DEC  48 kHz -> 8 kHz decimator only (480 in, 80 out per call)
 *   -DONLY_INT  8 kHz -> 48 kHz interpolator only (80 in, 480 out per call)
 *   -DTONE      alias and image measurement instead of the cost loop
 *   -DFRAMES=n  calls of each inside the measured loop
 *
 * One call is one 10 ms block.  scripts/codec_cost.sh [O2|Os] resample builds
 * and runs this on the host and under QEMU, compares the two by hash and prints
 * the instruction slope; `... resample-tone` runs the TONE build and prints the
 * attenuation, which is the criterion of reports/codecs.md section 3.1.
 */
#include <math.h>
#include <stdint.h>

#include "csk6_resample.h"
#include "harness_out.h"

#ifndef FRAMES
#define FRAMES 200
#endif

#define BLOCK_IN_DEC  480 /* 10 ms at 48 kHz */
#define BLOCK_OUT_DEC 80  /* 10 ms at 8 kHz */
#define BLOCK_IN_INT  BLOCK_OUT_DEC
#define BLOCK_OUT_INT BLOCK_IN_DEC

static struct csk6Decimator dec;
static struct csk6Interpolator itp;
/* Twelve blocks: the TONE build measures the last ten */
static int16_t inBuf[12 * BLOCK_IN_DEC];
static int16_t outBuf[BLOCK_OUT_INT];
static volatile uint32_t sink;

/* 48 kHz -> 8 kHz, as csk6_codec.c feeds it: one input sample per call. */
static size_t decBlock(const int16_t *in, int16_t *out, size_t n)
{
    size_t o = 0;

    for (size_t i = 0; i < n; i++) {
        int16_t y;
        if (csk6Decim_push(&dec, in[i], &y))
            out[o++] = y;
    }
    return o;
}

/* 8 kHz -> 48 kHz, as csk6_codec.c plays it: one input, six outputs. */
static size_t intBlock(const int16_t *in, int16_t *out, size_t n)
{
    size_t o = 0;

    for (size_t i = 0; i < n; i++) {
        csk6Interp_push(&itp, in[i]);
        for (unsigned int p = 0; p < CSK6_RESAMPLE_PHASES; p++)
            out[o++] = csk6Interp_out(&itp, p);
    }
    return o;
}

static uint32_t fnv1a(uint32_t h, const void *p, unsigned n)
{
    const uint8_t *b = p;
    for (unsigned i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

#ifdef TONE
/**
 * Single-bin DFT level of frequency f over n samples, in dBFS: 0 dB is a
 * full-scale sine.
 */
static int level_db10(const int16_t *x, unsigned n, float f, float fs)
{
    float re = 0.0f, im = 0.0f;

    for (unsigned i = 0; i < n; i++) {
        float a = 2.0f * (float)M_PI * f * (float)i / fs;
        re += (float)x[i] * cosf(a);
        im += (float)x[i] * sinf(a);
    }

    float mag = 2.0f * sqrtf(re * re + im * im) / (float)n;
    if (mag < 1.0e-6f)
        mag = 1.0e-6f;
    return (int)(200.0f * log10f(mag / 32768.0f));
}

static void toneReport(void)
{
    /*
     * Twelve 10 ms blocks are generated and the last ten are measured, so the
     * filter starts from silence outside the DFT window.  The window then holds
     * an integer number of cycles of both the probe and the tone it leaks from
     * (1 500 Hz in 800 samples at 8 kHz, 7 and 9 kHz in 4 800 at 48 kHz): a
     * rectangular DFT with a tone off its bins leaks about -35 dB five bins
     * away, which would look like an image.
     */
    enum { BLOCKS = 12 };

    /* A 6,5 kHz tone at 48 kHz folds to 1,5 kHz when decimated to 8 kHz. */
    for (unsigned i = 0; i < 12 * BLOCK_IN_DEC; i++)
        inBuf[i] = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * 6500.0f
                                            * (float)i / 48000.0f));

    csk6Decim_reset(&dec);
    size_t n = 0;
    int16_t decOut[BLOCKS * BLOCK_OUT_DEC];
    for (unsigned b = 0; b < BLOCKS; b++)
        n += decBlock(&inBuf[b * BLOCK_IN_DEC], &decOut[n], BLOCK_IN_DEC);

    int alias = level_db10(&decOut[2 * BLOCK_OUT_DEC], n - 2 * BLOCK_OUT_DEC,
                           1500.0f, 8000.0f);

    /* A 1 kHz tone at 8 kHz puts images at 7 kHz and 9 kHz at 48 kHz. */
    for (unsigned i = 0; i < 12 * BLOCK_IN_INT; i++)
        inBuf[i] = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * 1000.0f
                                            * (float)i / 8000.0f));

    csk6Interp_reset(&itp);
    size_t m = 0;
    int16_t intOut[BLOCKS * BLOCK_OUT_INT];
    for (unsigned b = 0; b < BLOCKS; b++)
        m += intBlock(&inBuf[b * BLOCK_IN_INT], &intOut[m], BLOCK_IN_INT);

    int image7 = level_db10(&intOut[2 * BLOCK_OUT_INT], m - 2 * BLOCK_OUT_INT,
                            7000.0f, 48000.0f);
    int image9 = level_db10(&intOut[2 * BLOCK_OUT_INT], m - 2 * BLOCK_OUT_INT,
                            9000.0f, 48000.0f);
    int tone1 = level_db10(&intOut[2 * BLOCK_OUT_INT], m - 2 * BLOCK_OUT_INT,
                           1000.0f, 48000.0f);

    char line[80], *p = line;
    p = hout_lit(p, "RESAMPLE-TONE alias_db10=");
    p = hout_i64(p, alias);
    p = hout_lit(p, " image7_db10=");
    p = hout_i64(p, image7);
    p = hout_lit(p, " image9_db10=");
    p = hout_i64(p, image9);
    p = hout_lit(p, " tone_db10=");
    p = hout_i64(p, tone1);
    p = hout_lit(p, "\n");
    *p = '\0';
    hout_str(line);
}
#endif /* TONE */

int main(void)
{
#ifdef TONE
    toneReport();
    return 0;
#else
    for (int i = 0; i < BLOCK_IN_DEC; i++)
        inBuf[i] = (int16_t)((i * 977) & 0x3fff) - 8192;

    csk6Decim_reset(&dec);
    csk6Interp_reset(&itp);

    uint32_t hash = 2166136261u;
    for (unsigned r = 0; r < FRAMES; r++) {
#if defined(ONLY_DEC)
        decBlock(inBuf, outBuf, BLOCK_IN_DEC);
        sink += (uint16_t)outBuf[0];
#elif defined(ONLY_INT)
        intBlock(inBuf, outBuf, BLOCK_IN_INT);
        sink += (uint16_t)outBuf[0];
#else
        decBlock(inBuf, outBuf, BLOCK_IN_DEC);
        intBlock(outBuf, inBuf, BLOCK_IN_INT);
        sink += (uint16_t)inBuf[0];
#endif
    }
#if defined(ONLY_DEC)
    hash = fnv1a(hash, outBuf, BLOCK_OUT_DEC * sizeof(int16_t));
#elif defined(ONLY_INT)
    hash = fnv1a(hash, outBuf, BLOCK_OUT_INT * sizeof(int16_t));
#else
    hash = fnv1a(hash, inBuf, BLOCK_OUT_INT * sizeof(int16_t));
#endif

    char line[64], *p = line;
    p = hout_lit(p, "RESAMPLE frames=");
    p = hout_dec(p, FRAMES);
    p = hout_lit(p, " hash=");
    p = hout_hex32(p, hash);
    p = hout_lit(p, "\n");
    *p = '\0';
    hout_str(line);
    return (int)sink & 0;
#endif
}
