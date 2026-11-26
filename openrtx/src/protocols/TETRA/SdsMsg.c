/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SDS message model: libtetra short data event -> displayable message.
 */

#include <stdio.h>
#include <string.h>

#include "protocols/TETRA/SdsMsg.h"

/*
 * Working buffer for the UTF-8 text before folding it into the font-safe
 * ASCII of the message. A UDD4 of 2047 bits holds at most 292 GSM 7-bit
 * characters, which is also the worst case in UTF-8 (2 bytes for Latin-1,
 * 3 for UCS-2 BMP characters), so 384 covers every single-part message.
 */
#define UTF8_MAX 384

/* Latin-1 supplement (U+0080..U+00FF) folded to ASCII, 1 byte per entry. */
static const char latin1_fold[0x80] = {
    /* 0x80 */ '?', '?', '?', '?', '?', '?', '?', '?',
    /* 0x88 */ '?', '?', '?', '?', '?', '?', '?', '?',
    /* 0x90 */ '?', '?', '?', '?', '?', '?', '?', '?',
    /* 0x98 */ '?', '?', '?', '?', '?', '?', '?', '?',
    /* 0xA0 */ ' ', '!', 'c', 'E', '$', 'Y', '|', 'S',
    /* 0xA8 */ '"', 'c', 'a', '<', '-', '-', 'R', '-',
    /* 0xB0 */ 'o', '+', '2', '3', '\'', 'u', 'P', '.',
    /* 0xB8 */ ',', '1', 'o', '>', '1', '/', '4', '?',
    /* 0xC0 */ 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'C',
    /* 0xC8 */ 'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I',
    /* 0xD0 */ 'D', 'N', 'O', 'O', 'O', 'O', 'O', 'x',
    /* 0xD8 */ 'O', 'U', 'U', 'U', 'U', 'Y', 'T', 's',
    /* 0xE0 */ 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c',
    /* 0xE8 */ 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
    /* 0xF0 */ 'd', 'n', 'o', 'o', 'o', 'o', 'o', '/',
    /* 0xF8 */ 'o', 'u', 'u', 'u', 'u', 'y', 't', 'y',
};

/* Code points above U+00FF that the GSM 03.38 alphabet can produce. */
static char greek_fold(uint32_t cp)
{
    switch (cp) {
    case 0x0394: return 'D'; /* Delta */
    case 0x03a6: return 'F'; /* Phi */
    case 0x0393: return 'G'; /* Gamma */
    case 0x039b: return 'L'; /* Lambda */
    case 0x03a9: return 'O'; /* Omega */
    case 0x03a0: return 'P'; /* Pi */
    case 0x03a8: return 'Y'; /* Psi */
    case 0x03a3: return 'S'; /* Sigma */
    case 0x0398: return 'T'; /* Theta */
    case 0x039e: return 'X'; /* Xi */
    case 0x20ac: return 'E'; /* Euro sign */
    default: return '?';
    }
}

int sds_ascii_from_utf8(const char *in, char *out, size_t len, bool *truncated)
{
    size_t n = 0;
    bool cut = false;

    if (len == 0)
        return 0;

    while (*in != '\0') {
        unsigned char c = (unsigned char)*in;
        uint32_t cp;
        int extra;

        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f;
            extra = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f;
            extra = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            in++; /* stray continuation byte */
            cut = cut || (n + 1 >= len);
            if (n + 1 < len)
                out[n++] = '?';
            continue;
        }

        in++;
        for (int k = 0; k < extra; k++) {
            if (((unsigned char)*in & 0xc0) != 0x80) {
                cp = '?';
                break;
            }
            cp = (cp << 6) | ((unsigned char)*in & 0x3f);
            in++;
        }

        char ch;

        if (cp < 0x20 || cp == 0x7f) {
            ch = ' '; /* CR, LF and tabs would break the line rendering */
        } else if (cp < 0x80) {
            ch = (char)cp;
        } else if (cp < 0x100) {
            ch = latin1_fold[cp - 0x80];
        } else {
            ch = greek_fold(cp);
        }

        if (n + 1 >= len) {
            cut = true;
            break;
        }
        out[n++] = ch;
    }

    out[n] = '\0';
    if (truncated != NULL)
        *truncated = cut;
    return (int)n;
}

static void copy_raw(struct sds_msg *m, const struct tetra_sds *s)
{
    unsigned nbytes = (s->nbits + 7u) / 8u;

    if (nbytes > SDS_RAW_BYTES)
        nbytes = SDS_RAW_BYTES;
    if (s->data != NULL && nbytes > 0)
        memcpy(m->raw, s->data, nbytes);
}

int sds_msg_hex(const struct sds_msg *m, char *out, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    unsigned nbits = m->text_bits;
    unsigned nbytes = (nbits + 7u) / 8u;

    if (nbytes > SDS_RAW_BYTES)
        nbytes = SDS_RAW_BYTES;
    for (unsigned i = 0; i < nbytes; i++) {
        if (n + 2 >= len)
            break;
        out[n++] = hex[m->raw[i] >> 4];
        out[n++] = hex[m->raw[i] & 0xf];
    }
    out[n] = '\0';
    return (int)n;
}

static int decode_text(const struct tetra_sds *s, struct sds_msg *m)
{
    struct tetra_sds_text t;
    char utf8[UTF8_MAX];
    int n;

    if (tetra_sds_parse_text(s, &t) != 0)
        return -1;

    m->kind = SDS_KIND_TEXT;
    m->pid = t.protocol_id;
    m->sdstl = t.sdstl;
    m->immediate = t.immediate;
    m->coding = t.coding;
    m->msg_ref = t.msg_ref;
    m->report_req = t.report_req;
    m->has_timestamp = t.has_timestamp;
    m->timestamp = t.timestamp;
    m->text_bits = t.nbits;

    n = tetra_sds_text_utf8(&t, utf8, sizeof(utf8));
    if (n < 0)
        return -1;

    /* A full buffer means the text did not fit: it was cut, not empty. */
    if (n >= (int)sizeof(utf8) - 1)
        m->truncated = true;
    sds_ascii_from_utf8(utf8, m->text, sizeof(m->text), &m->truncated);
    m->text_len = (uint8_t)strlen(m->text);

    /* loc: copy the payload head so the store can keep the raw bytes too */
    copy_raw(m, s);
    return 0;
}

