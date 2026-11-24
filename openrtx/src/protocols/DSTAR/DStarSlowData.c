/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DStarSlowData.c
 * @brief The D-STAR slow-data channel, see DStarSlowData.h.
 *
 * Only the framing is standardised (24 bits per frame, a resync word every
 * 21st frame); the block layout used here is the one every Icom radio emits and
 * every D-STAR decoder expects, as documented by G4KLX.
 *
 * Parsing is integer-only: no libm, no strtod, no sscanf.  A sentence is
 * anything the slow data delivered between a '$' and the newline that follows
 * it, and it is only trusted when its checksum (NMEA) or CRC (D-PRS) checks
 * out.
 */

#include "protocols/DSTAR/DStarSlowData.h"

#include <string.h>

void dstarSlow_reset(struct dstarSlow *sd)
{
    memset(sd, 0, sizeof(*sd));
}

uint8_t dstarNmeaChecksum(const char *body, int len)
{
    uint8_t x = 0;
    for (int i = 0; i < len; i++)
        x ^= (uint8_t)body[i];
    return x;
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* ddmm.mmmm + hemisphere -> degrees * 1e6, integer only. */
static bool parseCoord(const char *v, int len, char hemi, int degDigits, int32_t *out)
{
    int32_t deg = 0;
    int i = 0;

    if (len < degDigits + 3)
        return false;
    for (; i < degDigits; i++) {
        if (v[i] < '0' || v[i] > '9')
            return false;
        deg = deg * 10 + (v[i] - '0');
    }
    /* minutes, up to 4 decimals: scale to 1e6 degrees */
    int32_t minInt = 0;
    int32_t minFrac = 0;
    int fracDigits = 0;
    for (; i < len; i++) {
        if (v[i] == '.')
            break;
        if (v[i] < '0' || v[i] > '9')
            return false;
        minInt = minInt * 10 + (v[i] - '0');
    }
    if (i < len && v[i] == '.') {
        i++;
        for (; i < len; i++) {
            if (v[i] < '0' || v[i] > '9')
                return false;
            if (fracDigits < 6) {
                minFrac = minFrac * 10 + (v[i] - '0');
                fracDigits++;
            }
        }
    }
    if (minInt >= 60)
        return false;
    while (fracDigits < 6) {
        minFrac *= 10;
        fracDigits++;
    }

    /* value = deg + minutes/60, all in 1e-6 degrees */
    int32_t val = deg * 1000000 + (minInt * 1000000 + minFrac) / 60;
    if (hemi == 'S' || hemi == 'W')
        val = -val;
    else if (hemi != 'N' && hemi != 'E')
        return false;
    *out = val;
    return true;
}

/* Decimal text (possibly negative, at most one dot) to a scaled integer. */
static int32_t parseNum(const char *v, int len, int scale)
{
    int32_t v0 = 0;
    int sign = 1;
    int i = 0;
    int frac = 0;
    int scaleLeft = scale;

    if (len <= 0)
        return 0;
    if (v[0] == '-') {
        sign = -1;
        i = 1;
    }
    for (; i < len; i++) {
        if (v[i] == '.') {
            i++;
            break;
        }
        if (v[i] < '0' || v[i] > '9')
            break;
        v0 = v0 * 10 + (v[i] - '0');
    }
    for (; i < len && scaleLeft > 1; i++) {
        if (v[i] < '0' || v[i] > '9')
            break;
        frac = frac * 10 + (v[i] - '0');
        scaleLeft /= 10;
    }
    while (scaleLeft > 1) {
        frac *= 10;
        scaleLeft /= 10;
    }
    return sign * (v0 * scale + frac);
}

/* Index of the n-th comma separated field inside [s, s+len). */
static bool fieldAt(const char *s, int len, int idx, int *start, int *flen)
{
    int f = 0;
    int a = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || s[i] == ',') {
            if (f == idx) {
                *start = a;
                *flen = i - a;
                return true;
            }
            f++;
            a = i + 1;
        }
    }
    return false;
}

static bool startsWith(const char *s, int len, const char *p)
{
    int n = (int)strlen(p);
    return len >= n && memcmp(s, p, (size_t)n) == 0;
}

