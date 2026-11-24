/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dstar_vocoder_test.c
 * @brief Bit-exactness test of the D-STAR voice front end against mbelib-neo.
 *
 * Feeds the same 72-bit frames to dstarVocoder_params() and to mbelib-neo's
 * mbe_decodeDStarDVData() + mbe_decodeAmbe3600x2400Frame(), and compares the
 * 49 parameter bits, both clean and with channel bit errors (so the Golay
 * correction and the PN demodulation are exercised, not just the copy path).
 *
 * Build with mbelib-neo checked out next to this repository:
 *   gcc -O2 -std=c11 -Iopenrtx/include -I<mbelib-neo>/include \
 *       -o /tmp/dstar_vocoder_test tests/host/dstar/dstar_vocoder_test.c \
 *       openrtx/src/protocols/DSTAR/DStarVocoder.c \
 *       lib/dmr_vocoder/dmr_golay.c lib/dmr_vocoder/dmr_golay_table.c \
 *       <mbelib-neo>/build/libmbe-neo.a -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbelib-neo/mbelib.h"
#include "protocols/DSTAR/DStarVocoder.h"

static uint32_t rngState = 12345;

static uint32_t rnd(void)
{
    rngState = rngState * 1103515245u + 12345u;
    return rngState >> 8;
}

int main(void)
{
    int frames = 20000;
    int bad = 0;
    int tested = 0;
    int errFrames = 0;

    for (int f = 0; f < frames; f++) {
        /* Build a valid frame from a random 49-bit parameter word with mbelib's
         * own encoder, so the frame is inside the correcting power of the two
         * Golay words (a random 72-bit word usually is not, and mbelib refuses
         * those instead of returning parameters). */
        char params[49];
        for (int i = 0; i < 49; i++)
            params[i] = (char)(rnd() & 1u);

        char fr[4][24];
        unsigned char air[DSTAR_VOICE_BYTES];
        if (mbe_encodeAmbe3600x2400Frame(params, fr) != 0)
            continue;
        if (mbe_encodeDStarDVData(fr, air) != 0)
            continue;

        /* Channel errors inside the two FEC words, so the correction and the
         * PN demodulation are exercised rather than just the copy path. */
        int mode = f % 5;
        if (mode == 1)
            air[(int)(rnd() % 6)] ^= (uint8_t)(1u << (rnd() & 7));
        else if (mode == 2) {
            for (int k = 0; k < 2; k++)
                air[(int)(rnd() % 6)] ^= (uint8_t)(1u << (rnd() & 7));
        } else if (mode == 3) {
            for (int k = 0; k < 3; k++)
                air[(int)(rnd() % 3)] ^= (uint8_t)(1u << (rnd() & 7));
        }

        uint8_t mine[DSTAR_AMBE_BITS];
        int errs = 0;
        dstarVocoder_params(air, mine, &errs);

        char ref[49];
        mbe_process_result res;
        mbe_initProcessResult(&res);
        if (mbe_decodeDStarDVData(air, fr) != 0)
            continue;
        /* The return value of the frame decoder is the corrected error total,
         * not a status (mbelib-neo header), so every frame with a decodable
         * word is comparable. */
        int mbelibErrs = mbe_decodeAmbe3600x2400Frame(fr, ref, &res);
        (void)mbelibErrs;

        tested++;
        if (errs > 0)
            errFrames++;

        int diff = 0;
        for (int i = 0; i < DSTAR_AMBE_BITS; i++)
            if ((uint8_t)(ref[i] & 1) != mine[i])
                diff++;
        if (diff != 0) {
            if (bad < 5)
                printf("frame %d: %d/49 parameter bits differ (mine reports %d "
                       "corrected bits, mbelib %d+%d)\n", f, diff, errs,
                       res.c0_errors, res.protected_errors);
            bad++;
        }
    }

    {
        /* The all-zero and null frames are the ones a silent radio sends. */
        const uint8_t nulls[2][9] = {
            {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8},
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        };
        for (int k = 0; k < 2; k++) {
            uint8_t mine[DSTAR_AMBE_BITS];
            char fr[4][24];
            char ref[49];
            mbe_process_result res;
            mbe_initProcessResult(&res);
            dstarVocoder_params(nulls[k], mine, NULL);
            if (mbe_decodeDStarDVData(nulls[k], fr) != 0)
                continue;
            (void)mbe_decodeAmbe3600x2400Frame(fr, ref, &res);
            tested++;
            for (int i = 0; i < DSTAR_AMBE_BITS; i++)
                if ((uint8_t)(ref[i] & 1) != mine[i])
                    bad++;
        }
    }

    printf("%d frames compared, %d with channel errors, %d mismatches -> %s\n",
           tested, errFrames, bad, bad == 0 ? "bit-exact" : "MISMATCH");
    return bad == 0 ? 0 : 1;
}
