/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file encode_imbe.c
 * @brief Encode s16le 8 kHz speech into IMBE parameters with mbelib-neo.
 *
 * Host-only helper that produces the verification corpus: it runs the
 * reference's IMBE 7200x4400 encoder (GPL-2.0-or-later, TIA-102.BABA chapter 6)
 * over a raw PCM file and writes one 88-byte record per frame (one bit per
 * byte, b0 first), the format both decoders read.
 *
 *   encode_imbe speech8k.s16 frames.bin [max_frames]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mbelib-neo/mbelib.h"

#define PCM 160

int main(int argc, char **argv)
{
    FILE *fi, *fo;
    int16_t pcm[PCM];
    char bits[88];
    mbe_imbe4400_encoder *enc;
    mbe_parms cur, prev;
    long n = 0, max = -1;

    if (argc < 3) {
        fprintf(stderr, "usage: %s speech.s16 frames.bin [max_frames]\n", argv[0]);
        return 2;
    }
    if (argc > 3)
        max = strtol(argv[3], NULL, 10);

    fi = fopen(argv[1], "rb");
    if (fi == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    fo = fopen(argv[2], "wb");
    if (fo == NULL) {
        fprintf(stderr, "cannot create %s\n", argv[2]);
        fclose(fi);
        return 1;
    }

    enc = mbe_imbe4400EncoderAlloc();
    if (enc == NULL) {
        fprintf(stderr, "encoder alloc failed\n");
        return 1;
    }
    mbe_imbe4400EncoderReset(enc);
    mbe_initMbeParms(&cur, &prev, &cur);

    while ((max < 0 || n < max) && fread(pcm, sizeof pcm[0], PCM, fi) == PCM) {
        if (mbe_encodeImbe4400ParmsShort(enc, pcm, bits, &cur, &prev) < 0) {
            fprintf(stderr, "encode failed at frame %ld\n", n);
            break;
        }
        for (int i = 0; i < 88; i++)
            putc(bits[i] ? 1 : 0, fo);
        mbe_moveMbeParms(&cur, &prev);
        n++;
    }

    fclose(fi);
    fclose(fo);
    mbe_imbe4400EncoderFree(enc);
    printf("encode_imbe: %ld frames\n", n);
    return (n > 0) ? 0 : 1;
}
