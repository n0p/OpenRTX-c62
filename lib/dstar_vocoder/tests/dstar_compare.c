/* Compares the C62's AMBE 3600x2400 decoder with mbelib-neo's.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reads a frame bank (9 bytes per frame, air order), decodes it twice -- once
 * through the port's firmware entry points (dstarVocoder_decode(), which is
 * the whole receive path from air bits to PCM) and once through the reference
 * (ref_dstar, dumped to a file) -- and reports how many frames are identical,
 * how many differ by one step of the 16-bit output, and the SNR.
 *
 * usage: dstar_compare <bank.air> <ref.pcm> [out.pcm]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dstar_vocoder.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s bank.air ref.pcm [out.pcm]\n", argv[0]);
        return 2;
    }
    FILE *fi = fopen(argv[1], "rb");
    FILE *fr = fopen(argv[2], "rb");
    if (!fi || !fr) {
        fprintf(stderr, "cannot open inputs\n");
        return 2;
    }
    FILE *fo = argc > 3 ? fopen(argv[3], "wb") : NULL;

    dstarVocoder_reset();

    unsigned char air[9];
    int16_t ref[160], mine[160];
    long frames = 0, identical = 0, oneLsb = 0, worse = 0;
    long maxDiff = 0, diffSamples = 0, samples = 0;
    double sig = 0.0, err = 0.0;
    int firstBad = -1;

    while (fread(air, 1, 9, fi) == 9) {
        if (fread(ref, 2, 160, fr) != 160)
            break;
        if (dstarVocoder_decode(air, mine) != 0)
            break;
        frames++;
        if (fo)
            fwrite(mine, 2, 160, fo);

        long fmax = 0, fdiff = 0;
        for (int i = 0; i < 160; i++) {
            long d = (long)mine[i] - (long)ref[i];
            if (d < 0)
                d = -d;
            if (d > fmax)
                fmax = d;
            if (d != 0)
                fdiff++;
            sig += (double)ref[i] * (double)ref[i];
            err += (double)d * (double)d;
            samples++;
        }
        diffSamples += fdiff;
        if (fmax > maxDiff)
            maxDiff = fmax;
        if (fmax == 0)
            identical++;
        else if (fmax <= 1)
            oneLsb++;
        else {
            worse++;
            if (firstBad < 0)
                firstBad = (int)frames - 1;
        }
    }

    printf("frames %ld: %ld bit-exact, %ld within 1 LSB, %ld worse "
           "(first bad %d)\n",
           frames, identical, oneLsb, worse, firstBad);
    printf("samples %ld: %ld differ (%.4f %%), max |diff| %ld LSB, "
           "SNR %.1f dB\n",
           samples, diffSamples, 100.0 * (double)diffSamples / (double)samples,
           maxDiff, sig > 0.0 ? 10.0 * log10(sig / (err > 0.0 ? err : 1e-30)) : 0.0);

    fclose(fi);
    fclose(fr);
    if (fo)
        fclose(fo);
    return (identical == frames) ? 0 : 1;
}
