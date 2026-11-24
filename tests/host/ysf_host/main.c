/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.c
 * @brief Host harness for the System Fusion receive chain.
 *
 * Feeds a 48 kHz discriminator recording (stereo or mono WAV, 16 bit) through
 * the firmware's YsfPhy + YsfFrame, prints what it decodes, and writes the
 * decoded voice to a WAV next to it.  It is the same C code the radio runs, so
 * the numbers it prints are the numbers the firmware gets.
 *
 *   make && ./build/ysf_host capture.wav
 *
 * Second test: a text message.  There is no off-air capture carrying one, so
 * the harness builds a frame with the reverse of the receive chain (CRC, Golay,
 * convolutional code, interleave, whitening) and decodes it through the same
 * ysfFrame_decode().  That exercises the whole path except the modem, which the
 * capture covers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>

#include "protocols/YSF/YsfFrame.h"
#include "dmr_vocoder.h"
#include "protocols/YSF/YsfPhy.h"

/* ------------------------------------------------------------------ */
/* WAV                                                                */
/* ------------------------------------------------------------------ */

struct wav
{
    int channels;
    int rate;
    long samples;   /* per channel */
    int16_t *data;  /* interleaved */
};

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int wav_load(const char *path, struct wav *w)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12)
        goto fail;
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        goto fail;

    memset(w, 0, sizeof(*w));

    for (;;)
    {
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8)
            goto fail;

        uint32_t len = le32(ch + 4);

        if (!memcmp(ch, "fmt ", 4))
        {
            unsigned char *fmt = malloc(len);
            if (fmt == NULL || fread(fmt, 1, len, f) != len)
                goto fail;
            w->channels = le16(fmt + 2);
            w->rate = (int)le32(fmt + 4);
            free(fmt);
        }
        else if (!memcmp(ch, "data", 4))
        {
            w->data = malloc(len);
            if (w->data == NULL || fread(w->data, 1, len, f) != len)
                goto fail;
            w->samples = (long)(len / (2u * (uint32_t)w->channels));
            fclose(f);
            return 0;
        }
        else
        {
            if (fseek(f, (long)len + (long)(len & 1), SEEK_CUR) != 0)
                goto fail;
        }
    }

fail:
    fclose(f);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Voice output                                                       */
/* ------------------------------------------------------------------ */

static FILE *pcmOut;
static long pcmSamples;
static int16_t pcmBuf[160];

static void pcm_open(const char *path)
{
    pcmOut = fopen(path, "wb");
    if (pcmOut == NULL)
        return;

    /* 8 kHz mono: the header is rewritten when the file closes */
    unsigned char hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V',
                             'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1,
                             0, 0x40, 0x1F, 0, 0, 0x80, 0x3E, 0, 0, 2, 0,
                             16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    fwrite(hdr, 1, sizeof(hdr), pcmOut);
}

static void pcm_write(const int16_t *samples, int n)
{
    if (pcmOut == NULL)
        return;

    fwrite(samples, sizeof(int16_t), (size_t)n, pcmOut);
    pcmSamples += n;
}

static void pcm_close(void)
{
    if (pcmOut == NULL)
        return;

    uint32_t bytes = (uint32_t)(pcmSamples * 2);
    unsigned char tail[4];
    tail[0] = (unsigned char)(bytes & 0xFF);
    tail[1] = (unsigned char)((bytes >> 8) & 0xFF);
    tail[2] = (unsigned char)((bytes >> 16) & 0xFF);
    tail[3] = (unsigned char)((bytes >> 24) & 0xFF);
    fseek(pcmOut, 40, SEEK_SET);
    fwrite(tail, 1, 4, pcmOut);
    bytes += 36;
    tail[0] = (unsigned char)(bytes & 0xFF);
    tail[1] = (unsigned char)((bytes >> 8) & 0xFF);
    tail[2] = (unsigned char)((bytes >> 16) & 0xFF);
    tail[3] = (unsigned char)((bytes >> 24) & 0xFF);
    fseek(pcmOut, 4, SEEK_SET);
    fwrite(tail, 1, 4, pcmOut);
    fclose(pcmOut);
    pcmOut = NULL;
}

/* ------------------------------------------------------------------ */
/* Frame handling                                                     */
/* ------------------------------------------------------------------ */

static struct
{
    unsigned frames;
    unsigned fichOk;
    unsigned fichBad;
    unsigned voice49;
    unsigned voice72;
    unsigned textMessages;
    unsigned unsupported;
} stats;

