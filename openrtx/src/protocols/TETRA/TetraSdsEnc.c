/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file TetraSdsEnc.c
 * @brief TETRA uplink short data encoder, see TetraSdsEnc.h.  Ported from the
 * host prototype work/tetra-sds/src/sds_enc.c, where libtetra's own uplink
 * receiver decodes what this produces back to the same text.
 */

#include <string.h>

#include "protocols/TETRA/TetraSdsEnc.h"

/**
 * Append the n least significant bits of v, most significant first.
 *
 * The writer is a fixed 400 octet buffer, so every bit is checked against it:
 * the callers of this file used to bound the text themselves and one of them
 * (the UCS-2 branch of sds_tx_text_payload) did not, which turned a long
 * message into a write past the end of a stack object.
 */
static void put(struct sds_tx *t, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned bit = (v >> (n - 1 - i)) & 1;
        unsigned pos = t->nbits;

        if (pos >= 8u * sizeof(t->buf)) {
            t->overflow = true;
            return;
        }

        t->nbits = pos + 1;

        if (bit)
            t->buf[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
    }
}

/*
 * GSM 03.38 default alphabet: ASCII coincides with the septet values from
 * 0x20 to 0x3f, 0x41 to 0x5a and 0x61 to 0x7a, except for '$', '@' and '_'.
 * The characters that need the escape are returned as 0x1b with *ext set to
 * the extension code.
 */
static int gsm7_septet(unsigned char c, int *ext)
{
    *ext = -1;

    switch (c) {
    case '@': return 0x00;
    case '$': return 0x02;
    case '_': return 0x11;
    case '[': *ext = 0x3c; return 0x1b;
    case '\\': *ext = 0x2f; return 0x1b;
    case ']': *ext = 0x3e; return 0x1b;
    case '^': *ext = 0x14; return 0x1b;
    case '{': *ext = 0x28; return 0x1b;
    case '|': *ext = 0x40; return 0x1b;
    case '}': *ext = 0x29; return 0x1b;
    case '~': *ext = 0x3d; return 0x1b;
    /* Latin-1 letters that have a position in the default alphabet. */
    case 0xc4: return 0x5b; /* Ä */
    case 0xd6: return 0x5c; /* Ö */
    case 0xd1: return 0x5d; /* Ñ */
    case 0xdc: return 0x5e; /* Ü */
    case 0xa7: return 0x5f; /* § */
    case 0xe4: return 0x7b; /* ä */
    case 0xf6: return 0x7c; /* ö */
    case 0xf1: return 0x7d; /* ñ */
    case 0xfc: return 0x7e; /* ü */
    case 0xe0: return 0x7f; /* à */
    case 0xe9: return 0x05; /* é */
    case 0xe8: return 0x04; /* è */
    case 0xdf: return 0x1e; /* ß */
    case 0xc9: return 0x1f; /* É */
    default:
        break;
    }

    if ((c >= 0x20 && c <= 0x3f) || (c >= 0x41 && c <= 0x5a) ||
        (c >= 0x61 && c <= 0x7a))
        return (int)c;
    return -1;
}

