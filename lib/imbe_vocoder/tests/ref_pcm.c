/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file ref_pcm.c
 * @brief Reference PCM from mbelib-neo for the same IMBE frames (host only).
 *
 * Reads a file of 11-byte records, the packed parameter bytes of the air
 * interface (88 bits, b0 first), unpacks them one bit per byte as mbelib-neo
 * wants them, and writes the s16le PCM one 160-sample frame per record.
 *
 *   ref_pcm spec|dvsi frames.bin out.pcm
 *
 *   spec  mbelib-neo's IMBE path with its TIA-102.BABA 7.5 spec rendering
 *         (MBE_VOICED_SYNTHESIS_SPEC): the same pipeline the firmware's decoder
 *         implements, so this is the reference for bit-exactness.
 *   dvsi  mbe_processImbe4400Data(), i.e. mbelib-neo's default P25 path with
 *         DVSI's 10 ms subframe rendering: what a listener of that library
 *         hears, and the yardstick for how far the two renderings are apart.
 *
 * Built against the mbelib-neo tree (see tests/run_host.sh), which exports the
 * internal entry points this needs; the repeat logic is copied from its
 * imbe7200x4400.c because it lives in a static function there.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbe_adaptive.h"
#include "mbe_repeat.h"
#include "mbelib-neo/mbelib.h"

#define FRAME_BITS 88
#define PCM 160

static int16_t pcm[PCM];

static void decode_spec(const char d[FRAME_BITS], mbe_parms *cur, mbe_parms *prev, mbe_parms *enh, int errs)
{
    const int total = errs;
    float fbuf[PCM];
    float rm0;
    int bad, repeat;

    cur->errorCount4 = 0;
    cur->mutingThreshold = MBE_MUTING_THRESHOLD_IMBE;
    cur->errorCountTotal = total;
    cur->errorRate = (0.95f * prev->errorRate) + (0.000365f * (float)total);

    bad = mbe_decodeImbe4400Parms(d, cur, prev);

    /* imbe4400_repeat_required() on the parameter-bits path */
    repeat = (bad == 1) || (total > 5);
    if (repeat) {
        if (prev->repeatCount > (MBE_MAX_FRAME_REPEATS - 1)) {
            mbe_resetImbeModel(cur);
        } else {
            mbe_repeat_load_model(cur, prev);
            cur->repeatCount = mbe_repeat_next_count(prev->repeatCount);
        }
    } else {
        cur->repeatCount = 0;
    }

    mbe_moveMbeParms(cur, prev);
    rm0 = mbe_spectralAmpEnhanceWithRm0(cur, MBE_HIGH_BAND_GAIN_IMBE);
    mbe_synthesizeSpeechWithPreEnhRm0f(fbuf, cur, enh, rm0, MBE_MUTE_NOISE_SPEC, MBE_VOICED_SYNTHESIS_SPEC,
                                       MBE_AMPLITUDE_LIMIT_NONE);
    mbe_moveMbeParms(cur, enh);
    mbe_floattoshort(fbuf, pcm);
}

int main(int argc, char **argv)
{
    const char *mode, *in, *out;
    FILE *fi, *fo;
    mbe_parms cur, prev, enh;
    char d[FRAME_BITS];
    uint8_t packed[FRAME_BITS / 8];
    long n = 0;
    int spec;

    if (argc < 4) {
        fprintf(stderr, "usage: %s spec|dvsi frames.imbe out.pcm\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    in = argv[2];
    out = argv[3];
    spec = (strcmp(mode, "spec") == 0);

    fi = fopen(in, "rb");
    if (fi == NULL) {
        fprintf(stderr, "cannot open %s\n", in);
        return 1;
    }
    fo = fopen(out, "wb");
    if (fo == NULL) {
        fprintf(stderr, "cannot create %s\n", out);
        fclose(fi);
        return 1;
    }

    mbe_initMbeParms(&cur, &prev, &enh);

    while (fread(packed, 1, sizeof packed, fi) == sizeof packed) {
        for (int i = 0; i < FRAME_BITS; i++)
            d[i] = (char)((packed[i >> 3] >> (7 - (i & 7))) & 1u);
        if (spec) {
            decode_spec(d, &cur, &prev, &enh, 0);
            fwrite(pcm, sizeof pcm[0], PCM, fo);
        } else {
            if (mbe_processImbe4400Data(pcm, NULL, d, &cur, &prev, &enh) < 0) {
                fprintf(stderr, "frame %ld rejected\n", n);
                break;
            }
            fwrite(pcm, sizeof pcm[0], PCM, fo);
        }
        n++;
    }

    fclose(fi);
    fclose(fo);
    printf("ref_pcm %s: %ld frames\n", mode, n);
    return (n > 0) ? 0 : 1;
}
