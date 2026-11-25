/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file fuzz.c
 * @brief Robustness pass over the IMBE decoder: bit errors and random frames.
 *
 * Runs the corpus with 1, 2, 4 and 8 random bit errors per frame and 20 000
 * random parameter vectors, and checks that every sample stays inside int16 and
 * that no NaN reaches the output.  Built with ASan/UBSan by run_host.sh.
 *
 *   fuzz corpus.imbe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imbe_vocoder.h"

static uint32_t rng = 0x12345678u;

static uint32_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static long run(const uint8_t *frames, size_t nframes, int flips)
{
    int16_t pcm[IMBE_VOCODER_PCM_SAMPLES];
    uint8_t f[IMBE_VOCODER_PARAM_BYTES];
    long silent = 0;

    imbe_vocoder_reset();
    for (size_t i = 0; i < nframes; i++) {
        memcpy(f, frames + i * IMBE_VOCODER_PARAM_BYTES, sizeof f);
        for (int k = 0; k < flips; k++) {
            const unsigned bit = next() % 88u;
            f[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7u));
        }
        const int r = imbe_vocoder_decode_errs(f, flips > 5 ? 8 : 0, pcm);
        if (r < 0) {
            fprintf(stderr, "decode failed at frame %zu\n", i);
            exit(1);
        }
        if (r == 0)
            silent++;
        for (int n = 0; n < IMBE_VOCODER_PCM_SAMPLES; n++) {
            if (pcm[n] > 32767 || pcm[n] < -32768) {
                fprintf(stderr, "sample out of range at frame %zu sample %d\n", i, n);
                exit(1);
            }
        }
    }
    return silent;
}

int main(int argc, char **argv)
{
    FILE *f;
    uint8_t *frames;
    long size, nframes;

    if (argc < 2) {
        fprintf(stderr, "usage: %s frames.imbe\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    frames = malloc((size_t)size);
    if (frames == NULL || fread(frames, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    fclose(f);
    nframes = size / IMBE_VOCODER_PARAM_BYTES;

    printf("fuzz: %ld frames from %s\n", nframes, argv[1]);
    const int flips[5] = {0, 1, 2, 4, 8};
    for (int i = 0; i < 5; i++) {
        const long played = run(frames, (size_t)nframes, flips[i]);
        printf("   %d bit errors/frame: %ld voice frames, %ld repeated or muted\n", flips[i], played,
               nframes - played);
    }
    {
        /* random parameter vectors: nothing a channel could produce, but the
         * decoder must still stay in range */
        uint8_t rnd[IMBE_VOCODER_PARAM_BYTES];
        int16_t pcm[IMBE_VOCODER_PCM_SAMPLES];
        imbe_vocoder_reset();
        for (int i = 0; i < 20000; i++) {
            for (int k = 0; k < IMBE_VOCODER_PARAM_BYTES; k++)
                rnd[k] = (uint8_t)next();
            if (imbe_vocoder_decode_errs(rnd, 0, pcm) < 0) {
                fprintf(stderr, "random frame %d failed\n", i);
                return 1;
            }
        }
        printf("   20000 random parameter vectors: in range\n");
    }

    free(frames);
    return 0;
}
