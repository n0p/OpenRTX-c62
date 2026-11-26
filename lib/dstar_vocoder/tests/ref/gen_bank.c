/* Frame bank generator for the D-STAR vocoder tests.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Verification material: uses mbelib-neo's *encoder* to turn 49-bit parameter
 * words into valid 72-bit D-STAR voice frames, so that every frame fed to the
 * decoders is one that the Golay words accept (a random 72-bit word usually is
 * not inside the correcting power of the code, and both decoders would then
 * decode it into whatever the syndrome table says -- which is still a fair
 * test of the FEC, but it does not exercise the parameter tables).
 *
 * usage: gen_bank <out.air> [seed]
 *
 * The bank mixes, in this order:
 *   - the D-STAR null frame (what a radio sends with no microphone signal);
 *   - voice frames whose fields are drawn from their own codebook, so the
 *     pitch, voicing, gain, PRBA and HOC tables are all exercised;
 *   - voice frames with one to five channel bit errors, which walk the
 *     repeat/erasure paths;
 *   - tone frames (b0 126, single and dual frequency);
 *   - fully random 49-bit words, for the extreme ends of the tables.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbelib-neo/mbelib.h"

static uint32_t rng = 1;

static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

/* Assemble a 49-bit parameter word from the field layout of AMBE 3600x2400
 * (mbelib decode: b0 = d[0..5],d[48]; b1 = d[38..41]; b2 = d[6..9],d[42..43];
 * b3 = d[10..16],d[44..45]; b4 = d[17..21],d[46..47]; b5 = d[22,23,25,26];
 * b6 = d[27..30]; b7 = d[31..34]; b8 = d[35,36,37]). */
static void put_bits(char *d, const int *idx, int count, int value)
{
    for (int i = 0; i < count; i++)
        d[idx[i]] = (char)((value >> (count - 1 - i)) & 1);
}

static void compose(char d[49], int b0, int b1, int b2, int b3, int b4,
                    int b5, int b6, int b7, int b8)
{
    static const int i0[7] = {0, 1, 2, 3, 4, 5, 48};
    static const int i1[4] = {38, 39, 40, 41};
    static const int i2[6] = {6, 7, 8, 9, 42, 43};
    static const int i3[9] = {10, 11, 12, 13, 14, 15, 16, 44, 45};
    static const int i4[7] = {17, 18, 19, 20, 21, 46, 47};
    static const int i5[4] = {22, 23, 25, 26};
    static const int i6[4] = {27, 28, 29, 30};
    static const int i7[4] = {31, 32, 33, 34};
    static const int i8[3] = {35, 36, 37};

    memset(d, 0, 49);
    put_bits(d, i0, 7, b0);
    put_bits(d, i1, 4, b1);
    put_bits(d, i2, 6, b2);
    put_bits(d, i3, 9, b3);
    put_bits(d, i4, 7, b4);
    put_bits(d, i5, 4, b5);
    put_bits(d, i6, 4, b6);
    put_bits(d, i7, 4, b7);
    put_bits(d, i8, 3, b8);
}

static int emit(FILE *f, const char d[49])
{
    char fr[4][24];
    unsigned char air[9];
    if (mbe_encodeAmbe3600x2400Frame(d, fr) != 0)
        return -1;
    if (mbe_encodeDStarDVData(fr, air) != 0)
        return -1;
    fwrite(air, 1, 9, f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.air [seed]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return 2;
    if (argc > 2)
        rng = (uint32_t)strtoul(argv[2], NULL, 0);
    if (rng == 0)
        rng = 1;

    int n = 0;

    /* 1. the null frame: 25 frames of it, the length of a D-STAR silence. */
    {
        const unsigned char nullAir[9] = {0x9E, 0x8D, 0x32, 0x88, 0x26,
                                          0x1A, 0x3F, 0x61, 0xE8};
        for (int i = 0; i < 25; i++) {
            fwrite(nullAir, 1, 9, f);
            n++;
        }
    }

    /* 2. voice frames from the codebooks.  b0 0..119 is voice; 120..123 are
     * the AMBE+2 erasure codes and 124..127 the special ones, so the voice
     * part of the bank stays below 120.  A slow pitch walk keeps consecutive
     * frames correlated, which is what the phase/prediction paths want. */
    int b0 = 20;
    for (int i = 0; i < 1500; i++) {
        char d[49];
        b0 += (int)(rnd() % 7) - 3;
        if (b0 < 0)
            b0 = 0;
        if (b0 > 119)
            b0 = 119;
        int b1 = (int)(rnd() & 15);
        /* Mostly voiced voicing vectors, so the voiced paths dominate. */
        if ((i % 3) != 0)
            b1 = (int)(rnd() & 3);
        compose(d, b0, b1, (int)(rnd() & 63), (int)(rnd() & 511),
                (int)(rnd() & 255), (int)(rnd() & 15), (int)(rnd() & 15),
                (int)(rnd() & 15), (int)(rnd() & 7));
        if (emit(f, d) == 0)
            n++;
    }

    /* 3. the same kind of frames with 1..5 channel bit errors. */
    for (int i = 0; i < 600; i++) {
        char d[49];
        char fr[4][24];
        unsigned char air[9];
        compose(d, 20 + (int)(rnd() % 100), (int)(rnd() & 15),
                (int)(rnd() & 63), (int)(rnd() & 511), (int)(rnd() & 255),
                (int)(rnd() & 15), (int)(rnd() & 15), (int)(rnd() & 15),
                (int)(rnd() & 7));
        if (mbe_encodeAmbe3600x2400Frame(d, fr) != 0)
            continue;
        if (mbe_encodeDStarDVData(fr, air) != 0)
            continue;
        int errs = 1 + (int)(rnd() % 5);
        for (int k = 0; k < errs; k++)
            air[(int)(rnd() % 9)] ^= (unsigned char)(1u << (rnd() & 7));
        fwrite(air, 1, 9, f);
        n++;
    }

    /* 4. tone frames: b0 = 126 with a single tone (index 5..122) and a DTMF
     * pair (128..147), volume in the low bits of the tone word. */
    for (int i = 0; i < 100; i++) {
        char d[49];
        int index = (i & 1) ? 32 : (128 + (int)(rnd() % 20));
        int vol = 1 + (int)(rnd() % 63);
        /* The tone index is carried in the 12 bits of c1 (d[12..23]) and the
         * amplitude in d[0..5]; compose() puts b0 in d[0..5],d[48], so build
         * the word by hand here. */
        memset(d, 0, 49);
        put_bits(d, (const int[]){0, 1, 2, 3, 4, 5, 48}, 7, 126);
        for (int k = 0; k < 12; k++)
            d[12 + k] = (char)((index >> (11 - k)) & 1);
        for (int k = 0; k < 6; k++)
            d[24 + k] = (char)((vol >> (5 - k)) & 1);
        if (emit(f, d) == 0)
            n++;
    }

    /* 5. fully random parameter words. */
    for (int i = 0; i < 400; i++) {
        char d[49];
        uint64_t w = ((uint64_t)rnd() << 32) | rnd();
        w &= 0x1FFFFFFFFFFFFull;
        for (int k = 0; k < 49; k++)
            d[k] = (char)((w >> (48 - k)) & 1);
        if (emit(f, d) == 0)
            n++;
    }

    fclose(f);
    printf("bank: %d frames\n", n);
    return 0;
}