/* NMEA 0183: "$GPGGA,..." with an "*hh" checksum. */
static bool parseNmea(const char *s, int len, struct dstarFix *fix)
{
    int star = -1;
    for (int i = 0; i < len; i++) {
        if (s[i] == '*') {
            star = i;
            break;
        }
    }
    if (s[0] != '$' || star < 0 || star + 2 >= len)
        return false;

    uint8_t want = (uint8_t)((hexVal(s[star + 1]) << 4) | hexVal(s[star + 2]));
    if (dstarNmeaChecksum(s + 1, star - 1) != want)
        return false;

    const char *body = s + 1;
    int blen = star - 1;
    int st, fl;
    if (!fieldAt(body, blen, 0, &st, &fl) || fl != 5)
        return false;

    const char *type = body + st + 2; /* skip the talker id (GP, GN, GL...) */

    if (startsWith(type, blen - st - 2, "GGA")) {
        /* time, lat, N/S, lon, E/W, fix quality, sats, hdop, alt, M */
        int s1, l1, s2, l2, s3, l3, s4, l4, s5, l5, s6, l6;
        if (!fieldAt(body, blen, 2, &s1, &l1) || !fieldAt(body, blen, 3, &s2, &l2) ||
            !fieldAt(body, blen, 4, &s3, &l3) || !fieldAt(body, blen, 5, &s4, &l4) ||
            !fieldAt(body, blen, 6, &s6, &l6))
            return false;
        if (l6 == 0 || (l6 == 1 && body[s6] == '0'))
            return false; /* fix quality 0: no fix */
        if (!parseCoord(body + s1, l1, body[s2], 2, &fix->lat))
            return false;
        if (!parseCoord(body + s3, l3, body[s4], 3, &fix->lon))
            return false;
        if (fieldAt(body, blen, 9, &s5, &l5) && l5 > 0)
            fix->alt = parseNum(body + s5, l5, 1);
        fix->valid = true;
        return true;
    }

    if (startsWith(type, blen - st - 2, "RMC")) {
        /* time, A/V, lat, N/S, lon, E/W, speed kn, course, date */
        int s1, l1, s2, l2, s3, l3, s4, l4, s5, l5, s6, l6, s7, l7;
        if (!fieldAt(body, blen, 2, &s1, &l1) || !fieldAt(body, blen, 3, &s2, &l2) ||
            !fieldAt(body, blen, 4, &s3, &l3) || !fieldAt(body, blen, 5, &s4, &l4) ||
            !fieldAt(body, blen, 6, &s5, &l5))
            return false;
        if (l1 < 1 || body[s1] != 'A')
            return false;
        if (!parseCoord(body + s2, l2, body[s3], 2, &fix->lat))
            return false;
        if (!parseCoord(body + s4, l4, body[s5], 3, &fix->lon))
            return false;
        if (fieldAt(body, blen, 7, &s6, &l6) && l6 > 0) {
            /* knots * 1.852 -> km/h, in 1/10 km/h */
            int32_t kn = parseNum(body + s6, l6, 10);
            fix->speed = (kn * 1852) / 10000;
        }
        if (fieldAt(body, blen, 8, &s7, &l7) && l7 > 0)
            fix->course = (uint16_t)parseNum(body + s7, l7, 1);
        fix->valid = true;
        return true;
    }

    return false;
}

