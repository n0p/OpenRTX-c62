/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DpmrTx.c
 * @brief dPMR transmit, see DpmrTx.h.
 */

#include <string.h>

#include "protocols/DPMR/DpmrTx.h"

/** Preamble of clause 6.1.6 in the header frame: 72 bits, 9 x 0x5F. */
#define PREAMBLE_SYMBOLS 36

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

int8_t dpmrTxDibitLevel(uint8_t dibit)
{
    /* Table 12.1, the map DpmrPhy.c slices with: high bit = negative */
    static const int8_t level[4] = { +1, +3, -1, -3 };

    return level[dibit & 3u];
}

/** Convert dibits to symbol levels. */
static void to_levels(const uint8_t *dibits, int n, int8_t *levels)
{
    for (int i = 0; i < n; i++)
        levels[i] = dpmrTxDibitLevel(dibits[i]);
}

/** The 12-bit channel code as 12 dibits: bit c leaves as (c, 1). */
static void channelCodeDibits(uint16_t code, uint8_t *dibits)
{
    for (int i = 0; i < 12; i++)
        dibits[i] = (uint8_t)((((code >> (11 - i)) & 1u) << 1) | 1u);
}

/* ------------------------------------------------------------------ */
/* Frames                                                             */
/* ------------------------------------------------------------------ */

void dpmrTx_setup(dpmr_tx_t *tx, uint32_t ownId, uint32_t calledId,
                  uint16_t channelCode)
{
    memset(tx, 0, sizeof(*tx));
    tx->ownId = ownId & 0xFFFFFFu;
    tx->calledId = calledId & 0xFFFFFFu;
    tx->channelCode = channelCode & 0xFFFu;
    tx->mode = DPMR_MODE_VOICE;
    tx->version = 0;
    tx->format = DPMR_FMT_PEER;
}

void dpmrTx_setCallType(dpmr_tx_t *tx, uint8_t mode, uint8_t version,
                        uint8_t format)
{
    tx->mode = mode & 0x07u;
    tx->version = version & 0x03u;
    tx->format = format & 0x03u;
}

void dpmrTx_preamble(int8_t levels[DPMR_FRAME_SYMBOLS])
{
    /* 0x5F is 0101 1111: +3 +3 -3 -3 in table 12.1 */
    static const uint8_t pattern[4] = { 1, 1, 3, 3 };

    for (int s = 0; s < DPMR_FRAME_SYMBOLS; s++)
        levels[s] = dpmrTxDibitLevel(pattern[s & 3u]);
}

void dpmrTx_header(dpmr_tx_t *tx, uint8_t ht, uint16_t ci,
                   int8_t levels[DPMR_FRAME_SYMBOLS])
{
    uint8_t dibits[DPMR_FRAME_SYMBOLS];

    memset(dibits, 0, sizeof(dibits));

    /* Preamble: 0x5F is 0101 1111, i.e. +3 +3 -3 -3 in table 12.1 */
    for (int s = 0; s < PREAMBLE_SYMBOLS; s++) {
        static const uint8_t pattern[4] = { 1, 1, 3, 3 }; /* dibits of 0x5F */

        dibits[s] = pattern[s & 3u];
    }

    /* FS1 is 48 bits, 24 symbols, right after the preamble (4.2.2) */
    {
        uint8_t sym[24];
        size_t n = dpmrDefs_syncLevels(DPMR_SYNC_FS1, (int8_t *)sym);

        for (size_t i = 0; i < n; i++) {
            /* level -> dibit in table 12.1 */
            const int8_t l = (int8_t)sym[i];

            dibits[PREAMBLE_SYMBOLS + i] =
                (uint8_t)(((l < 0) ? 2 : 0) | ((l == 3 || l == -3) ? 1 : 0));
        }
    }

    /* HI0 and HI1: the same 120-bit field, twice, with the channel code
     * between them (4.2.2) */
    dpmrFec_hiEncode(ht, tx->calledId, tx->ownId, tx->mode, tx->version,
                     tx->format, ci, &dibits[PREAMBLE_SYMBOLS + 24]);
    channelCodeDibits(tx->channelCode,
                      &dibits[PREAMBLE_SYMBOLS + 24 + DPMR_HI_SYMBOLS]);
    dpmrFec_hiEncode(ht, tx->calledId, tx->ownId, tx->mode, tx->version,
                     tx->format, ci,
                     &dibits[PREAMBLE_SYMBOLS + 24 + DPMR_HI_SYMBOLS
                             + DPMR_SYNC_SYMBOLS]);

    to_levels(dibits, DPMR_FRAME_SYMBOLS, levels);
}

