/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DStarVocoder.c
 * @brief D-STAR voice frame front end, see DStarVocoder.h.
 *
 * The 72 bits of a DV frame are the AMBE-2020 channel frame: the Golay
 * codewords are interleaved with the unprotected bits, so the first step is
 * mbelib's air-bit map (the same table DSD, dsdcc and mbelib-neo use, from the
 * D-STAR specification).  After that the chain is the one mbelib's
 * mbe_decodeAmbe3600x2400Frame() runs:
 *
 *   Golay(23,12) on c0's codeword, overall parity bit fixed up  -> 12 bits
 *   LCG 173x + 13849 seeded with 16*c0 -> 23-bit PN, XORed off c1
 *   Golay(23,12) on c1                                          -> 12 bits
 *   c2 (11 bits) and c3 (14 bits) copied unprotected
 *
 * The Golay decoder is the one already in lib/dmr_vocoder (same primitive code,
 * syndrome table verified over all 2^23 codewords), so nothing is duplicated.
 */

#include "protocols/DSTAR/DStarVocoder.h"

#include <string.h>

#include "dmr_golay.h"

/* Air bit i (0..71, LSB first inside each byte) belongs to bit plane
 * (value >> 5) at column (value & 31): mbelib's dstar_dW/dstar_dX. */
static const uint8_t airMap[72] = {
    0x0A, 0x16, 0x6B, 0x49, 0x2A, 0x36, 0x0B, 0x17, 0x28,
    0x34, 0x09, 0x15, 0x6A, 0x48, 0x29, 0x35, 0x68, 0x46,
    0x27, 0x33, 0x08, 0x14, 0x69, 0x47, 0x06, 0x12, 0x67,
    0x45, 0x26, 0x32, 0x07, 0x13, 0x24, 0x30, 0x05, 0x11,
    0x66, 0x44, 0x25, 0x31, 0x64, 0x42, 0x23, 0x2F, 0x04,
    0x10, 0x65, 0x43, 0x02, 0x0E, 0x63, 0x41, 0x22, 0x2E,
    0x03, 0x0F, 0x20, 0x2C, 0x01, 0x0D, 0x62, 0x40, 0x21,
    0x2D, 0x60, 0x6C, 0x4A, 0x2B, 0x00, 0x0C, 0x61, 0x6D,
};

/**
 * @brief Codeword value of a bit plane slice, ready for the Golay decoder.
 *
 * mbelib keeps the codeword of a plane with the first bit of the slice as the
 * most significant one, while the (23,12) decoder in lib/dmr_vocoder takes the
 * codeword with its check bits at the bottom -- the same word read the other way
 * round (the DMR frame layout is bit-reversed with respect to D-STAR's).  The
 * bits are therefore assembled in reverse; a valid D-STAR codeword has syndrome
 * zero that way round (checked against mbelib's own encoder in the host test).
 */
static uint32_t planeWord(const uint8_t fr[4][24], int row, int first, int last)
{
    uint32_t v = 0;
    for (int j = last; j >= first; j--)
        v = (v << 1) | (fr[row][j] & 1u);
    return v;
}

int dstarVocoder_params(const uint8_t air[DSTAR_VOICE_BYTES],
                        uint8_t params[DSTAR_AMBE_BITS], int *errors)
{
    uint8_t fr[4][24];
    int errs = 0;

    memset(fr, 0, sizeof(fr));
    for (int i = 0; i < 72; i++) {
        uint8_t m = airMap[i];
        fr[m >> 5][m & 31] = (uint8_t)((air[i >> 3] >> (i & 7)) & 1u);
    }

    /* c0: the parity bit is fr[0][0], the codeword fr[0][1..23]. */
    uint32_t c0code = planeWord(fr, 0, 1, 23);
    uint16_t c0 = dmr_golay23127_decode(c0code);
    errs += (int)dmr_golay_last_errors;

    /* Overall (24,12) parity: when the Golay word needed no correction, the
     * extra parity bit must make the 24 bits even, and if it does not it is the
     * parity bit that is wrong (mbelib's mbe_eccAmbe3600C0). */
    if (errs == 0) {
        int ones = fr[0][0] & 1;
        for (int j = 1; j < 24; j++)
            ones += fr[0][j] & 1;
        if ((ones & 1) != 0) {
            fr[0][0] ^= 1;
            errs++;
        }
    }

    /* c1: XOR the LCG sequence seeded with 16 * c0, bit 23 down to 1. */
    {
        uint16_t pr = (uint16_t)(16u * c0);
        int k = 1;
        for (int j = 22; j >= 0; j--) {
            pr = (uint16_t)((173u * pr + 13849u) & 0xFFFFu);
            if ((pr >> 15) & 1u)
                fr[1][j] ^= 1u;
            k++;
        }
        (void)k;
    }

    uint32_t c1code = planeWord(fr, 1, 0, 22);
    uint16_t c1 = dmr_golay23127_decode(c1code);
    errs += (int)dmr_golay_last_errors;

    /* Pack the 49 parameter bits exactly as mbelib does: c0 and c1 as 12 bits
     * MSB first, then 11 bits of plane 2 and 14 bits of plane 3. */
    for (int i = 0; i < 12; i++)
        params[i] = (uint8_t)((c0 >> (11 - i)) & 1u);
    for (int i = 0; i < 12; i++)
        params[12 + i] = (uint8_t)((c1 >> (11 - i)) & 1u);
    for (int i = 0; i < 11; i++)
        params[24 + i] = (uint8_t)(fr[2][10 - i] & 1u);
    for (int i = 0; i < 14; i++)
        params[35 + i] = (uint8_t)(fr[3][13 - i] & 1u);

    if (errors != NULL)
        *errors = errs;
    return 0;
}

/* Weak default: no 2400 core linked, so nothing plays. */
__attribute__((weak)) int dstarVocoder_decode(const uint8_t air[DSTAR_VOICE_BYTES],
                                              int16_t pcm[DSTAR_PCM_SAMPLES])
{
    (void)air;
    (void)pcm;
    return -1;
}

__attribute__((weak)) bool dstarVocoder_available(void)
{
    return false;
}
