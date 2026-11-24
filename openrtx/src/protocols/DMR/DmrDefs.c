/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrDefs.c
 * @brief DMR air-interface tables. See DmrDefs.h for their provenance.
 */

#include "protocols/DMR/DmrDefs.h"

const uint8_t dmr_cach_tact_pos[7] = { 0, 4, 8, 12, 14, 18, 22 };

const uint8_t dmr_cach_payload_pos[17] = { 1,  2,  3,  5,  6,  7,  9,  10, 11,
                                           13, 15, 16, 17, 19, 20, 21, 23 };

/*
 * 72 on-air bits of a voice frame (36 dibits, MSB first) map to the
 * ambe_fr[4][24] planes through these four schedules.
 */
const int8_t dmr_rw[36] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                            0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2,
                            0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2 };

const int8_t dmr_rx[36] = { 23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5,
                            17, 4,  16, 3, 15, 2, 14, 1, 13, 0, 12, 10,
                            11, 9,  10, 8, 9,  7, 8,  6, 7,  5, 6,  4 };

const int8_t dmr_ry[36] = { 0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3,
                            1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
                            1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3 };

const int8_t dmr_rz[36] = { 5,  3,  4,  2,  3,  1, 2,  0, 1,  13, 0,  12,
                            22, 11, 21, 10, 20, 9, 19, 8, 18, 7,  17, 6,
                            16, 5,  15, 4,  14, 3, 13, 2, 12, 1,  11, 0 };

/*
 * ambe_fr[row][col] -> transmission-order bit index.  Matches mbelib's
 * mbe_eccAmbe3600Data_common: A = row0[23..0], B = row1[22..0],
 * C = row2[10..0] followed by row3[13..0].
 */
const int8_t dmr_ambe_layout[4][24] = {
    { 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12,
      11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0 },
    { 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35,
      34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, -1 },
    { 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 71, 70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60,
      59, 58, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
};

const uint16_t dmr_golay_basis[8] = { 0xB08E, 0xE093, 0x70A9, 0x60DC,
                                      0x7036, 0xD06C, 0x90D9, 0xA03D };

bool dmr_sync_is_voice(enum dmrSync sync)
{
    return (sync == DMR_SYNC_BS_VOICE) || (sync == DMR_SYNC_MS_VOICE);
}

bool dmr_sync_is_bs(enum dmrSync sync)
{
    return (sync == DMR_SYNC_BS_VOICE) || (sync == DMR_SYNC_BS_DATA);
}

void dmr_sync_symbols(enum dmrSync sync, int8_t out[DMR_SYNC_SYMBOLS])
{
    uint64_t word;

    switch (sync) {
        case DMR_SYNC_BS_VOICE:
            word = DMR_SYNC_WORD_BS_VOICE;
            break;
        case DMR_SYNC_BS_DATA:
            word = DMR_SYNC_WORD_BS_DATA;
            break;
        case DMR_SYNC_MS_VOICE:
            word = DMR_SYNC_WORD_MS_VOICE;
            break;
        case DMR_SYNC_MS_DATA:
            word = DMR_SYNC_WORD_MS_DATA;
            break;
        default:
            for (size_t i = 0; i < DMR_SYNC_SYMBOLS; i++)
                out[i] = 0;
            return;
    }

    /*
     * Dibit 3 or 2 (the marking symbols of the sync field) is the +3 level,
     * 1 or 0 the -3 one: the field carries no inner symbols.
     */
    for (size_t i = 0; i < DMR_SYNC_SYMBOLS; i++) {
        unsigned shift = 2 * (DMR_SYNC_SYMBOLS - 1 - (unsigned)i);
        uint8_t dibit = (uint8_t)((word >> shift) & 3u);
        out[i] = (dibit >= 2) ? 3 : -3;
    }
}

const char *dmr_sync_name(enum dmrSync sync)
{
    static const char *const names[] = { "none", "BS voice", "BS data",
                                         "MS voice", "MS data" };

    if ((unsigned)sync >= (sizeof(names) / sizeof(names[0])))
        return names[0];

    return names[sync];
}

const char *dmr_data_type_name(uint8_t type)
{
    static const char *const names[] = {
        "PI hdr",   "VoiceLC", "Term", "CSBK", "MBC hdr", "MBC",
        "Data hdr", "1/2",     "3/4",  "Idle", "Rate 1",  "Unified",
    };

    if (type >= (sizeof(names) / sizeof(names[0])))
        return "?";

    return names[type];
}