static int decode_location(const struct tetra_sds *s, struct sds_msg *m)
{
    struct tetra_sds_location l;
    char nmea[UTF8_MAX];

    if (tetra_sds_parse_location(s, &l) != 0)
        return -1;

    m->kind = SDS_KIND_LOCATION;
    m->pid = l.protocol_id;
    m->sdstl = l.sdstl;
    m->coding = l.coding;
    m->msg_ref = l.msg_ref;
    m->report_req = l.report_req;
    m->text_bits = l.nbits;

    tetra_sds_location_ascii(&l, nmea, sizeof(nmea));
    sds_ascii_from_utf8(nmea, m->text, sizeof(m->text), &m->truncated);
    m->text_len = (uint8_t)strlen(m->text);
    copy_raw(m, s);
    return 0;
}

static int decode_lip(const struct tetra_sds *s, struct sds_msg *m)
{
    struct tetra_lip l;

    if (tetra_sds_parse_lip(s, &l) != 0)
        return -1;

    m->kind = SDS_KIND_LIP;
    m->pid = TETRA_PID_LIP;
    m->text_bits = s->nbits;

    if (l.has_position) {
        /* 1e-7 degrees, four decimals is ~11 m: more than the display needs */
        long lat = l.lat_e7;
        long lon = l.lon_e7;
        const char *ns = lat < 0 ? "S" : "N";
        const char *ew = lon < 0 ? "W" : "E";

        if (lat < 0)
            lat = -lat;
        if (lon < 0)
            lon = -lon;
        snprintf(m->text, sizeof(m->text), "LIP %s%ld.%04ld %s%ld.%04ld", ns,
                 lat / 10000000L, (lat % 10000000L) / 1000L, ew,
                 lon / 10000000L, (lon % 10000000L) / 1000L);
    } else if (l.kind == TETRA_LIP_SHORT_REPORT) {
        snprintf(m->text, sizeof(m->text), "LIP report");
    } else {
        snprintf(m->text, sizeof(m->text), "LIP pdu %u", l.pdu_ext);
    }
    m->text_len = (uint8_t)strlen(m->text);
    copy_raw(m, s);
    return 0;
}

int sds_msg_decode(const struct tetra_sds *s, struct sds_msg *m)
{
    memset(m, 0, sizeof(*m));

    if (s == NULL)
        return -1;

    m->sds_type = (uint8_t)s->type;
    m->src_ssi = s->src_ssi;
    m->dst_ssi = s->dst_ssi;
    m->complete = s->complete;
    m->addressed = s->addressed;
    m->text_bits = s->nbits;

    if (!s->complete) {
        /* A fragment was lost: show what we know, not a half message. */
        return -1;
    }

    switch (s->type) {
    case TETRA_SDS_STATUS:
        m->kind = SDS_KIND_STATUS;
        m->status = s->status;
        snprintf(m->text, sizeof(m->text), "STATUS %u", s->status);
        m->text_len = (uint8_t)strlen(m->text);
        m->text_bits = 16;
        return 0;

    case TETRA_SDS_UDD1:
    case TETRA_SDS_UDD2:
    case TETRA_SDS_UDD3:
        m->kind = SDS_KIND_RAW;
        copy_raw(m, s);
        if (sds_msg_hex(m, m->text, sizeof(m->text)) == 0)
            m->text[0] = '\0';
        m->text_len = (uint8_t)strlen(m->text);
        return 0;

    case TETRA_SDS_UDD4:
        if (decode_text(s, m) == 0)
            return 0;
        if (decode_location(s, m) == 0)
            return 0;
        if (decode_lip(s, m) == 0)
            return 0;
        /* Something else: protocol identifier we do not render. */
        m->kind = SDS_KIND_OTHER;
        m->pid = (s->nbits >= 8 && s->data != NULL) ? s->data[0] : 0;
        snprintf(m->text, sizeof(m->text), "PID 0x%02x", m->pid);
        m->text_len = (uint8_t)strlen(m->text);
        copy_raw(m, s);
        return 0;

    default:
        return -1;
    }
}

bool sds_msg_has_text(const struct sds_msg *m)
{
    return m->text_len > 0 && m->kind != SDS_KIND_EMPTY;
}

const char *sds_msg_kind_name(enum sds_kind kind)
{
    switch (kind) {
    case SDS_KIND_TEXT: return "text";
    case SDS_KIND_STATUS: return "status";
    case SDS_KIND_RAW: return "raw";
    case SDS_KIND_LOCATION: return "location";
    case SDS_KIND_LIP: return "lip";
    case SDS_KIND_OTHER: return "other";
    case SDS_KIND_EMPTY:
    default: return "empty";
    }
}

int sds_msg_format(const struct sds_msg *m, char *out, size_t len)
{
    int n;

    if (len == 0)
        return 0;
    if (!sds_msg_has_text(m)) {
        out[0] = '\0';
        return 0;
    }
    if (m->src_ssi != 0)
        n = snprintf(out, len, "%lu: %s", (unsigned long)m->src_ssi, m->text);
    else
        n = snprintf(out, len, "%s", m->text);
    if (n < 0)
        return 0;
    return (n < (int)len) ? n : (int)len - 1;
}
