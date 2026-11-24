/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DpmrData.c
 * @brief dPMR short data, see DpmrData.h for the clauses.
 */

#include "protocols/DPMR/DpmrData.h"

#include <string.h>

/*
 * Table 5.9b, four bits.  The values above 0111 are reserved and reported as
 * they are: a receiver that drops them cannot tell a status message from a
 * manufacturer extension of the same system.
 */
void dpmrData_sldDecode(uint32_t sld, struct dpmrSld *out)
{
    /*
     * Clause 5.9.2, 18 bits: Reserved 5, DP 2, Format 4, Cont. 1, length 6,
     * most significant bit first.
     */
    memset(out, 0, sizeof(*out));

    out->dp = (uint8_t)((sld >> 11) & 0x3u);
    out->format = (uint8_t)((sld >> 7) & 0xFu);
    out->last = ((sld >> 6) & 1u) != 0;
    out->length = (uint8_t)(sld & 0x3Fu);
}

unsigned dpmrData_slowDecode(uint32_t sld, struct dpmrSlowData *out)
{
    /*
     * Clause 5.9.1: Cont. 1 + user data 8, twice.  The first byte is the most
     * significant one, so the 18 bits are flag, byte, flag, byte from bit 17
     * down to bit 0.
     */
    for (unsigned i = 0; i < 2; i++) {
        unsigned shift = 9 * (1 - i);

        out->last[i] = ((sld >> (shift + 8)) & 1u) != 0;
        out->byte[i] = (uint8_t)((sld >> shift) & 0xFFu);
    }

    return 2;
}

void dpmrSld_reset(struct dpmrSldMessage *m)
{
    memset(m, 0, sizeof(*m));
}

void dpmrSld_push(struct dpmrSldMessage *m, const struct dpmrSlowData *d)
{
    if (m->complete) {
        /* A new message starts where the last one ended */
        m->len = 0;
        m->complete = false;
        m->overflow = false;
    }

    for (unsigned i = 0; i < 2; i++) {
        if (m->len < DPMR_SLD_TEXT_BYTES) {
            m->text[m->len++] = d->byte[i];
        } else {
            m->overflow = true;
        }

        if (d->last[i]) {
            m->complete = true;
            return;
        }
    }
}
