/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/aprs.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define DEG2RAD 0.017453292519943295

static int fmtDouble(char *out, size_t cap, const char *fmt, double v)
{
    int n = snprintf(out, cap, fmt, v);
    return (n < 0) ? 0 : n;
}

/* ------------------------------------------------------------ encoding --- */

size_t aprs_build_position(char *out, size_t cap, const aprs_pos_t *p,
                           bool messaging)
{
    double alat = fabs(p->lat);
    double alon = fabs(p->lon);

    int latDeg = (int)alat;
    double latMin = (alat - latDeg) * 60.0;
    int lonDeg = (int)alon;
    double lonMin = (alon - lonDeg) * 60.0;

    char buf[96];
    int n = snprintf(buf, sizeof(buf), "%c%02d%05.2f%c%c%03d%05.2f%c%c%s",
                     messaging ? '=' : '!',
                     latDeg, latMin, (p->lat >= 0.0) ? 'N' : 'S',
                     p->table,
                     lonDeg, lonMin, (p->lon >= 0.0) ? 'E' : 'W',
                     p->symbol, p->comment);

    if ((n <= 0) || ((size_t)n >= cap))
        return 0;

    memcpy(out, buf, (size_t)n + 1u);
    return (size_t)n;
}

size_t aprs_build_position_compressed(char *out, size_t cap,
                                      const aprs_pos_t *p, bool messaging)
{
    if (cap < 14)
        return 0;

    /* Base-91: 91^4 = 68574961 > 380926 * 180 and > 190463 * 360 */
    uint32_t y = (uint32_t)(380926.0 * (90.0 - p->lat));
    uint32_t x = (uint32_t)(190463.0 * (180.0 + p->lon));

    if (y > 68574960u) y = 68574960u;
    if (x > 68574960u) x = 68574960u;

    size_t n = 0;
    out[n++] = messaging ? '=' : '!';
    out[n++] = p->table;

    out[n++] = (char)(33u + ((y / 753571u) % 91u));
    out[n++] = (char)(33u + ((y / 8281u) % 91u));
    out[n++] = (char)(33u + ((y / 91u) % 91u));
    out[n++] = (char)(33u + (y % 91u));

    out[n++] = (char)(33u + ((x / 753571u) % 91u));
    out[n++] = (char)(33u + ((x / 8281u) % 91u));
    out[n++] = (char)(33u + ((x / 91u) % 91u));
    out[n++] = (char)(33u + (x % 91u));

    out[n++] = p->symbol;

    size_t clen = strlen(p->comment);
    for (size_t i = 0; (i < clen) && (n + 1u < cap); i++)
        out[n++] = (uint8_t)p->comment[i] > 0x7E ? ' ' : p->comment[i];

    out[n] = '\0';
    return n;
}

size_t aprs_build_status(char *out, size_t cap, const char *text)
{
    int n = snprintf(out, cap, ">%s", text);
    if (n <= 0) return 0;
    if ((size_t)n >= cap) return 0;
    return (size_t)n;
}

size_t aprs_build_message(char *out, size_t cap, const char *addressee,
                          const char *text, const char *msgNo)
{
    char addr[APRS_CALL_MAX];
    size_t alen = strlen(addressee);
    if (alen > 9) alen = 9;
    memcpy(addr, addressee, alen);
    for (size_t i = alen; i < 9; i++) addr[i] = ' ';
    addr[9] = '\0';

    int n;
    if ((msgNo != NULL) && (msgNo[0] != '\0'))
        n = snprintf(out, cap, ":%s:%s{%s", addr, text, msgNo);
    else
        n = snprintf(out, cap, ":%s:%s", addr, text);

    if (n <= 0) return 0;
    if ((size_t)n >= cap) return 0;
    return (size_t)n;
}

/*
 * Acknowledgements carry the number inside the text (APRS101 chapter 12):
 * ":ADDRESSEE:ack12", not ":ADDRESSEE:ack{12".
 */
size_t aprs_build_ack(char *out, size_t cap, const char *addressee,
                      const char *msgNo)
{
    char text[12];
    snprintf(text, sizeof(text), "ack%s", (msgNo != NULL) ? msgNo : "");
    return aprs_build_message(out, cap, addressee, text, NULL);
}

size_t aprs_build_rej(char *out, size_t cap, const char *addressee,
                      const char *msgNo)
{
    char text[12];
    snprintf(text, sizeof(text), "rej%s", (msgNo != NULL) ? msgNo : "");
    return aprs_build_message(out, cap, addressee, text, NULL);
}

