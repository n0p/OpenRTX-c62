/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_golay.c
 * @brief Golay(23,12)/Golay(24,12) syndrome decoders for the DMR vocoder.
 */

#include "dmr_golay.h"

/* g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 */
#define DMR_GOLAY_GENPOL 0x0C75u
#define DMR_GOLAY_X11 0x000800u
#define DMR_GOLAY_MASK12 0xFFFFF800u

unsigned int dmr_golay_last_errors;

/**
 * @brief Remainder of a 23-bit vector divided by g(x).
 *
 * @param pattern Vector (information bits, error pattern or received word).
 * @return Syndrome of the vector.
 */
static uint32_t golay_syndrome(uint32_t pattern)
{
    uint32_t aux = DMR_GOLAY_X11 << 11; /* X^22 */

    if (pattern >= DMR_GOLAY_X11) {
        while (pattern & DMR_GOLAY_MASK12) {
            while ((aux & pattern) == 0u)
                aux >>= 1;
            pattern ^= (aux / DMR_GOLAY_X11) * DMR_GOLAY_GENPOL;
        }
    }
    return pattern;
}

uint16_t dmr_golay23127_decode(uint32_t code23)
{
    uint32_t syndrome = golay_syndrome(code23);
    uint32_t error = dmr_golay23127_err[syndrome & 0x7FFu];

    code23 ^= error;
    dmr_golay_last_errors = (unsigned int)__builtin_popcount(error);

    return (uint16_t)(code23 >> 11);
}

uint16_t dmr_golay24128_decode(uint32_t code24)
{
    return dmr_golay23127_decode(code24 >> 1);
}
