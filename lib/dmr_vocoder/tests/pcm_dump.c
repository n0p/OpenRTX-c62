/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file pcm_dump.c
 * @brief Decode a file of AMBE+2 frames and dump the PCM, host or bare metal.
 *
 * Reads ITERS frames of 9 bytes from "dmr_frames.ambe" (or argv[1] on a host),
 * decodes them, and writes the 160-sample big-endian-agnostic little-endian
 * s16 PCM stream to "dmr_out.pcm" (argv[2] on a host).  Used to check that the
 * same code produces the same samples on the target as on the host, and, with
 * a QEMU instruction-counting plugin, to measure the cost per frame.
 *
 * Build:
 *   host:        cc -O2 -DDMR_TEST_HOST   (see tests/README.md)
 *   Cortex-M33:  arm-none-eabi-gcc -mcpu=cortex-m33 ... -specs=rdimon.specs
 *                with the QEMU mps2-an505 harness in the C62 work tree.
 *
 * Compile-time options:
 *   ITERS   number of frames to decode (default 100)
 *   WARM    number of frames decoded before the measured loop, not dumped
 *           (default 0); set to 1 to build the FFT plan/DCT tables before the
 *           measurement, or use it to compare init cost separately.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmr_vocoder.h"

#ifndef ITERS
#define ITERS 100
#endif
#ifndef WARM
#define WARM 0
#endif

/*
 * The semihosting C library (arm-none-eabi -specs=rdimon.specs) needs its
 * monitor handles initialised before stdio or file I/O work.
 */
#if defined(__arm__) && !defined(DMR_TEST_HOST)
extern void initialise_monitor_handles(void);
#define TEST_STDIO_INIT() initialise_monitor_handles()
#else
#define TEST_STDIO_INIT() do { } while (0)
#endif

static int16_t pcm[DMR_VOCODER_PCM_SAMPLES];
static uint8_t frame[DMR_VOCODER_FRAME_BYTES];
/* ITERS=0 measures only the start-up cost: the array must still be valid. */
static int16_t out[ITERS > 0 ? ITERS : 1][DMR_VOCODER_PCM_SAMPLES];

/* Keeps the compiler from dropping the decode loop. */
static volatile int32_t sink;

static size_t read_frames(FILE *f)
{
    size_t n = 0;

    while (n < (size_t)ITERS && fread(frame, 1, sizeof frame, f) == sizeof frame) {
        if (n < (size_t)WARM) { /* warm-up: same state machine, output dropped */
            dmr_vocoder_decode(frame, pcm);
        } else {
            dmr_vocoder_decode(frame, pcm);
            memcpy(out[n - WARM], pcm, sizeof pcm);
        }
        sink += pcm[n % DMR_VOCODER_PCM_SAMPLES];
        n++;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *in = (argc > 1) ? argv[1] : "dmr_frames.ambe";
    const char *outp = (argc > 2) ? argv[2] : "dmr_out.pcm";
    FILE *fi;
    FILE *fo;

    TEST_STDIO_INIT();
    printf("dmr_vocoder pcm dump: iters=%d warm=%d state=%u B\n", ITERS, WARM,
           (unsigned)dmr_vocoder_state_bytes());

    fi = fopen(in, "rb");
    if (fi == NULL) {
        printf("cannot open %s\n", in);
        return 1;
    }

    dmr_vocoder_reset();
    size_t n = read_frames(fi);
    fclose(fi);

    size_t dumped = (n > (size_t)WARM) ? n - (size_t)WARM : 0;
    fo = fopen(outp, "wb");
    if (fo == NULL) {
        printf("cannot create %s\n", outp);
        return 1;
    }
    size_t written = fwrite(out, sizeof out[0], dumped, fo);
    fclose(fo);

    printf("frames read=%u dumped=%u written=%u arena=%u B last_errors=%u sink=%ld\n",
           (unsigned)n, (unsigned)dumped, (unsigned)written,
           (unsigned)dmr_vocoder_arena_used(), dmr_vocoder_last_errors(),
           (long)sink);
    return (dumped == (size_t)ITERS) ? 0 : 1;
}
