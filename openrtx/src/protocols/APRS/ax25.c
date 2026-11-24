/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/ax25.h"
#include <string.h>

/* Address field bit layout (AX.25 v2.2 §2.3, figure 2.2) */
#define ADDR_C_BIT      0x80u   /* command/response, not used by APRS */
#define ADDR_RESERVED   0x60u   /* always 1,1  */
#define ADDR_SSID_MASK  0x1Eu   /* SSID in bits 4..1, i.e. ssid << 1 */
#define ADDR_H_BIT      0x01u   /* last address of the field (end of address) */
#define ADDR_EXT_BIT    0x01u   /* same bit, per address: end of the address */

uint16_t ax25_fcs_raw(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0x8408u) : (crc >> 1);
    }

    return crc;
}

uint16_t ax25_fcs(const uint8_t *data, size_t len)
{
    return (uint16_t)(ax25_fcs_raw(data, len) ^ 0xFFFFu);
}

/*
 * A frame is good when the CRC of data plus the two FCS bytes, taken without
 * the final XOR, equals the AX.25 residue 0xF0B8.
 */
bool ax25_fcs_ok(const uint8_t *frame, size_t len)
{
    return ax25_fcs_raw(frame, len) == 0xF0B8u;
}

void ax25_addr_set(ax25_addr_t *a, const char *call)
{
    memset(a, 0, sizeof(*a));

    size_t n = 0;
    while ((call[n] != '\0') && (call[n] != '-') && (n < AX25_CALL_LEN))
    {
        char c = call[n];
        if ((c >= 'a') && (c <= 'z'))
            c = (char)(c - 'a' + 'A');
        a->call[n] = c;
        n++;
    }
    for (size_t i = n; i < AX25_CALL_LEN; i++)
        a->call[i] = ' ';

    if (call[n] == '-')
    {
        uint8_t ssid = 0;
        for (size_t i = n + 1; call[i] >= '0' && call[i] <= '9'; i++)
        {
            ssid = (uint8_t)(ssid * 10u + (uint8_t)(call[i] - '0'));
            if (ssid > 15)
                ssid = 15;
        }
        a->ssid = ssid & 0x0Fu;
    }
}

size_t ax25_addr_format(char *out, size_t cap, const ax25_addr_t *a)
{
    char buf[AX25_CALL_LEN + 4];
    size_t len = 0;

    for (size_t i = 0; i < AX25_CALL_LEN; i++)
    {
        if (a->call[i] == '\0' || a->call[i] == ' ')
            break;
        buf[len++] = a->call[i];
    }

    if (a->ssid != 0)
    {
        buf[len++] = '-';
        if (a->ssid >= 10)
            buf[len++] = (char)('0' + a->ssid / 10);
        buf[len++] = (char)('0' + a->ssid % 10);
    }

    if ((out != NULL) && (cap > 0))
    {
        size_t cp = (len < cap - 1) ? len : cap - 1;
        memcpy(out, buf, cp);
        out[cp] = '\0';
    }

    return len;
}

static void encodeAddr(uint8_t *out, const ax25_addr_t *a, bool last)
{
    for (size_t i = 0; i < AX25_CALL_LEN; i++)
    {
        char c = (a->call[i] == '\0') ? ' ' : a->call[i];
        out[i] = (uint8_t)((uint8_t)c << 1);
    }

    uint8_t ssid = (uint8_t)(ADDR_RESERVED | ((a->ssid & 0x0Fu) << 1));
    if (a->repeated)
        ssid |= ADDR_C_BIT;
    if (last)
        ssid |= ADDR_H_BIT;

    out[AX25_CALL_LEN] = ssid;
}

static void decodeAddr(const uint8_t *in, ax25_addr_t *a)
{
    memset(a, 0, sizeof(*a));

    for (size_t i = 0; i < AX25_CALL_LEN; i++)
        a->call[i] = (char)(in[i] >> 1);

    a->ssid     = (uint8_t)((in[AX25_CALL_LEN] & ADDR_SSID_MASK) >> 1);
    a->repeated = (in[AX25_CALL_LEN] & ADDR_C_BIT) != 0;
}

size_t ax25_build_ui(uint8_t *out, size_t cap, const ax25_frame_t *f)
{
    size_t len = AX25_ADDR_SIZE * (2u + f->nDigis) + 2u + f->infoLen + 2u;
    if (len > cap)
        return 0;

    size_t pos = 0;
    encodeAddr(&out[pos], &f->dst, false);
    pos += AX25_ADDR_SIZE;

    encodeAddr(&out[pos], &f->src, f->nDigis == 0);
    pos += AX25_ADDR_SIZE;

    for (uint8_t i = 0; i < f->nDigis; i++)
    {
        encodeAddr(&out[pos], &f->digi[i], i == (uint8_t)(f->nDigis - 1));
        pos += AX25_ADDR_SIZE;
    }

    out[pos++] = 0x03;   /* UI frame, poll bit clear */
    out[pos++] = 0xF0;   /* no layer 3 protocol */

    if (f->infoLen > 0)
    {
        memcpy(&out[pos], f->info, f->infoLen);
        pos += f->infoLen;
    }

    uint16_t fcs = ax25_fcs(out, pos);
    out[pos++] = (uint8_t)(fcs & 0xFFu);
    out[pos++] = (uint8_t)(fcs >> 8);

    return pos;
}

bool ax25_parse(uint8_t *frame, size_t len, ax25_frame_t *out)
{
    /* dst + src + control + pid */
    if (len < AX25_ADDR_SIZE * 2u + 2u)
        return false;

    size_t pos = 0;
    uint8_t addrCount = 0;

    memset(out, 0, sizeof(*out));

    while (true)
    {
        if ((pos + AX25_ADDR_SIZE) > len)
            return false;

        const uint8_t *a = &frame[pos];
        ax25_addr_t decoded;
        decodeAddr(a, &decoded);

        if (addrCount == 0)
            out->dst = decoded;
        else if (addrCount == 1)
            out->src = decoded;
        else if (out->nDigis < AX25_MAX_DIGIS)
            out->digi[out->nDigis++] = decoded;
        else
            return false;   /* more than 8 addresses: not AX.25 */

        addrCount++;
        pos += AX25_ADDR_SIZE;

        if ((a[AX25_CALL_LEN] & ADDR_EXT_BIT) != 0)
            break;          /* last address of the field */
    }

    if ((pos + 2u) > len)
        return false;

    out->control = frame[pos++];
    out->pid     = frame[pos++];

    if ((out->control & 0xEFu) != 0x03u)   /* UI, poll bit ignored */
        return false;

    out->info    = &frame[pos];
    out->infoLen = len - pos;

    return true;
}

uint8_t ax25_parse_path(const char *path, ax25_addr_t *digis, uint8_t max)
{
    uint8_t n = 0;
    char item[12];
    size_t len = 0;

    if (path == NULL)
        return 0;

    for (const char *p = path; ; p++)
    {
        if ((*p == ',') || (*p == '\0'))
        {
            item[len] = '\0';
            if ((len > 0) && (n < max))
                ax25_addr_set(&digis[n++], item);
            len = 0;
            if (*p == '\0')
                break;
        }
        else if (len < (sizeof(item) - 1))
        {
            item[len++] = *p;
        }
    }

    return n;
}
