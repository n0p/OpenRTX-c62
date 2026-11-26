/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file NxdnTx.c
 * @brief NXDN48 transmit, see NxdnTx.h.
 */

#include <string.h>

#include "protocols/NXDN/NxdnTx.h"

/** The 182 payload dibits: LICH, SACCH, four VCHs. */
#define PAYLOAD_DIBITS (NXDN_LICH_DIBITS + NXDN_SACCH_DIBITS \
                        + NXDN_VCH_PER_FRAME * NXDN_VCH_DIBITS)

/** Preamble word of 4.4.3, 24 bits, and the pattern that follows it. */
#define NXDN_PREAMBLE_BITS 0x5775FDUL

void nxdnTx_setup(nxdn_tx_t *tx, uint16_t src, uint16_t dst, uint8_t ran,
                  bool group)
{
    memset(tx, 0, sizeof(*tx));
    tx->src = src;
    tx->dst = dst;
    tx->ran = ran & 0x3Fu;
    tx->scrambler = true;
    tx->first = true;

    /* VCALL (TS 1-C): type, call control option, call type, source, dest */
    uint8_t octets[9] = {
        NXDN_MSG_VCALL, 0x00, 0x00,
        (uint8_t)(src >> 8), (uint8_t)src,
        (uint8_t)(dst >> 8), (uint8_t)dst,
        0x00, 0x00,
    };
    /* Call type: 3 bits at bit offset 16, 0 = group, 3 = individual */
    octets[2] = group ? 0x00u : 0x60u;
    nxdnTx_setMessage(tx, octets);
}

void nxdnTx_setMessage(nxdn_tx_t *tx, const uint8_t octets[9])
{
    for (int i = 0; i < 72; i++)
        tx->l3[i] = (uint8_t)((octets[i >> 3] >> (7 - (i & 7))) & 1u);
    tx->seg = 0;
}

void nxdnTx_release(nxdn_tx_t *tx)
{
    const uint8_t octets[9] = {
        NXDN_MSG_TX_REL, 0x00, 0x00,
        (uint8_t)(tx->src >> 8), (uint8_t)tx->src,
        (uint8_t)(tx->dst >> 8), (uint8_t)tx->dst,
        0x00, 0x00,
    };
    nxdnTx_setMessage(tx, octets);
    tx->last = true;
}

/**
 * Fill the 26 information bits of the SACCH: SR (RAN and segment index) and
 * the 18 bits of this frame's segment of the layer 3 message.
 */
static void sacch_info(nxdn_tx_t *tx, uint8_t info[26])
{
    /*
     * The receiver reads the segment as part = 3 - ((SR >> 6) & 3), so the
     * transmitter writes 3 - segment there.  Bits 5:0 are the RAN.
     */
    const uint8_t sr = (uint8_t)((((3 - tx->seg) & 3) << 6) | (tx->ran & 0x3Fu));

    for (int i = 0; i < 8; i++)
        info[i] = (uint8_t)((sr >> (7 - i)) & 1u);
    for (int i = 0; i < 18; i++)
        info[8 + i] = tx->l3[18 * tx->seg + i];

    tx->seg = (tx->seg + 1) & 3;
}

void nxdnTx_frame(nxdn_tx_t *tx,
                  const uint8_t ambe[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES],
                  int8_t levels[NXDN_FRAME_SYMBOLS])
{
    uint8_t dibits[PAYLOAD_DIBITS];
    uint8_t info[26];
    uint8_t bits[NXDN_SACCH_BITS];

    memset(dibits, 0, sizeof(dibits));
    memset(info, 0, sizeof(info));

    /*
     * LICH: RDCH (a conventional/direct traffic channel), the SACCH of a
     * superframe -- or the non-superframe structure on the first and last
     * frames of a call --, no VCH pair stolen, inbound direction.
     */
    const uint8_t usc = (tx->first || tx->last) ? NXDN_USC_SACCH_NS
                                                : NXDN_USC_SACCH_SF;
    const uint8_t control = (uint8_t)((NXDN_RF_RDCH << 5) | (usc << 3)
                                      | (NXDN_STEAL_NONE << 1) | 0u);
    nxdnFecLichEncode(control, dibits);

    /* SACCH: this frame's segment of the layer 3 message */
    sacch_info(tx, info);
    nxdnFecSacchEncode(info, bits);
    for (int i = 0; i < NXDN_SACCH_BITS; i++) {
        uint8_t *d = &dibits[NXDN_OFF_SACCH + (i >> 1)];

        if ((i & 1) == 0)
            *d = (uint8_t)((*d & 0x01u) | (uint8_t)(bits[i] << 1));
        else
            *d = (uint8_t)((*d & 0x02u) | bits[i]);
    }

    /* The four VCHs, one AMBE+2 frame each */
    for (int v = 0; v < NXDN_VCH_PER_FRAME; v++) {
        static const uint8_t zero[NXDN_AMBE_BYTES] = { 0 };

        nxdnFecVchFromAmbe(ambe ? ambe[v] : zero,
                           &dibits[NXDN_OFF_VCH1 + v * NXDN_VCH_DIBITS]);
    }

    if (tx->scrambler)
        nxdnFecScramble(dibits, false);

    /* FSW as it is, then the payload through the dibit table */
    memcpy(levels, nxdnFsw, NXDN_FSW_SYMBOLS);
    for (int i = 0; i < PAYLOAD_DIBITS; i++)
        levels[NXDN_FSW_SYMBOLS + i] = nxdnTxDibitLevel(dibits[i]);

    tx->first = false;
    tx->frames++;
}

void nxdnTx_preamble(int8_t levels[NXDN_FRAME_SYMBOLS])
{
    for (int s = 0; s < NXDN_FRAME_SYMBOLS; s++) {
        if (s < 12) {
            /* 0x5775FD, two bits per symbol, MSB first */
            const uint8_t dibit = (uint8_t)((NXDN_PREAMBLE_BITS
                                             >> (22 - 2 * s)) & 3u);

            levels[s] = nxdnTxDibitLevel(dibit);
        } else {
            /* +3 +3 -3 -3, the pattern clause 4.4.3 puts after the word */
            levels[s] = ((s & 2u) == 0u) ? 3 : -3;
        }
    }
}

int8_t nxdnTxDibitLevel(uint8_t dibit)
{
    /* NxdnPhy.c level_to_dibit(): sign bit high, magnitude bit is the outer
     * level.  01 = -3, 00 = -1, 11 = +3, 10 = +1. */
    static const int8_t level[4] = { -1, -3, +1, +3 };

    return level[dibit & 3u];
}