/* ------------------------------------------------------------ decoding --- */

static void copyTrim(char *dst, size_t cap, const char *src, size_t len)
{
    size_t n = (len < cap - 1) ? len : cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';

    /* Trailing spaces are padding in the addressee field */
    while ((n > 0) && ((dst[n - 1] == ' ') || (dst[n - 1] == '\r')
                       || (dst[n - 1] == '\n')))
        dst[--n] = '\0';
}

static bool parseUncompressedLat(const char *s, double *lat)
{
    /* DDMM.mmN */
    if ((s[4] != '.') || ((s[7] != 'N') && (s[7] != 'S')))
        return false;

    for (int i = 0; i < 7; i++)
    {
        if ((i == 4) || (i == 7)) continue;
        if ((s[i] < '0') || (s[i] > '9')) return false;
    }

    double deg = (s[0] - '0') * 10.0 + (s[1] - '0');
    double min = (s[2] - '0') * 10.0 + (s[3] - '0')
               + ((s[5] - '0') * 10.0 + (s[6] - '0')) / 100.0;
    *lat = deg + min / 60.0;
    if (s[7] == 'S') *lat = -*lat;
    return true;
}

static bool parseUncompressedLon(const char *s, double *lon)
{
    /* DDDMM.mmW */
    if ((s[5] != '.') || ((s[8] != 'E') && (s[8] != 'W')))
        return false;

    for (int i = 0; i < 8; i++)
    {
        if ((i == 5) || (i == 8)) continue;
        if ((s[i] < '0') || (s[i] > '9')) return false;
    }

    double deg = (s[0] - '0') * 100.0 + (s[1] - '0') * 10.0 + (s[2] - '0');
    double min = (s[3] - '0') * 10.0 + (s[4] - '0')
               + ((s[6] - '0') * 10.0 + (s[7] - '0')) / 100.0;
    *lon = deg + min / 60.0;
    if (s[8] == 'W') *lon = -*lon;
    return true;
}

static bool base91(const char *s, uint32_t *v)
{
    uint32_t acc = 0;
    for (int i = 0; i < 4; i++)
    {
        if ((s[i] < 33) || (s[i] > 123)) return false;
        acc = acc * 91u + ((uint32_t)s[i] - 33u);
    }
    *v = acc;
    return true;
}

/*
 * Decode a position payload. Uncompressed is tried first because its shape is
 * unmistakable (a decimal point at byte 4), then the compressed form, whose
 * base-91 fields are validated against the legal ranges.
 */
static bool parsePosition(const char *s, size_t len, aprs_pos_t *p)
{
    /*
     * The uncompressed form is type, lat 8, table, lon 9 and a symbol at s[19],
     * so a position is 20 bytes and a shorter field is truncated.  The test is
     * len >= 19, which is what the lat/lon need; what must not happen is
     * reading s[19] when the field ends there (it would be the first byte past
     * the information field, inside the frame but not part of the sentence) or
     * computing the comment length as len - 20u, a size_t underflow that only
     * stayed harmless because copyTrim() bounds by its destination.  A field
     * of exactly 19 bytes still yields its fix, with no symbol and no comment.
     */
    if (len >= 19 && s[5] == '.')
    {
        double lat, lon;
        /* layout: type, lat 8, table 1, lon 9, symbol 1, comment */
        if (parseUncompressedLat(&s[1], &lat)
            && parseUncompressedLon(&s[10], &lon))
        {
            p->lat        = lat;
            p->lon        = lon;
            p->table      = s[9];
            p->symbol     = (len >= 20) ? s[19] : ' ';
            p->compressed = false;
            copyTrim(p->comment, sizeof(p->comment), &s[20],
                     (len > 20) ? len - 20u : 0u);
            return true;
        }
    }

    if (len >= 13)
    {
        uint32_t y, x;
        if (base91(&s[2], &y) && base91(&s[6], &x))
        {
            double lat = 90.0 - (double)y / 380926.0;
            double lon = (double)x / 190463.0 - 180.0;

            /* 91^4 covers 0..68574960: anything outside the sphere is noise */
            if ((lat >= -90.0) && (lat <= 90.0)
                && (lon >= -180.0) && (lon <= 180.0))
            {
                p->lat        = lat;
                p->lon        = lon;
                p->table      = s[1];
                p->symbol     = s[10];
                p->compressed = true;
                copyTrim(p->comment, sizeof(p->comment), &s[11], len - 11u);
                return true;
            }
        }
    }

    return false;
}

