/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host harness of the DMR protocol package (protocols/DMR/, dmr_*.c): reads
 * 48 kHz discriminator audio, the way the radio hands it to the mode, feeds
 * it to the package's demodulator and prints what the package decoded: the
 * bursts, the slot type FEC, the CRC-protected blocks, the calls, the voice
 * frames and, with mbelib, the PCM.
 *
 *   tests/host/dmr_pkg_host/build.sh
 *   build_host/dmr_pkg_host capture.wav [-o out.wav] [-v]
 *
 * This is the A/B against the tree's old chain (work/dmr-rx/build/dmr_rx on
 * the same capture): same input, same block size, same counters.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_proto.h"
#include "protocols/DMR/dmr_voice.h"
#include "protocols/DMR/dmr_sms.h"
#include "protocols/DMR/dmr_glue.h"
#include "protocols/DMR/dmr_track.h"
#include "protocols/DMR/trunk/dmr_trunk.h"

#define HALF_SAMPLES 1440 /* 30 ms at 48 kHz */

/*
 * interfaces/delays.h on the radio: dmr_track, the voice timeouts and the
 * Tier III engine stamp their entries with it and measure deadlines against
 * it.  Here it is *audio* time, not wall clock: the harness chews through two
 * seconds of capture in a few milliseconds, and a wall clock would keep every
 * one of the engine's deadlines (settle after a retune, hunt dwell, call
 * timeouts) in the future for the whole run, dropping everything it heard.
 * The loop below advances it a block (30 ms) at a time.
 */
static long long tickMs;

long long getTick(void)
{
    return tickMs;
}

/* ---- WAV in ------------------------------------------------------------- */

static int16_t *samples;
static size_t nsamples;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int wav_load(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        perror(path);
        return -1;
    }
    uint8_t hdr[12];

    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "%s: not a WAV\n", path);
        fclose(f);
        return -1;
    }

    unsigned channels = 1, bits = 16, rate = 0;
    uint8_t ck[8];

    while (fread(ck, 1, 8, f) == 8) {
        const uint32_t len = le32(ck + 4);

        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            const uint32_t n = len < sizeof(fmt) ? len : sizeof(fmt);

            if (fread(fmt, 1, n, f) != n)
                break;
            channels = le16(fmt + 2);
            rate = le32(fmt + 4);
            bits = le16(fmt + 14);
            if (len > n)
                fseek(f, (long)(len - n), SEEK_CUR);
        } else if (memcmp(ck, "data", 4) == 0) {
            if (channels != 1 || bits != 16 || rate != 48000) {
                fprintf(stderr, "%s: need mono s16 48 kHz, has %u ch %u bit "
                                "%u Hz\n", path, channels, bits, rate);
                fclose(f);
                return -1;
            }
            nsamples = len / 2;
            samples = malloc(nsamples * sizeof(int16_t));
            if (samples == NULL || fread(samples, 2, nsamples, f) != nsamples) {
                fprintf(stderr, "%s: short read\n", path);
                fclose(f);
                return -1;
            }
            fclose(f);
            return 0;
        } else {
            fseek(f, (long)len + (long)(len & 1u), SEEK_CUR);
        }
    }
    fprintf(stderr, "%s: no data chunk\n", path);
    fclose(f);
    return -1;
}

/* ---- PCM out ------------------------------------------------------------ */

static FILE *outWav;
static uint32_t outFrames;

static void wav_open(const char *path)
{
    outWav = fopen(path, "wb");
    if (outWav == NULL) {
        perror(path);
        return;
    }
    uint8_t hdr[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                        0x40, 0x1F, 0, 0, 0x80, 0x3E, 0, 0, 2, 0, 16, 0,
                        'd', 'a', 't', 'a', 0, 0, 0, 0 };

    fwrite(hdr, 1, sizeof(hdr), outWav);
}

static void wav_close(void)
{
    if (outWav == NULL)
        return;
    const uint32_t bytes = outFrames * 2;
    uint8_t sz[4] = { (uint8_t)(bytes), (uint8_t)(bytes >> 8),
                      (uint8_t)(bytes >> 16), (uint8_t)(bytes >> 24) };

    fseek(outWav, 4, SEEK_SET);
    fwrite(sz, 1, 4, outWav);
    fseek(outWav, 40, SEEK_SET);
    fwrite(sz, 1, 4, outWav);
    fclose(outWav);
    outWav = NULL;
}

static void sink(const float *pcm, int n)
{
    if (outWav == NULL)
        return;
    for (int i = 0; i < n; i++) {
        float v = pcm[i] * 32767.0f;
        int16_t s = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        fwrite(&s, 2, 1, outWav);
    }
    outFrames += (uint32_t)n;
}

