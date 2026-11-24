/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file txmodel.c
 * @brief Reverse of the receive chain, for the host harness only.
 *
 * No off-air capture carries a System Fusion text message, so the text path is
 * exercised the other way round: this builds a V/D mode 2 frame with the
 * published encoder (CRC-16, Golay(24,12), rate-1/2 K=5, dibit interleave,
 * PN(9,5) whitening) and the harness decodes it through the same
 * ysfFrame_decode() the radio runs.  Everything except the modem is real.
 */

#include <string.h>

#include "protocols/YSF/YsfFrame.h"
#include "txmodel.h"

static const uint8_t syncDibits[20] = {
    3, 1, 1, 0, 1, 3, 0, 1, 3, 0, 2, 1, 1, 2, 0, 3, 1, 0, 3, 1
};

static const uint8_t parity32[32] = {
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

static uint16_t crc16(const uint8_t *bits, int len)
{
    uint32_t crc = 0;

    for (int i = 0; i < len; i++)
    {
        crc = ((crc << 1) | (bits[i] & 1u)) & 0x1FFFFu;
        if (crc & 0x10000u)
            crc = (crc & 0xFFFFu) ^ 0x1021u;
    }

    return (uint16_t)((crc ^ 0xFFFFu) & 0xFFFFu);
}

/** Systematic extended Golay(24,12): 12 data, 11 remainder, 1 parity. */
static uint32_t golay24(uint32_t m)
{
    uint32_t cw = (m & 0xFFFu) << 11;

    for (int i = 22; i >= 11; i--)
        if (cw & (1u << i))
            cw ^= 0xC75u << (i - 11);

    uint32_t word = ((m & 0xFFFu) << 11) | (cw & 0x7FFu);
    int ones = 0;
    for (uint32_t v = word; v != 0; v >>= 1)
        ones += (int)(v & 1u);

    return (word << 1) | (uint32_t)(ones & 1);
}

/** Rate 1/2 K=5: 100 input bits -> 100 dibits. */
static void conv_encode(const uint8_t *in, int steps, uint8_t *out)
{
    int reg = 0;

    for (int i = 0; i < steps; i++)
    {
        reg = ((reg << 1) | (in[i] & 1)) & 0x1F;
        out[i] = (uint8_t)((parity32[reg & 0x19] << 1) | parity32[reg & 0x17]);
    }
}

/** Write rows, read columns: out[i + j * rows] = in[j + i * cols]. */
static void interleave(const uint8_t *in, int rows, int cols, uint8_t *out)
{
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
            out[i + j * rows] = in[j + i * cols];
}

static void whiten(uint8_t *bits, int n)
{
    uint16_t reg = 0x1C9;

    for (int i = 0; i < n; i++)
    {
        uint8_t bit = (uint8_t)(reg & 1u);
        uint8_t feedback = (uint8_t)(((reg >> 4) ^ reg) & 1u);

        reg = (uint16_t)((reg >> 1) | ((uint16_t)feedback << 8));
        bits[i] ^= bit;
    }
}

static void put(uint8_t *bits, int offset, int length, uint32_t value)
{
    for (int i = 0; i < length; i++)
        bits[offset + i] = (uint8_t)((value >> (length - 1 - i)) & 1u);
}

/** FICH: 32 information bits in, 100 interleaved dibits out. */
static void build_fich(uint8_t bn, uint8_t fn, uint8_t ft, uint8_t *dibits)
{
    uint8_t info[48];
    uint8_t coded[96];
    uint8_t payload[100];
    uint8_t air[100];

    memset(info, 0, sizeof(info));
    put(info, 0, 2, 1);    /* fi: communication */
    put(info, 2, 2, 2);    /* cs: callsigns assigned */
    put(info, 4, 2, 0);    /* cm: group */
    put(info, 6, 2, bn);   /* bn: block number */
    put(info, 8, 2, 0);    /* bt */
    put(info, 10, 3, fn);  /* fn */
    put(info, 13, 3, ft);  /* ft */
    put(info, 18, 3, 1);   /* mr */
    put(info, 21, 1, 1);   /* vp: repeater */
    put(info, 22, 2, 2);   /* dt: V/D mode 2 */
    put(info, 24, 1, 1);   /* st: no squelch code */
    put(info, 25, 7, 0);   /* sc */

    uint16_t crc = crc16(info, 48); /* over the 32 bits and 16 zeros */

    put(info, 32, 16, crc);

    for (int g = 0; g < 4; g++)
    {
        uint32_t m = 0;
        for (int i = 0; i < 12; i++)
            m = (m << 1) | (info[g * 12 + i] & 1u);

        uint32_t w = golay24(m);
        for (int i = 0; i < 24; i++)
            coded[g * 24 + i] = (uint8_t)((w >> (23 - i)) & 1u);
    }

    for (int i = 0; i < 96; i++)
        payload[i] = coded[i];
    for (int i = 96; i < 100; i++)
        payload[i] = 0;

    conv_encode(payload, 100, air);
    interleave(air, 20, 5, dibits);
}

/** Data channel: 10 characters in, 100 interleaved dibits out. */
static void build_dch(const char *text10, uint8_t *dibits)
{
    uint8_t info[96];
    uint8_t payload[100];
    uint8_t air[100];

    memset(info, 0, sizeof(info));

    for (int i = 0; i < 10; i++)
        for (int b = 0; b < 8; b++)
            info[i * 8 + b] = (uint8_t)(((uint8_t)text10[i] >> (7 - b)) & 1u);

    whiten(info, 80);

    uint16_t crc = crc16(info, 96);
    put(info, 80, 16, crc);

    for (int i = 0; i < 96; i++)
        payload[i] = info[i];
    for (int i = 96; i < 100; i++)
        payload[i] = 0;

    conv_encode(payload, 100, air);
    interleave(air, 20, 5, dibits);
}

void txmodel_vd2_text(uint8_t fn, uint8_t ft, const char *text10,
                      struct ysfFrame *frame)
{
    uint8_t fich[100];
    uint8_t dch[100];

    memset(frame, 0, sizeof(*frame));

    memcpy(frame->dibits, syncDibits, sizeof(syncDibits));
    build_fich(2, fn, ft, fich);
    memcpy(&frame->dibits[20], fich, 100);
    build_dch(text10, dch);

    for (int s = 0; s < 5; s++)
    {
        memcpy(&frame->dibits[120 + s * 72], &dch[s * 20], 20);
        /* Voice slots are left as zeros: the fixed bit fails and no slot is
         * produced, which is the behaviour of a corrupt voice block. */
    }
}
