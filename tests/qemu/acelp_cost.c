/* SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * TETRA ACELP (OpenACELP) decoder cost harness: one 137-bit frame -> 240
 * samples of 8 kHz PCM, repeated, on an emulated Cortex-M33 (QEMU
 * mps2-an505) with the instruction counter of scripts/codec_cost.sh.
 *
 *   -DFRAMES=n  frames inside the measured loop (1 and 21 give the slope)
 *   -DBFI=1     every frame marked lost, to cost the concealment path
 *
 * The payload is a pseudo-random bit pattern with bfi=0: the decoder's work
 * per frame does not depend on the bits (it is codebook lookups plus a fixed
 * synthesis), and this tree has no libtetra capture wired into the harness.
 * The hash lets the script check that host and target decode the same PCM.
 */
#include <stdint.h>

#include <openacelp.h>
#include "harness_out.h"

#ifndef FRAMES
#define FRAMES 20
#endif
#ifndef WARMUP
#define WARMUP 4
#endif
#ifndef BFI
#define BFI 0
#endif

#define NFRAMES 32

static uint8_t frames[NFRAMES][OA_FRAME_BITS];
static int16_t pcm[OA_FRAME_SAMPLES];
static oa_dec_t dec;
static volatile uint32_t sink;

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

int main(void)
{
    uint32_t s = 0x12345678u;
    for (int i = 0; i < NFRAMES; i++)
        for (int b = 0; b < OA_FRAME_BITS; b++) {
            s = s * 1664525u + 1013904223u;
            frames[i][b] = (s >> 13) & 1u;
        }

    oa_dec_init(&dec, &oa_tables_tetra);

    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < WARMUP + FRAMES; i++) {
        oa_dec_frame(&dec, frames[i % NFRAMES], BFI, pcm);
        if (i + 1 == WARMUP + FRAMES)
            hash = fnv1a(hash, (const uint8_t *)pcm, sizeof(pcm));
        sink += (uint16_t)pcm[0];
    }

    char line[96], *p = line;
    p = hout_lit(p, "ACELP frames=");
    p = hout_dec(p, FRAMES);
    p = hout_lit(p, " bfi=");
    p = hout_dec(p, BFI);
    p = hout_lit(p, " bits=");
    p = hout_dec(p, OA_FRAME_BITS);
    p = hout_lit(p, " samples=");
    p = hout_dec(p, OA_FRAME_SAMPLES);
    p = hout_lit(p, " hash=");
    p = hout_hex32(p, hash);
    p = hout_lit(p, "\n");
    *p = '\0';
    hout_str(line);
    return (int)sink & 0;
}
