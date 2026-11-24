/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file pcm_dump.c
 * @brief Decode a file of D-STAR voice frames and dump the PCM, host or bare
 * metal.
 *
 * Reads ITERS frames of 9 bytes (the 72 voice bits of a DV frame, air order)
 * from "dstar_frames.air" (argv[1] on a host), decodes them through the
 * firmware's entry point (dstarVocoder_decode(), i.e. FEC + decoder + PCM),
 * and writes the 160-sample little-endian s16 stream to "dstar_out.pcm"
 * (argv[2] on a host).  Used to check that the same code produces the same
 * samples on the target as on the host, and, with a QEMU instruction-counting
 * plugin, to measure the cost per frame.
 *
 * Accumulating the PCM into `sink` at the end keeps the decode loop from being
 * optimised away; the checksum is printed so a run can be compared.
 *
 * Build:
 *   host:        cc -O2 -DDSTAR_TEST_HOST   (see tests/README.md)
 *   Cortex-M33:  arm-none-eabi-gcc -mcpu=cortex-m33 ... -specs=rdimon.specs
 *                with the QEMU mps2-an505 harness in tests/qemu.
 *
 * Compile-time options:
 *   ITERS   number of frames to decode (default 100)
 *   WARM    frames decoded before the measured loop, output dropped (0)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dstar_vocoder.h"

#ifndef ITERS
#define ITERS 100
#endif
#ifndef WARM
#define WARM 0
#endif

/* The semihosting C library (arm-none-eabi -specs=rdimon.specs) needs its
 * monitor handles initialised before stdio or file I/O work. */
#if defined(__arm__) && !defined(DSTAR_TEST_HOST)
extern void initialise_monitor_handles(void);
#define TEST_STDIO_INIT() initialise_monitor_handles()
#else
#define TEST_STDIO_INIT() do { } while (0)
#endif

static int16_t pcm[DSTAR_VOCODER_PCM_SAMPLES];
static uint8_t frame[DSTAR_VOCODER_FRAME_BYTES];
/* ITERS=0 measures only the start-up cost: the array must still be valid. */
static int16_t out[ITERS > 0 ? ITERS : 1][DSTAR_VOCODER_PCM_SAMPLES];

static volatile int32_t sink;

int main(int argc, char **argv)
{
    const char *in = (argc > 1) ? argv[1] : "dstar_frames.air";
    const char *outp = (argc > 2) ? argv[2] : "dstar_out.pcm";
    FILE *fi;
    FILE *fo;
    size_t n = 0;

    TEST_STDIO_INIT();
    printf("dstar_vocoder pcm dump: iters=%d warm=%d state=%u B\n", ITERS, WARM,
           (unsigned)dstarVocoder_state_bytes());

    fi = fopen(in, "rb");
    if (fi == NULL) {
        printf("cannot open %s\n", in);
        return 1;
    }

    dstarVocoder_reset();
    while (n < (size_t)ITERS &&
           fread(frame, 1, sizeof frame, fi) == sizeof frame) {
        if (dstarVocoder_decode(frame, pcm) != 0)
            break;
        if (n >= (size_t)WARM)
            memcpy(out[n - WARM], pcm, sizeof pcm);
        sink += pcm[n % DSTAR_VOCODER_PCM_SAMPLES];
        n++;
    }
    fclose(fi);

    size_t dumped = (n > (size_t)WARM) ? n - (size_t)WARM : 0;
    fo = fopen(outp, "wb");
    if (fo == NULL) {
        printf("cannot create %s\n", outp);
        return 1;
    }
    size_t written = fwrite(out, sizeof out[0], dumped, fo);
    fclose(fo);

    {
        uint16_t frames = 0, bad = 0, eras = 0, tones = 0, muted = 0;
        dstarVocoder_stats(&frames, &bad, &eras, &tones, &muted);
        printf("read=%u dumped=%u written=%u bad=%u erasures=%u tones=%u "
               "muted=%u sink=%ld\n",
               (unsigned)n, (unsigned)dumped, (unsigned)written,
               (unsigned)bad, (unsigned)eras, (unsigned)tones,
               (unsigned)muted, (long)sink);
    }
    return (dumped > 0) ? 0 : 1;
}
