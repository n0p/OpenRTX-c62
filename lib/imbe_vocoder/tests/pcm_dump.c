/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file pcm_dump.c
 * @brief Decode a file of IMBE frames and dump the PCM, host or bare metal.
 *
 * Reads ITERS frames of the 11 packed parameter bytes the P25 LDU carries
 * (88 bits, b0 first) from "imbe_frames.imbe" (argv[1] on a host), decodes them with lib/imbe_vocoder and
 * writes the little-endian s16 PCM to "imbe_out.pcm" (argv[2] on a host).  The
 * same source runs on the host and on an emulated Cortex-M33, which is how the
 * port is proved identical on both and how QEMU's instruction counter measures
 * the cost per frame.
 *
 * Compile-time options:
 *   ITERS  frames to decode (default 100)
 *   WARM   frames decoded before the measured loop, not dumped (default 0)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imbe_vocoder.h"

#ifndef ITERS
#define ITERS 100
#endif
#ifndef WARM
#define WARM 0
#endif

#if defined(__arm__) && !defined(IMBE_TEST_HOST)
extern void initialise_monitor_handles(void);
#define TEST_STDIO_INIT() initialise_monitor_handles()
#else
#define TEST_STDIO_INIT() do { } while (0)
#endif

static int16_t pcm[IMBE_VOCODER_PCM_SAMPLES];
static uint8_t params[IMBE_VOCODER_PARAM_BYTES];
static int16_t out[ITERS > 0 ? ITERS : 1][IMBE_VOCODER_PCM_SAMPLES];

static volatile int32_t sink;

int main(int argc, char **argv)
{
    const char *in = (argc > 1) ? argv[1] : "imbe_frames.imbe";
    const char *outp = (argc > 2) ? argv[2] : "imbe_out.pcm";
    FILE *fi, *fo;
    size_t n = 0, dumped = 0;

    TEST_STDIO_INIT();
    printf("imbe_vocoder pcm dump: iters=%d warm=%d state=%u B\n", ITERS, WARM,
           (unsigned)imbe_vocoder_state_bytes());

    fi = fopen(in, "rb");
    if (fi == NULL) {
        printf("cannot open %s\n", in);
        return 1;
    }

    imbe_vocoder_reset();
    while (n < (size_t)ITERS && fread(params, 1, sizeof params, fi) == sizeof params) {
        imbe_vocoder_decode(params, pcm);
        if (n >= (size_t)WARM && dumped < (size_t)ITERS) {
            memcpy(out[dumped], pcm, sizeof pcm);
            dumped++;
        }
        sink += pcm[n % IMBE_VOCODER_PCM_SAMPLES];
        n++;
    }
    fclose(fi);

    fo = fopen(outp, "wb");
    if (fo == NULL) {
        printf("cannot create %s\n", outp);
        return 1;
    }
    size_t written = fwrite(out, sizeof out[0], dumped, fo);
    fclose(fo);

    printf("frames read=%u dumped=%u written=%u errors=%u sink=%ld\n", (unsigned)n, (unsigned)dumped,
           (unsigned)written, imbe_vocoder_last_errors(), (long)sink);
    return (dumped > 0) ? 0 : 1;
}
