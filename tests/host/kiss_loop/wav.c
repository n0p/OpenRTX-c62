/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool wav_load(const char *path, wav_t *w)
{
    memset(w, 0, sizeof(*w));

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0)
    {
        fclose(f);
        return false;
    }

    uint16_t channels = 1, bits = 16, format = 1;
    uint32_t rate = 0;
    long dataOffset = -1, dataLen = 0;

    while (true)
    {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8)
            break;

        uint32_t len = rd32(ch + 4);

        if (memcmp(ch, "fmt ", 4) == 0)
        {
            uint8_t fmt[40];
            size_t n = (len < sizeof(fmt)) ? len : sizeof(fmt);
            if (fread(fmt, 1, n, f) != n)
                break;
            format   = rd16(fmt);
            channels = rd16(fmt + 2);
            rate     = rd32(fmt + 4);
            bits     = rd16(fmt + 14);
            if ((len > n) && (fseek(f, (long)(len - n), SEEK_CUR) != 0))
                break;
        }
        else if (memcmp(ch, "data", 4) == 0)
        {
            dataOffset = ftell(f);
            dataLen    = (long)len;
            if (fseek(f, (long)len, SEEK_CUR) != 0)
                break;
        }
        else
        {
            if (fseek(f, (long)((len + 1u) & ~1u), SEEK_CUR) != 0)
                break;
        }
    }

    if ((dataOffset < 0) || (format != 1) || (bits != 16)
        || (channels < 1) || (channels > 2) || (rate < 8000))
    {
        fclose(f);
        return false;
    }

    if (fseek(f, dataOffset, SEEK_SET) != 0)
    {
        fclose(f);
        return false;
    }

    size_t frames = (size_t)dataLen / (2u * channels);
    int16_t *raw  = malloc(frames * channels * sizeof(int16_t));
    if (raw == NULL)
    {
        fclose(f);
        return false;
    }

    size_t got = fread(raw, sizeof(int16_t), frames * channels, f);
    fclose(f);
    frames = got / channels;

    int16_t *mono = malloc(frames * sizeof(int16_t));
    if (mono == NULL)
    {
        free(raw);
        return false;
    }

    for (size_t i = 0; i < frames; i++)
    {
        if (channels == 1)
            mono[i] = raw[i];
        else
            mono[i] = (int16_t)(((int32_t)raw[2 * i] + raw[2 * i + 1]) / 2);
    }

    free(raw);
    w->samples = mono;
    w->count   = frames;
    w->rate    = rate;
    return true;
}

void wav_free(wav_t *w)
{
    free(w->samples);
    memset(w, 0, sizeof(*w));
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void writeHeader(FILE *f, uint32_t rate, uint32_t count)
{
    uint8_t hdr[44];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "RIFF", 4);
    wr32(hdr + 4, 36u + count * 2u);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    wr32(hdr + 16, 16);
    wr16(hdr + 20, 1);            /* PCM */
    wr16(hdr + 22, 1);            /* mono */
    wr32(hdr + 24, rate);
    wr32(hdr + 28, rate * 2u);    /* byte rate */
    wr16(hdr + 32, 2);            /* block align */
    wr16(hdr + 34, 16);           /* bits */
    memcpy(hdr + 36, "data", 4);
    wr32(hdr + 40, count * 2u);
    fwrite(hdr, 1, sizeof(hdr), f);
}

bool wav_save(const char *path, const int16_t *samples, size_t count,
              uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return false;
    writeHeader(f, rate, (uint32_t)count);
    size_t wr = fwrite(samples, sizeof(int16_t), count, f);
    fclose(f);
    return wr == count;
}

bool wav_append(const char *path, const int16_t *samples, size_t count,
                uint32_t rate)
{
    FILE *f = fopen(path, "rb+");
    if (f == NULL)
        return wav_save(path, samples, count, rate);

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return false;
    }
    long end = ftell(f);

    if (fwrite(samples, sizeof(int16_t), count, f) != count)
    {
        fclose(f);
        return false;
    }

    /* Patch the RIFF and data sizes */
    long total = end + (long)(count * 2u) - 44;
    uint8_t v[4];
    wr32(v, (uint32_t)(total + 36));
    fseek(f, 4, SEEK_SET);
    fwrite(v, 1, 4, f);
    wr32(v, (uint32_t)total);
    fseek(f, 40, SEEK_SET);
    fwrite(v, 1, 4, f);

    fclose(f);
    (void)rate;
    return true;
}