int sds_tx_text_payload(struct sds_tx *t, const char *text, uint8_t coding,
                        bool sdstl, bool immediate, uint8_t ref)
{
    const unsigned len = (unsigned)strlen(text);
    const unsigned capacity = 8u * (unsigned)sizeof(t->buf);

    if (sdstl)
        put(t, immediate ? TETRA_PID_IMM_TEXT : TETRA_PID_TEXT, 8);
    else
        put(t, immediate ? TETRA_PID_SIMPLE_IMM_TEXT : TETRA_PID_SIMPLE_TEXT,
            8);

    if (sdstl) {
        put(t, 0, 4);   /* SDS-TRANSFER */
        put(t, 0, 2);   /* no delivery report */
        put(t, 0, 1);   /* no storage / forward control */
        put(t, 0, 1);
        put(t, ref, 8); /* message reference */
        put(t, 0, 1);   /* no time stamp */
        put(t, coding, 7);
    } else {
        put(t, 0, 1);
        put(t, coding, 7);
    }

    if (t->overflow)
        return -1;

    if (coding == TETRA_TCS_GSM7) {
        /*
         * GSM 03.38 packing, without the septet and octet arrays it used to
         * need (2 KiB, on the stack of a receiver whose DTCM is all but
         * full).  The first pass only measures and validates the alphabet;
         * the second one packs straight into the writer, seven bits at a
         * time through a small accumulator.  Octet j carries septet bits
         * 8j..8j+7, least significant bit first inside the septet, and the
         * octets go out most significant bit first like every other field.
         */
        unsigned nsept = 0;

        for (unsigned i = 0; i < len; i++) {
            int ext;
            int v = gsm7_septet((unsigned char)text[i], &ext);

            if (v < 0)
                return -1; /* not in the alphabet: send it as UCS-2 */
            nsept += (ext >= 0) ? 2u : 1u;
        }

        /* 7 * nsept cannot wrap: nsept is at most twice strlen(text) */
        const unsigned octets = (7u * nsept + 7u) / 8u;

        if ((t->nbits + 8u * octets) > capacity)
            return -1;

        uint32_t acc = 0;    /* pending septet bits, least significant first */
        unsigned accBits = 0;

        for (unsigned i = 0; i < len; i++) {
            int ext;
            int v = gsm7_septet((unsigned char)text[i], &ext);

            acc |= (uint32_t)v << accBits;
            accBits += 7;

            while (accBits >= 8) {
                put(t, acc & 0xffu, 8);
                acc >>= 8;
                accBits -= 8;
            }

            if (ext >= 0) {
                acc |= (uint32_t)ext << accBits;
                accBits += 7;

                while (accBits >= 8) {
                    put(t, acc & 0xffu, 8);
                    acc >>= 8;
                    accBits -= 8;
                }
            }
        }

        if (accBits > 0)
            put(t, acc & 0xffu, 8);
    } else if (coding == TETRA_TCS_UCS2) {
        if ((t->nbits + 16u * len) > capacity)
            return -1;
        for (unsigned i = 0; i < len; i++)
            put(t, (unsigned char)text[i], 16);
    } else {
        if ((t->nbits + 8u * len) > capacity)
            return -1;
        for (unsigned i = 0; i < len; i++)
            put(t, (unsigned char)text[i], 8);
    }
    return t->overflow ? -1 : 0;
}

int sds_tx_u_sds_data(struct sds_tx *t, uint32_t dst_ssi, uint8_t area,
                      const uint8_t *payload, unsigned payload_bits)
{
    if (payload_bits > 0x7ff)
        return -1;

    memset(t, 0, sizeof(*t));
    put(t, SDS_ENC_U_SDS_DATA, 5);
    put(t, area & 0xf, 4); /* area selection, 14.8.1 */
    put(t, 1, 2);          /* called party: SSI */
    put(t, dst_ssi, 24);
    put(t, 3, 2);          /* SDTI: UDD4 */
    put(t, payload_bits, 11);
    for (unsigned i = 0; i < payload_bits; i++) {
        unsigned bit = (payload[i >> 3] >> (7 - (i & 7))) & 1;

        put(t, bit, 1);
    }
    return t->overflow ? -1 : 0;
}

int sds_tx_u_status(struct sds_tx *t, uint32_t dst_ssi, uint8_t area,
                    uint16_t status)
{
    memset(t, 0, sizeof(*t));
    put(t, SDS_ENC_U_STATUS, 5);
    put(t, area & 0xf, 4);
    put(t, 1, 2);
    put(t, dst_ssi, 24);
    put(t, status, 16);
    return t->overflow ? -1 : 0;
}

int sds_tx_wrap_llc(struct sds_tx *t, const struct sds_tx *cmce)
{
    struct sds_tx out;

    memset(&out, 0, sizeof(out));
    put(&out, 2, 4); /* LLC BL-UDATA: unacknowledged */
    put(&out, 2, 3); /* MLE: CMCE */
    for (unsigned i = 0; i < cmce->nbits; i++) {
        unsigned bit = (cmce->buf[i >> 3] >> (7 - (i & 7))) & 1;

        put(&out, bit, 1);
    }
    *t = out;
    return out.overflow ? -1 : 0;
}

