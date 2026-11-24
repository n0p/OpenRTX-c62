/* Reference AMBE+2 decoder, verbatim copy of OpenDMR's opendmr_decode() path
 * (opendmr.cpp) on top of mbelib-neo.  Used only to produce reference PCM for
 * the accuracy comparison; it is not part of the delivered decoder.
 *
 * usage: ref_dec <in.ambe> <out.raw> [n_frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbelib.h"

extern unsigned int ref_decode24128(unsigned int code);
extern unsigned int ref_decode23127(unsigned int code);

static unsigned int prng_mask_23bit(unsigned int aOrig)
{
    unsigned short pr[24];
    pr[0] = (unsigned short)(16u * aOrig);
    for (int i = 1; i < 24; i++)
        pr[i] = (unsigned short)((173u * (unsigned)pr[i - 1] + 13849u) % 65536u);
    for (int i = 1; i < 24; i++)
        pr[i] = pr[i] / 32768u;
    unsigned int mask = 0;
    for (int i = 1; i <= 23; i++)
        if (pr[i])
            mask |= (1u << (23 - i));
    return mask;
}

/* identical to opendmr.cpp:decode_ambe_frame */
static void decode_ambe_frame(const unsigned char *f72, char ambe_d[49])
{
    unsigned int a = 0, b = 0, c = 0;
    for (int i = 0; i < 24; i++)
        if ((f72[i / 8] >> (7 - (i % 8))) & 1)
            a |= (0x800000u >> i);
    for (int i = 0; i < 23; i++) {
        int pos = 24 + i;
        if ((f72[pos / 8] >> (7 - (pos % 8))) & 1)
            b |= (0x400000u >> i);
    }
    for (int i = 0; i < 25; i++) {
        int pos = 47 + i;
        if ((f72[pos / 8] >> (7 - (pos % 8))) & 1)
            c |= (0x1000000u >> i);
    }
    unsigned int aOrig = ref_decode24128(a);
    unsigned int mask = prng_mask_23bit(aOrig);
    unsigned int bOrig = ref_decode23127(b ^ mask);
    memset(ambe_d, 0, 49);
    for (int i = 0; i < 12; i++)
        ambe_d[i] = (char)((aOrig >> (11 - i)) & 1);
    for (int i = 0; i < 12; i++)
        ambe_d[12 + i] = (char)((bOrig >> (11 - i)) & 1);
    for (int i = 0; i < 25; i++)
        ambe_d[24 + i] = (char)((c >> (24 - i)) & 1);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.ambe out.raw [n]\n", argv[0]);
        return 2;
    }
    FILE *fi = fopen(argv[1], "rb");
    FILE *fo = fopen(argv[2], "wb");
    if (!fi || !fo)
        return 2;
    long n = argc > 3 ? atol(argv[3]) : 0;

    static mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);

    unsigned char fr[9];
    short pcm[160];
    long done = 0;
    while (fread(fr, 1, 9, fi) == 9 && (!n || done < n)) {
        char d[49];
        int errs = 0, errs2 = 0;
        char estr[64] = {0};
        decode_ambe_frame(fr, d);
        mbe_processAmbe2450Data(pcm, &errs, &errs2, estr, d, &cur, &prev, &enh, 3);
        fwrite(pcm, 2, 160, fo);
        done++;
    }
    fclose(fi);
    fclose(fo);
    printf("ref: %ld frames\n", done);
    return 0;
}