bool aprs_parse_info(const char *info, size_t len, aprs_info_t *out)
{
    memset(out, 0, sizeof(*out));

    if ((info == NULL) || (len == 0))
        return false;

    const char type = info[0];

    if ((type == '!') || (type == '='))
    {
        if (parsePosition(info, len, &out->pos))
        {
            out->type = APRS_INFO_POSITION;
            return true;
        }

        /* Position of an unknown form: keep the raw text */
        out->type = APRS_INFO_OTHER;
        copyTrim(out->text, sizeof(out->text), info, len);
        return true;
    }

    if ((type == '/') || (type == '@'))
    {
        /* Position with timestamp: 7 timestamp bytes, then the payload */
        if ((len > 8) && parsePosition(info + 7, len - 7u, &out->pos))
        {
            out->type = APRS_INFO_POSITION_TS;
            return true;
        }

        out->type = APRS_INFO_OTHER;
        copyTrim(out->text, sizeof(out->text), info, len);
        return true;
    }

    if (type == '>')
    {
        out->type = APRS_INFO_STATUS;
        copyTrim(out->text, sizeof(out->text), &info[1], len - 1u);
        return true;
    }

    if (type == ':')
    {
        if (len < 11)
        {
            out->type = APRS_INFO_OTHER;
            copyTrim(out->text, sizeof(out->text), info, len);
            return true;
        }

        char addr[APRS_CALL_MAX];
        copyTrim(addr, sizeof(addr), &info[1], 9);
        memcpy(out->addressee, addr, sizeof(addr));

        const char *text = &info[11];
        size_t tlen = len - 11u;

        /* A message number trails the text as "{nn" */
        const char *brace = NULL;
        for (size_t i = 0; i < tlen; i++)
        {
            if (text[i] == '{')
            {
                brace = &text[i];
                break;
            }
        }

        size_t bodyLen = (brace != NULL) ? (size_t)(brace - text) : tlen;

        /*
         * An ACK is "ack" plus a 1..5 character message number, with nothing
         * else in the text. The length bound keeps ordinary messages that
         * start with "ack" (e.g. "acknowledged") out of this branch.
         */
        if ((bodyLen >= 4) && (bodyLen <= 8)
            && (memcmp(text, "ack", 3) == 0))
            out->type = APRS_INFO_ACK;
        else if ((bodyLen >= 4) && (bodyLen <= 8)
                 && (memcmp(text, "rej", 3) == 0))
            out->type = APRS_INFO_REJ;
        else
            out->type = APRS_INFO_MESSAGE;

        if ((out->type == APRS_INFO_ACK) || (out->type == APRS_INFO_REJ))
        {
            /* The number of an ACK is in the text itself: "ack12" */
            size_t n = 0;
            for (size_t i = 3; (i < bodyLen) && (n < 5); i++)
                out->msgNo[n++] = text[i];
            out->msgNo[n] = '\0';
        }
        else if (brace != NULL)
        {
            /* The number runs to the end of the information field, which is
             * not NUL terminated: bound the copy by the field length. */
            size_t n = 0;
            for (size_t i = (size_t)(brace - text) + 1u;
                 (i < tlen) && (n < 5); i++)
                out->msgNo[n++] = text[i];
            out->msgNo[n] = '\0';
        }

        copyTrim(out->text, sizeof(out->text), text, bodyLen);
        return true;
    }

    out->type = APRS_INFO_OTHER;
    copyTrim(out->text, sizeof(out->text), info, len);
    return true;
}

size_t aprs_format_position(char *out, size_t cap, const aprs_pos_t *p)
{
    double alat = fabs(p->lat);
    double alon = fabs(p->lon);
    int latDeg  = (int)alat;
    int lonDeg  = (int)alon;

    int n = snprintf(out, cap, "%02d %05.2f%c %03d %05.2f%c",
                     latDeg, (alat - latDeg) * 60.0,
                     (p->lat >= 0.0) ? 'N' : 'S',
                     lonDeg, (alon - lonDeg) * 60.0,
                     (p->lon >= 0.0) ? 'E' : 'W');

    return (n > 0) ? (size_t)n : 0;
}

