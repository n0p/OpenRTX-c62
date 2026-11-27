/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stddef.h>

#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnPhy.h"

/**
 * Frame sync word: 10 symbols, hex CDF59 (TS 1-A Table 4.4-2).  Read two bits
 * per symbol, 11 00 11 01 11 11 01 01 10 01, with 01 = +3, 00 = +1, 10 = -1
 * and 11 = -3.
 */
const int8_t nxdnFsw[NXDN_FSW_SYMBOLS] = {
    -3, +1, -3, +3, -3, -3, +3, +3, -1, +3,
};

/**
 * Default data scrambler of TS 1-A 4.6: a 9-bit LFSR (X^9 + X^4 + 1) seeded
 * with 0b011100100, one output bit per symbol, inverting the sign of the dibit
 * when it is 1.  This is the mask for the 182 payload dibits of a frame, from
 * the first LICH dibit to the last VCH4 dibit.
 */
const uint8_t nxdnScramble[23] = {
    0xE4, 0x54, 0xC3, 0x5E, 0x76, 0xA2, 0x50, 0xCB, 0x6F, 0x92, 0xF6, 0x93,
    0x95, 0x19, 0x30, 0xA6, 0x58, 0xFA, 0x8B, 0xC6, 0x35, 0xCD, 0x23,
};

void nxdnLichParse(uint8_t raw, struct nxdnLich *lich)
{
    lich->raw = (uint8_t)(raw & 0x7F);
    lich->rfType = (uint8_t)((raw >> 5) & 0x03);
    lich->usc = (uint8_t)((raw >> 3) & 0x03);
    lich->steal = (uint8_t)((raw >> 1) & 0x03);
    lich->outbound = (raw & 1) != 0;
}

void nxdnLichVoice(const struct nxdnLich *lich, int *first, int *count)
{
    *first = 0;
    *count = 0;

    if ((lich->rfType != NXDN_RF_RTCH) && (lich->rfType != NXDN_RF_RDCH))
        return;

    /* Voice rides with the SACCH superframe; the non-superframe structure is
     * the call's first and last frame, which carry voice too. */
    if ((lich->usc == NXDN_USC_UDCH) || (lich->usc == NXDN_USC_SACCH_IDLE))
        return;

    switch (lich->steal) {
    case NXDN_STEAL_NONE:
        *first = 0;
        *count = 4;
        break;
    case NXDN_STEAL_FIRST:
        /* VCH1/VCH2 are FACCH1 (enum nxdnSteal): the *first* half of the
         * slot carries signalling and the second half carries voice.  The
         * two cases used to be the other way round, so a frame with half a
         * slot stolen handed the 36 Golay protected FACCH1 dibits to the
         * vocoder as AMBE+2 -- which always "decodes" something, a burst of
         * noise at speech volume -- and threw away the half that had voice. */
        *first = 2;
        *count = 2;
        break;
    case NXDN_STEAL_SECOND:
        /* VCH3/VCH4 are FACCH1: the first half is voice */
        *first = 0;
        *count = 2;
        break;
    default:
        break;
    }
}

bool nxdnRanFollows(uint8_t setting, int lastRan)
{
    /* "Any" is every value the 6 bit field cannot hold */
    if (setting > 63)
        return true;

    /* Before the first SACCH there is no RAN to compare with: pass, or the
     * receiver would never get the frame that tells it the RAN */
    if (lastRan < 0)
        return true;

    return lastRan == (int)setting;
}

bool nxdnRanFrame(uint8_t setting, int *lastRan, const struct nxdnFrame *frame)
{
    if (frame->sacchOk)
        *lastRan = (int)(frame->sacchSr & 0x3Fu);

    return nxdnRanFollows(setting, *lastRan);
}

uint8_t nxdnRanTransmit(uint8_t setting)
{
    return (setting > 63) ? 63 : setting;
}
