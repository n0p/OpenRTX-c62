/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file P25Defs.c
 * @brief Constants and helpers of the P25 Phase 1 air interface.
 */

#include "protocols/P25/P25Defs.h"

void p25_sync_symbols(int8_t out[P25_SYNC_SYMBOLS])
{
    /*
     * TIA-102.BAAA symbol mapping: dibit 01 is +1800 Hz, 00 is +600 Hz, 10 is
     * -600 Hz and 11 is -1800 Hz.  The sync field carries only the outer two,
     * so its 24 symbols are +1 or -1.
     */
    for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
    {
        uint32_t dibit = (uint32_t)((P25_SYNC_WORD >> (46 - 2 * i)) & 0x3);
        out[i] = (dibit == 1) ? 1 : -1;
    }
}

const char *p25_duid_name(enum p25Duid duid)
{
    switch (duid)
    {
        case P25_DUID_HDU: return "HDU";
        case P25_DUID_TDU: return "TDU";
        case P25_DUID_LDU1: return "LDU1";
        case P25_DUID_TSBK: return "TSBK";
        case P25_DUID_LDU2: return "LDU2";
        case P25_DUID_PDU: return "PDU";
        case P25_DUID_TDU15: return "TDULC";
        default: return "?";
    }
}

bool p25_duid_is_voice(enum p25Duid duid)
{
    return duid == P25_DUID_LDU1 || duid == P25_DUID_LDU2;
}

uint32_t p25_gather(const uint8_t *bits, const uint16_t *pos, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (uint32_t)(bits[pos[i]] & 1);
    return v;
}