bool aprs_parse_lat(const char *s, double *lat)
{
    double v = 0.0;
    /* Decimal degrees are accepted too, for the menu editor */
    if (strpbrk(s, ".") != NULL)
    {
        if (sscanf(s, "%lf", &v) != 1)
            return false;
        if ((v < -90.0) || (v > 90.0))
            return false;
        *lat = v;
        return true;
    }
    (void)fmtDouble;
    return false;
}

bool aprs_parse_lon(const char *s, double *lon)
{
    double v = 0.0;
    if (sscanf(s, "%lf", &v) != 1)
        return false;
    if ((v < -180.0) || (v > 180.0))
        return false;
    *lon = v;
    return true;
}

/* ----------------------------------------------------------- stations --- */

void aprs_station_list_init(aprs_station_list_t *l)
{
    memset(l, 0, sizeof(*l));
}

static aprs_station_t *findStation(aprs_station_list_t *l, const char *call)
{
    for (uint8_t i = 0; i < l->count; i++)
    {
        if (strcmp(l->st[i].call, call) == 0)
            return &l->st[i];
    }
    return NULL;
}

uint32_t aprs_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371000.0;
    double dLat = (lat2 - lat1) * DEG2RAD;
    double dLon = (lon2 - lon1) * DEG2RAD;
    double a = sin(dLat / 2.0) * sin(dLat / 2.0)
             + cos(lat1 * DEG2RAD) * cos(lat2 * DEG2RAD)
             * sin(dLon / 2.0) * sin(dLon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return (uint32_t)(r * c);
}

uint16_t aprs_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double dLon = (lon2 - lon1) * DEG2RAD;
    double y = sin(dLon) * cos(lat2 * DEG2RAD);
    double x = cos(lat1 * DEG2RAD) * sin(lat2 * DEG2RAD)
             - sin(lat1 * DEG2RAD) * cos(lat2 * DEG2RAD) * cos(dLon);
    double b = atan2(y, x) / DEG2RAD;
    if (b < 0.0) b += 360.0;
    return (uint16_t)(b + 0.5);
}

aprs_station_t *aprs_station_update(aprs_station_list_t *l,
                                    const ax25_frame_t *f,
                                    const aprs_info_t *info,
                                    bool haveLocal, double localLat,
                                    double localLon, uint32_t nowMs)
{
    char call[APRS_CALL_MAX];
    ax25_addr_format(call, sizeof(call), &f->src);

    aprs_station_t *s = findStation(l, call);
    if (s == NULL)
    {
        if (l->count < APRS_MAX_STATIONS)
        {
            s = &l->st[l->count++];
        }
        else
        {
            /* Full: replace round robin, oldest-ish first */
            s = &l->st[l->next];
            l->next = (uint8_t)((l->next + 1u) % APRS_MAX_STATIONS);
        }
        memset(s, 0, sizeof(*s));
        snprintf(s->call, sizeof(s->call), "%s", call);
    }

    s->lastMs = nowMs;
    s->lastType = info->type;
    if (s->packets < 0xFFFFu)
        s->packets++;

    if (info->type == APRS_INFO_POSITION || info->type == APRS_INFO_POSITION_TS)
    {
        s->hasPos = true;
        s->lat    = info->pos.lat;
        s->lon    = info->pos.lon;
        s->symbol = info->pos.symbol;
        s->table  = info->pos.table;
        snprintf(s->comment, sizeof(s->comment), "%.47s", info->pos.comment);

        if (haveLocal)
        {
            s->distM   = (int32_t)aprs_distance_m(localLat, localLon,
                                                  s->lat, s->lon);
            s->bearing = aprs_bearing_deg(localLat, localLon, s->lat, s->lon);
        }
    }
    else if (info->type == APRS_INFO_STATUS && s->comment[0] == '\0')
    {
        snprintf(s->comment, sizeof(s->comment), "%.47s", info->text);
    }

    return s;
}

bool aprs_beacon_due(uint32_t lastMs, uint32_t intervalS, uint32_t nowMs)
{
    if (intervalS < 30u)
        intervalS = 30u;

    return (uint32_t)(nowMs - lastMs) >= (intervalS * 1000u);
}

bool aprs_path_contains(const ax25_frame_t *f, const char *call)
{
    ax25_addr_t want;
    ax25_addr_set(&want, call);

    for (uint8_t i = 0; i < f->nDigis; i++)
    {
        if ((memcmp(f->digi[i].call, want.call, AX25_CALL_LEN) == 0)
            && (f->digi[i].ssid == want.ssid))
            return true;
    }

    return false;
}