/* D-PRS: "$$CRCxxxx,CALL>API51,DSTAR*:!DDMM.mmN/DDDMM.mmE>comment" */
static uint16_t crcStream(const char *s, int len, bool withCr)
{
    uint16_t crc = 0xFFFF;

    for (int i = 0; i < len + (withCr ? 1 : 0); i++) {
        uint8_t c = (i < len) ? (uint8_t)s[i] : (uint8_t)'\r';
        crc ^= c;
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

static bool parseDprs(const char *s, int len, struct dstarFix *fix)
{
    if (!startsWith(s, len, "$$CRC") || len < 11 || s[9] != ',')
        return false;

    uint16_t want = 0;
    for (int i = 5; i < 9; i++) {
        int h = hexVal(s[i]);
        if (h < 0)
            return false;
        want = (uint16_t)((want << 4) | (uint16_t)h);
    }
    const char *rest = s + 10;
    int rlen = len - 10;
    if (crcStream(rest, rlen, false) != want && crcStream(rest, rlen, true) != want)
        return false;

    int colon = -1;
    for (int i = 0; i < rlen; i++) {
        if (rest[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon < 0 || colon + 21 > rlen)
        return false;

    const char *pos = rest + colon + 1;
    int plen = rlen - colon - 1;
    if (pos[0] != '!' && pos[0] != '=' && pos[0] != '/' && pos[0] != '@')
        return false;
    int off = (pos[0] == '/' || pos[0] == '@') ? 8 : 1; /* timestamp variants */
    if (plen < off + 19)
        return false;
    pos += off;

    if (!parseCoord(pos, 7, pos[7], 2, &fix->lat))
        return false;
    if (!parseCoord(pos + 9, 7, pos[16], 3, &fix->lon))
        return false;
    fix->fromAprs = true;
    fix->valid = true;
    return true;
}

bool dstarFix_parse(const char *sentence, struct dstarFix *fix)
{
    memset(fix, 0, sizeof(*fix));

    int len = (int)strlen(sentence);
    while (len > 0 && (sentence[len - 1] == '\r' || sentence[len - 1] == '\n' ||
                       sentence[len - 1] == ' '))
        len--;
    if (len < 10)
        return false;

    if (startsWith(sentence, len, "$$CRC"))
        return parseDprs(sentence, len, fix);
    if (sentence[0] == '$')
        return parseNmea(sentence, len, fix);
    return false;
}

/* ------------------------------------------------------------------ rx --- */

static void blockComplete(struct dstarSlow *sd)
{
    uint8_t type = (uint8_t)(sd->blk[0] & DSTAR_SLOW_TYPE_MASK);
    uint8_t len = (uint8_t)(sd->blk[0] & DSTAR_SLOW_LEN_MASK);
    uint8_t frag = (uint8_t)(sd->blk[0] & DSTAR_SLOW_LEN_MASK);
    const uint8_t *payload = sd->blk + 1;

    sd->lastType = sd->blk[0];
    sd->blocks++;

    if (type == DSTAR_SLOW_TEXT) {
        /* Type 0x4n: fragment n, always five characters, no length. */
        int base = (int)frag * 5;
        if (base + 5 <= DSTAR_TEXT_LEN) {
            for (int i = 0; i < 5; i++)
                sd->text[base + i] = (char)(payload[i] & 0x7F);
            sd->text[DSTAR_TEXT_LEN] = '\0';
            sd->textMask = (uint8_t)(sd->textMask | (1u << (frag & 3)));
        }
    } else if (type == DSTAR_SLOW_HEADER) {
        /* Type 0x5n: five bytes of the 41-byte radio header, 'f' padded. */
        if ((size_t)sd->hdrLen + 5u <= sizeof(sd->hdr)) {
            memcpy(sd->hdr + sd->hdrLen, payload, 5);
            sd->hdrLen = (uint8_t)(sd->hdrLen + 5);
        }
    } else if (type == DSTAR_SLOW_GPS) {
        /* Type 0x3n: five characters of an NMEA / D-PRS sentence. */
        if (len > 5)
            len = 5;
        if (payload[0] == '$' && sd->gpsLen > 0) {
            /* A new sentence started without a newline: close the old one by
             * appending a terminator, which dstarSlow_sentence() looks for. */
            sd->gps[sd->gpsLen++] = '\n';
        } else {
            for (int i = 0; i < len; i++) {
                if (payload[i] == DSTAR_SLOW_FILL)
                    break;
                if (sd->gpsLen < DSTAR_GPS_LEN)
                    sd->gps[sd->gpsLen++] = (char)payload[i];
            }
            sd->gps[sd->gpsLen] = '\0';
        }
    }
}

bool dstarSlow_push(struct dstarSlow *sd, const uint8_t slow[DSTAR_SLOW_BYTES])
{
    if (sd->half == 0) {
        memcpy(sd->blk, slow, 3);
        sd->half = 1;
        return false;
    }

    memcpy(sd->blk + 3, slow, 3);
    sd->half = 0;
    blockComplete(sd);
    sd->blk[0] = 0;
    return true;
}

const char *dstarSlow_text(struct dstarSlow *sd)
{
    if (sd->textMask != 0x0F)
        return NULL;
    sd->textMask = 0;
    return sd->text;
}

const char *dstarSlow_sentence(struct dstarSlow *sd)
{
    int n = sd->gpsLen;

    while (n > 0 && (sd->gps[n - 1] == '\r' || sd->gps[n - 1] == '\n' ||
                     sd->gps[n - 1] == ' '))
        n--;

    if (n >= 8) {
        /* A complete sentence is one that ends before the newline, or whose
         * buffer is full: hand it over and restart. */
        bool done = (n < sd->gpsLen) || (sd->gpsLen >= DSTAR_GPS_LEN);
        if (done) {
            sd->gps[n] = '\0';
            sd->gpsLen = 0;
            return sd->gps;
        }
    }
    return NULL;
}