/* Append a '1' fill bit then zeros up to nbits (MAC fill). */
static void fill_to(uint8_t *buf, unsigned *n, unsigned nbits)
{
    if (*n < nbits) {
        buf[*n >> 3] |= (uint8_t)(0x80 >> (*n & 7));
        *n += 1;
    }
    *n = nbits;
}

int sds_tx_mac_access(uint8_t out[92], uint32_t ssi, const struct sds_tx *sdu)
{
    struct sds_tx m;
    unsigned len = 1 + 1 + 1 + 2 + 24 + 1 + sdu->nbits;

    memset(&m, 0, sizeof(m));
    if (len >= 92)
        return -1;
    put(&m, 0, 1); /* MAC-ACCESS */
    put(&m, 1, 1); /* fill bits */
    put(&m, 0, 1); /* no optional field */
    put(&m, 0, 2); /* address type: SSI */
    put(&m, ssi, 24);
    put(&m, 0, 1); /* no optional element */
    for (unsigned i = 0; i < sdu->nbits; i++) {
        unsigned bit = (sdu->buf[i >> 3] >> (7 - (i & 7))) & 1;

        put(&m, bit, 1);
    }
    if (m.overflow)
        return -1;
    fill_to(m.buf, &m.nbits, 92);
    for (unsigned i = 0; i < 92; i++)
        out[i] = (uint8_t)((m.buf[i >> 3] >> (7 - (i & 7))) & 1);
    return 0;
}

int sds_tx_mac_data(uint8_t out[268], uint32_t ssi, const struct sds_tx *sdu)
{
    struct sds_tx m;
    unsigned body = 2 + 1 + 1 + 2 + 24 + 1 + 6 + sdu->nbits;
    unsigned li = (body + 7) / 8;
    bool fill = li * 8 != body;

    memset(&m, 0, sizeof(m));
    put(&m, 0, 2);    /* MAC-DATA */
    put(&m, fill, 1); /* fill bits */
    put(&m, 0, 1);    /* no optional field */
    put(&m, 0, 2);    /* address type: SSI */
    put(&m, ssi, 24);
    put(&m, 0, 1);    /* no optional element */
    put(&m, li, 6);   /* length indication, octets */
    for (unsigned i = 0; i < sdu->nbits; i++) {
        unsigned bit = (sdu->buf[i >> 3] >> (7 - (i & 7))) & 1;

        put(&m, bit, 1);
    }
    /*
     * A TM-SDU longer than the block can carry is refused here instead of
     * being truncated by fill_to() below, which rewinds nbits and would hand
     * out a block whose tail is whatever the writer held.
     */
    if (m.overflow || (body > 268u))
        return -1;
    fill_to(m.buf, &m.nbits, li * 8);
    fill_to(m.buf, &m.nbits, 268);
    for (unsigned i = 0; i < 268; i++)
        out[i] = (uint8_t)((m.buf[i >> 3] >> (7 - (i & 7))) & 1);
    return 0;
}

int tetraSds_encodeText(struct sds_tx *out, uint32_t dst_ssi, const char *text,
                        bool sdstl)
{
    struct sds_tx payload;

    if ((out == NULL) || (text == NULL))
        return -1;

    /*
     * GSM 7-bit first: it is what TETRA radios expect and it is three times
     * shorter than UCS-2.  A character outside the default alphabet is not an
     * error, it just means the message goes as UCS-2.
     */
    memset(&payload, 0, sizeof(payload));
    if (sds_tx_text_payload(&payload, text, TETRA_TCS_GSM7, sdstl, false, 1)
        != 0) {
        memset(&payload, 0, sizeof(payload));
        if (sds_tx_text_payload(&payload, text, TETRA_TCS_UCS2, sdstl, false,
                                1) != 0)
            return -1;
    }

    return sds_tx_u_sds_data(out, dst_ssi, SDS_ENC_AREA_DEFAULT, payload.buf,
                             payload.nbits);
}