static const char *dtName(int dt)
{
    switch (dt)
    {
        case 0: return "V/D1";
        case 1: return "DataFR";
        case 2: return "V/D2";
        case 3: return "VoiceFR";
        default: return "?";
    }
}

static const char *fiName(int fi)
{
    switch (fi)
    {
        case 0: return "HC";
        case 1: return "CC";
        case 2: return "TC";
        default: return "XX";
    }
}

static void onFrame(const struct ysfFrame *frame, void *user)
{
    (void)user;

    stats.frames++;

    const struct ysfDecoded *d = ysfFrame_decode(frame);

    if (d == NULL)
        return;

    if (!d->fichOk)
    {
        stats.fichBad++;
        printf("f=%3u sync=%d evm=%3u%% FICH FAIL\n", stats.frames,
               20 - frame->syncErrors, frame->evm);
        return;
    }

    stats.fichOk++;

    for (unsigned i = 0; i < d->voiceCount; i++)
    {
        const struct ysfVoiceSlot *slot = &d->voice[i];

        if (slot->nbits == 49)
        {
            stats.voice49++;

            if (dmr_vocoder_decode_ambe(slot->bits, pcmBuf) == 0)
                pcm_write(pcmBuf, 160);
        }
        else if (slot->nbits == 72)
        {
            uint8_t frame[DMR_VOCODER_FRAME_BYTES];

            stats.voice72++;
            memset(frame, 0, sizeof(frame));

            for (int k = 0; k < 72; k++)
                frame[k >> 3] = (uint8_t)((frame[k >> 3] << 1) |
                                          (slot->bits[k] & 1u));

            if (dmr_vocoder_decode(frame, pcmBuf) == 0)
                pcm_write(pcmBuf, 160);
        }
    }

    const struct ysfCallInfo *c = ysfFrame_call();

    if (c->dt == 3)
        stats.unsupported += YSF_VOICE_SLOTS;

    printf("f=%3u sync=%d evm=%3u%% %-7s %s fn=%u/%u dgid=%s%u "
           "DST='%s' SRC='%s' U/L='%s' D/L='%s' blk=%u\n",
           stats.frames, 20 - frame->syncErrors, frame->evm, dtName(c->dt),
           fiName(c->fi), c->fn, c->ft, c->dgidValid ? "" : "-", c->dgid,
           c->dest, c->source, c->uplink, c->downlink, c->dataBlocks);

    if (d->textChanged)
    {
        stats.textMessages++;

        const struct ysfText *t = ysfFrame_text();

        printf("    TEXT: \"%s\"\n", t->text);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s capture.wav [out.wav]\n", argv[0]);
        return 1;
    }

    struct wav w;
    if (wav_load(argv[1], &w) != 0)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    printf("input: %d ch, %d Hz, %ld samples (%.2f s)\n", w.channels, w.rate,
           w.samples, (double)w.samples / (double)w.rate);

    if (w.rate != 48000)
    {
        fprintf(stderr, "expected 48 kHz discriminator audio\n");
        return 1;
    }

    pcm_open((argc > 2) ? argv[2] : "ysf_voice.wav");
    ysfPhy_init();
    ysfFrame_reset();

    int16_t *mono = malloc((size_t)w.samples * sizeof(int16_t));
    if (mono == NULL)
        return 1;

    for (long i = 0; i < w.samples; i++)
        mono[i] = w.data[i * w.channels];

    for (long i = 0; i < w.samples; i += 4800)
    {
        long n = w.samples - i;
        if (n > 4800)
            n = 4800;
        ysfPhy_push(&mono[i], (size_t)n, onFrame, NULL);
    }

    pcm_close();

    const struct ysfCallInfo *c = ysfFrame_call();

    printf("\nframes %u, FICH ok %u, FICH bad %u\n", stats.frames, stats.fichOk,
           stats.fichBad);
    printf("voice slots: %u at 49 bits (V/D2), %u at 72 bits (V/D1),"
           " %u unsupported\n", stats.voice49, stats.voice72,
           stats.unsupported);
    printf("text messages %u, PCM %ld samples\n", stats.textMessages,
           pcmSamples);
    printf("last call: DST='%s' SRC='%s' DG-ID %s%u, %u data blocks\n", c->dest,
           c->source, c->dgidValid ? "" : "-", c->dgid, c->dataBlocks);

    free(mono);
    free(w.data);

    return 0;
}
