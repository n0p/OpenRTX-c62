/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file sim_png.c
 * @brief PNG writer of the host simulator. See sim_png.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "sim_png.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/** One PNG chunk: length, type, payload, CRC of type+payload. */
static bool write_chunk(FILE *f, const char *type, const uint8_t *data,
                        uint32_t len)
{
    uint8_t head[8];
    put32(head, len);
    memcpy(head + 4, type, 4);

    if (fwrite(head, 1, sizeof(head), f) != sizeof(head))
        return false;

    if ((len > 0) && (fwrite(data, 1, len, f) != len))
        return false;

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len > 0)
        crc = crc32(crc, data, len);

    uint8_t tail[4];
    put32(tail, (uint32_t)crc);

    return fwrite(tail, 1, sizeof(tail), f) == sizeof(tail);
}

bool sim_png_write(const char *path, const uint8_t *rgb, int w, int h)
{
    if ((path == NULL) || (rgb == NULL) || (w <= 0) || (h <= 0))
        return false;

    /* Raw scanlines: one filter byte (0, "none") then the row */
    size_t stride = (size_t)w * 3;
    size_t raw_len = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_len);

    if (raw == NULL)
        return false;

    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (stride + 1) * (size_t)y;
        row[0] = 0;
        memcpy(row + 1, rgb + stride * (size_t)y, stride);
    }

    uLongf zlen = compressBound(raw_len);
    uint8_t *z = malloc(zlen);

    if (z == NULL) {
        free(raw);
        return false;
    }

    bool ok = compress2(z, &zlen, raw, raw_len, 6) == Z_OK;

    if (ok) {
        FILE *f = fopen(path, "wb");

        if (f == NULL) {
            ok = false;
        } else {
            static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0d,
                                                  0x0a, 0x1a, 0x0a };
            uint8_t ihdr[13];
            put32(ihdr + 0, (uint32_t)w);
            put32(ihdr + 4, (uint32_t)h);
            ihdr[8]  = 8;    /* bit depth           */
            ihdr[9]  = 2;    /* colour type: RGB    */
            ihdr[10] = 0;    /* deflate             */
            ihdr[11] = 0;    /* adaptive filtering  */
            ihdr[12] = 0;    /* no interlace        */

            ok = (fwrite(signature, 1, sizeof(signature), f)
                  == sizeof(signature))
                 && write_chunk(f, "IHDR", ihdr, sizeof(ihdr))
                 && write_chunk(f, "IDAT", z, (uint32_t)zlen)
                 && write_chunk(f, "IEND", NULL, 0);

            if (fclose(f) != 0)
                ok = false;
        }
    }

    free(z);
    free(raw);

    return ok;
}
