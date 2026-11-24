/* Reference D-STAR voice decoder: mbelib-neo, verbatim D-STAR path.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Verification material only: nothing here is linked into the firmware.  It is
 * the oracle the C port in lib/dstar_vocoder is compared against.
 *
 * usage: ref_dstar <in.air> <out.pcm> [n_frames]
 *
 * The chain is exactly what a D-STAR receiver does with the 72 voice bits of a
 * DV frame: mbe_decodeDStarDVData() undoes the air interleaving, and
 * mbe_processAmbe3600x2400Frame() runs the FEC, the parameter decode and the
 * synthesis.  Both are mbelib-neo's own entry points, so what this program
 * prints is the reference's PCM by construction.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbelib-neo/mbelib.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.air out.pcm [n]\n", argv[0]);
        return 2;
    }
    FILE *fi = fopen(argv[1], "rb");
    FILE *fo = fopen(argv[2], "wb");
    if (!fi || !fo)
        return 2;
    long n = argc > 3 ? atol(argv[3]) : 0;

    static mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);

    unsigned char air[9];
    short pcm[160];
    long done = 0;
    while (fread(air, 1, 9, fi) == 9 && (!n || done < n)) {
        char fr[4][24];
        char d[49];
        mbe_process_result res;
        mbe_initProcessResult(&res);
        if (mbe_decodeDStarDVData(air, fr) < 0)
            continue;
        if (mbe_processAmbe3600x2400Frame(pcm, &res, fr, d, &cur, &prev, &enh) < 0)
            continue;
        fwrite(pcm, 2, 160, fo);
        done++;
    }
    fclose(fi);
    fclose(fo);
    printf("ref: %ld frames\n", done);
    return 0;
}
