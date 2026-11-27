/* SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * 8 kHz <-> 48 kHz polyphase resampler cost harness, Cortex-M33 under QEMU.
 * This is the cost of replacing the sample selection and repetition that
 * platform/drivers/audio/csk6_codec.c does today (divisor 6, no filtering)
 * with a 48-tap low-pass split in six phases: 8 taps per phase, Q15
 * coefficients, 32-bit accumulator.
 *
 *   -DONLY_DEC  48 kHz -> 8 kHz decimator only (480 in, 80 out per call)
 *   -DONLY_INT  8 kHz -> 48 kHz interpolator only (80 in, 480 out per call)
 *   -DFRAMES=n  calls of each inside the measured loop
 *
 * One call is one 10 ms block.  Coefficients are a windowed sinc generated
 * for this harness; they are not the ones a port would ship (those want a
 * design with a stated passband and stopband), so treat the cost, not the
 * filter, as the result.
 */
#include <stdint.h>

#include "harness_out.h"

#ifndef FRAMES
#define FRAMES 200
#endif

#define PHASES 6
#define TAPS   8
#define BLOCK_IN_DEC  480
#define BLOCK_OUT_DEC 80
#define BLOCK_IN_INT  80
#define BLOCK_OUT_INT 480

static const int16_t coef[PHASES][TAPS] = {
    {  -44,  102, -205,  392, -748, 1563, -4183, 13857 },
    {  -68,  153, -301,  561, -1046, 2171, -6298, 26604 },
    {  -93,  206, -397,  722, -1300, 2622, -8762, 29948 },
    { -108,  239, -451,  800, -1391, 2681, -8762, 29948 },
    { -104,  229, -424,  735, -1245, 2349, -6298, 26604 },
    {  -81,  181, -350,  641, -1192, 2544, -4183, 13857 },
};

static int16_t inBuf[BLOCK_IN_DEC];
static int16_t outBuf[BLOCK_OUT_INT];
static volatile uint32_t sink;

static int16_t sat16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

/* 48 kHz -> 8 kHz: one output every six input samples, 6 x 8 tap phases. */
static void decBlock(const int16_t *in, int16_t *out, unsigned n)
{
    unsigned o = 0;
    for (unsigned i = 0; i + PHASES * TAPS <= n; i += PHASES) {
        int32_t acc = 0;
        for (unsigned p = 0; p < PHASES; p++) {
            const int16_t *c = coef[p];
            const int16_t *x = &in[i + p];
            for (unsigned k = 0; k < TAPS; k++)
                acc += (int32_t)c[k] * x[k * PHASES];
        }
        out[o++] = sat16(acc >> 15);
    }
}

/* 8 kHz -> 48 kHz: six output samples per input sample, same phases. */
static void intBlock(const int16_t *in, int16_t *out, unsigned n)
{
    unsigned o = 0;
    for (unsigned i = 0; i < n; i++) {
        for (unsigned p = 0; p < PHASES; p++) {
            const int16_t *c = coef[p];
            int32_t acc = 0;
            for (unsigned k = 0; k < TAPS; k++) {
                int32_t idx = (int32_t)i - (int32_t)k;
                int32_t x = idx >= 0 ? in[idx] : 0;
                acc += (int32_t)c[k] * x;
            }
            out[o++] = sat16(acc >> 15);
        }
    }
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

int main(void)
{
    for (int i = 0; i < BLOCK_IN_DEC; i++)
        inBuf[i] = (int16_t)((i * 977) & 0x3fff) - 8192;

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
}