/* ---- Main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    long t3Hz = 0;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "--t3") == 0 && i + 1 < argc)
            t3Hz = strtol(argv[++i], NULL, 0);
        else
            in = argv[i];
    }
    if (in == NULL) {
        fprintf(stderr, "usage: %s capture.wav [-o pcm.wav] [-v]\n", argv[0]);
        return 2;
    }
    if (wav_load(in) != 0)
        return 2;

    if (!dmr_demod_init()) {
        fprintf(stderr, "dmr_demod_init failed\n");
        return 3;
    }
    dmr_proto_reset();
    dmr_voice_reset();

    /* Tier III listening on this capture's channel */
    if (t3Hz > 0) {
        c62_t3.mode = C62_T3_LISTEN;
        c62_t3.baseHz = 0;
        c62_dmr.id = 1234567; /* an ID is needed only to register */
        dmrTrunk_start((uint32_t)t3Hz, (uint32_t)t3Hz);
    }
    if (out != NULL) {
        wav_open(out);
        dmr_voice_set_sink(sink);
    }

    float block[HALF_SAMPLES];

    for (size_t off = 0; off + HALF_SAMPLES <= nsamples; off += HALF_SAMPLES) {
        for (size_t k = 0; k < HALF_SAMPLES; k++)
            block[k] = (float)samples[off + k];
        tickMs += (long long)HALF_SAMPLES * 1000 / 48000; /* 30 ms per block */
        dmr_demod_feed_disc(block, (int)HALF_SAMPLES);

        /* The radio's speech thread does this; here, right after the block */
        while (dmr_voice_decode_next())
            ;
    }

    dmr_proto_stats_t st;
    dmr_demod_status_t ds;
    dmr_slot_info_t s0, s1;

    dmr_proto_get_stats(&st);
    dmr_demod_get_status(&ds);
    dmr_proto_get_slot(0, &s0);
    dmr_proto_get_slot(1, &s1);

    printf("= %s\n", in);
    printf("  audio        %.2f s\n", (double)nsamples / 48000.0);
    printf("  sync         %u (locked %d, bs %d, ms %d, inverted %d)\n",
           ds.syncs, ds.locked, ds.bs, ds.ms, ds.inverted);
    printf("  bursts       %u (validated %u)\n", st.frames, st.frames_valid);
    printf("  LC           headers %u, terminators %u\n", st.lc_headers,
           st.terminators);
    printf("  EMB          ok %u, bad %u (B..F %u)\n", st.emb_lc_ok,
           st.emb_bad, st.voice_bf);
    printf("  CSBK         %u, idle %u\n", st.csbk, st.idle);
    printf("  data         headers %u, packets %u, CRC bad %u, lrrp %u\n",
           st.data_headers, st.data_packets, st.data_crc_bad, st.lrrp);
    printf("  embedded LC  voice %u, alias hdr %u, alias blk %u, other %u\n",
           st.elc_voice, st.elc_ta_hdr, st.elc_ta_blk, st.elc_other);
    printf("  slots        TS1 cc %d act %d, TS2 cc %d act %d\n", s0.cc,
           (int)s0.activity, s1.cc, (int)s1.activity);

    dmr_log_entry_t log[DMR_LOG_LEN];
    const int n = dmr_proto_get_log(log);

    printf("  call log     %d entries\n", n);
    for (int i = 0; i < n; i++)
        printf("    %6u ms slot %u %s %u -> %u %s%s\n", log[i].t_ms,
               log[i].slot, log[i].group ? "group" : "unit", log[i].src,
               log[i].dst, log[i].alias[0] ? log[i].alias : "",
               log[i].data ? " [data]" : "");

    if (t3Hz > 0) {
        dmrTrunkStatus_t ts;
        dmrTrunk_getStatus(&ts);
        printf("  tier III     state %u (0 off, 1 hunt, 2 camped, 3 follow), "
               "confirmed %d, TSCC %u Hz cc %u syscode %u\n", ts.state,
               ts.confirmed, ts.tsccHz, ts.tsccCc, ts.syscode);
        printf("               csbk %u, grants %u, followed %u, unmapped %u, "
               "hunts %u\n", ts.csbk, ts.grants, ts.followed, ts.unmapped,
               ts.hunts);
    }

    uint32_t frames = 0, avgUs = 0;

    dmr_voice_get_timing(&frames, &avgUs);
    printf("  voice        %u frames decoded, %u us avg\n", frames, avgUs);
    printf("  pcm          %u samples (%.2f s)\n", outFrames,
           (double)outFrames / 8000.0);

    if (verbose) {
        int slot = 0;
        if (dmr_proto_bs_slot(&slot))
            printf("  last slot    TS%d\n", slot + 1);
    }

    wav_close();
    free(samples);
    return 0;
}
