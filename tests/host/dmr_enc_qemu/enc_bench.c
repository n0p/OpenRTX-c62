/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost bench for the DMR transmitter's AMBE+2 encoder (OpenDMR, the library
 * the C62 TX build links), bare metal on QEMU's mps2-an505 (Cortex-M33, as the
 * CSK6011B).  Instructions are counted by a TCG plugin, so what comes out is
 * "instructions per 20 ms frame" and, at the radio's clock, the share of the
 * core the encoder takes while the call is up.
 *
 * Built and run by run.sh; this file is only the bench body:
 *
 *   pcm8k.raw   raw s16 little-endian, 8 kHz mono, the speech to encode; the
 *               bench loops it when ITERS is larger than the file, which is
 *               what the encoder does on a long transmission (its state
 *               carries across frames).
 *   enc.ambe    ITERS AMBE+2 frames, 9 bytes each, for a listening or
 *               round-trip check.
 *
 * The ELF is built three times, with ITERS = 0, 1 and N: the 0-frame run
 * measures start-up, the 1-frame one adds the encoder's creation and its first
 * (cold) frame, and the difference between the N-frame and the 1-frame runs
 * divided by N-1 is the steady-state cost of one frame.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "opendmr.h"

#ifndef ITERS
#define ITERS 100
#endif

extern void initialise_monitor_handles(void);

#define FRAME_SAMPLES OPENDMR_PCM_SAMPLES /* 160, 20 ms at 8 kHz */
#define MAX_SAMPLES (30 * 8000)           /* 30 s of speech is plenty */

static int16_t pcm[MAX_SAMPLES];
static size_t nSamples;
static uint8_t ambe[OPENDMR_AMBE_FRAME_BYTES];

static void fail(const char *what)
{
    fprintf(stderr, "bench: %s\n", what);
    /* The semihosting exit in startup.c turns any return into SYS_EXIT */
}

int main(void)
{
    initialise_monitor_handles();

    FILE *f = fopen("pcm8k.raw", "rb");
    if (f == NULL) {
        fail("no pcm8k.raw");
        return 1;
    }
    nSamples = fread(pcm, sizeof(int16_t), MAX_SAMPLES, f);
    fclose(f);
    if (nSamples < FRAME_SAMPLES) {
        fail("pcm8k.raw shorter than one frame");
        return 1;
    }
    printf("bench: %u samples, %.2f s, %d iterations\n", (unsigned)nSamples,
           (double)nSamples / 8000.0, (int)ITERS);

    FILE *o = fopen("enc.ambe", "wb");

    opendmr_encoder_t *enc = opendmr_encoder_create();
    if (enc == NULL) {
        fail("opendmr_encoder_create");
        return 1;
    }

    size_t off = 0;
    unsigned bytes = 0;

    for (int i = 0; i < ITERS; i++) {
        if (off + FRAME_SAMPLES > nSamples)
            off = 0;
        if (!opendmr_encode(enc, pcm + off, ambe)) {
            fail("opendmr_encode");
            return 1;
        }
        off += FRAME_SAMPLES;
        for (int k = 0; k < OPENDMR_AMBE_FRAME_BYTES; k++)
            bytes += ambe[k];
        if (o != NULL)
            fwrite(ambe, 1, OPENDMR_AMBE_FRAME_BYTES, o);
    }
    if (o != NULL)
        fclose(o);

    /* A frame of silence must not be all zeros, and neither must real speech:
     * the sum is a cheap "it really encoded something" check */
    printf("bench: %d frames, frame checksum %u, last %02x %02x %02x %02x\n",
           (int)ITERS, bytes, ambe[0], ambe[1], ambe[2], ambe[3]);
    return 0;
}