void dpmrTx_frame(dpmr_tx_t *tx,
                  const uint8_t ambe[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES],
                  int8_t levels[DPMR_FRAME_SYMBOLS])
{
    uint8_t dibits[DPMR_FRAME_SYMBOLS];

    memset(dibits, 0, sizeof(dibits));

    /*
     * Frames 1 and 3 of the superframe open with FS2 and frames 2 and 4 with
     * the channel code (clause 5.1 and 6.1), the fn field saying which.
     */
    if ((tx->fn & 1) == 0) {
        int8_t sym[DPMR_SYNC_SYMBOLS];
        size_t n = dpmrDefs_syncLevels(DPMR_SYNC_FS2, sym);

        for (size_t i = 0; i < n; i++) {
            const int8_t l = sym[i];

            dibits[i] = (uint8_t)(((l < 0) ? 2 : 0)
                                  | ((l == 3 || l == -3) ? 1 : 0));
        }
    } else {
        channelCodeDibits(tx->channelCode, dibits);
    }

    /* CCH: the address slices of the frame number, clause 9.1 */
    uint16_t id12 = 0;

    switch (tx->fn) {
    case 0: id12 = (uint16_t)((tx->calledId >> 12) & 0xFFFu); break;
    case 1: id12 = (uint16_t)(tx->calledId & 0xFFFu); break;
    case 2: id12 = (uint16_t)((tx->ownId >> 12) & 0xFFFu); break;
    default: id12 = (uint16_t)(tx->ownId & 0xFFFu); break;
    }

    dpmrFec_cchEncode((uint8_t)tx->fn, id12, tx->mode, tx->version,
                      tx->format, 0, &dibits[DPMR_SYNC_SYMBOLS]);

    /* Four AMBE+2 frames, one per 36-dibit group of the TCH */
    for (int f = 0; f < DPMR_AMBE_FRAMES; f++) {
        static const uint8_t zero[DPMR_AMBE_BYTES] = { 0 };
        uint8_t *tch = &dibits[DPMR_SYNC_SYMBOLS + DPMR_CCH_SYMBOLS + 36 * f];

        dpmrFec_voiceFrameEncode(ambe ? ambe[f] : zero, tch);
    }

    tx->fn = (tx->fn + 1) & 3;

    to_levels(dibits, DPMR_FRAME_SYMBOLS, levels);
}

void dpmrTx_end(dpmr_tx_t *tx, int status, int8_t levels[DPMR_TX_END_SYMBOLS])
{
    uint8_t dibits[DPMR_TX_END_SYMBOLS];

    memset(dibits, 0, sizeof(dibits));

    /* FS3, 24 bits, 12 symbols */
    {
        int8_t sym[DPMR_SYNC_SYMBOLS];
        size_t n = dpmrDefs_syncLevels(DPMR_SYNC_FS3, sym);

        for (size_t i = 0; i < n; i++) {
            const int8_t l = sym[i];

            dibits[i] = (uint8_t)(((l < 0) ? 2 : 0)
                                  | ((l == 3 || l == -3) ? 1 : 0));
        }
    }

    dpmrFec_endEncode((status >= 0) ? 1u : 0u, 0, 0,
                      (status >= 0) ? (uint8_t)status : 0u,
                      &dibits[DPMR_SYNC_SYMBOLS]);

    to_levels(dibits, DPMR_TX_END_SYMBOLS, levels);
    (void)tx;
}
