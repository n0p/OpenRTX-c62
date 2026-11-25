/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host bench for the packet modem: KISS codec, the AFSK 1200 and G3RUH 9600
 * engines end to end, BER against noise, and the WAV files that let direwolf
 * be the reference on both rates.
 *
 *   kiss_test selftest
 *   kiss_test loop [--modem 1200|9600] [--len N] [--trials N]
 *   kiss_test ber  [--modem 1200|9600] [--from dB] [--to dB] [--step dB]
 *                  [--bits N]
 *   kiss_test wav <out.wav> <1200|9600> <string> [<string> ...]
 *   kiss_test decode <in.wav> <1200|9600>
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/APRS/ax25.h"
#include "protocols/APRS/hdlc.h"
#include "protocols/KISS/kiss.h"
#include "protocols/KISS/packet_modem.h"
#include "wav.h"

#define FS 48000u

static int failures;
static int checks;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL  %s\n", what);
    }
}

/* ------------------------------------------------------------- helpers --- */

static uint32_t rngState = 0x12345678u;

static uint32_t rnd(void)
{
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rngState = x;
    return x;
}

/* Gaussian noise, Box-Muller in double, deterministic for a fixed seed. */
static double gauss(void)
{
    static bool have;
    static double spare;

    if (have)
    {
        have = false;
        return spare;
    }

    double u, v, s;
    do
    {
        u = (double)(rnd() >> 8) / 16777216.0;
        v = (double)(rnd() >> 8) / 16777216.0;
        u = 2.0 * u - 1.0;
        v = 2.0 * v - 1.0;
        s = u * u + v * v;
    } while ((s >= 1.0) || (s == 0.0));

    s = sqrt(-2.0 * log(s) / s);
    spare = v * s;
    have = true;
    return u * s;
}

static pkt_modem_t parseModem(const char *s)
{
    if ((s == NULL) || (strcmp(s, "1200") == 0))
        return PKT_MODEM_AFSK1200;
    if (strcmp(s, "9600") == 0)
        return PKT_MODEM_AFSK9600;
    return (pkt_modem_t)pkt_modem_parse(s);
}

/* Build an AX.25 UI frame with the given callsigns and information field. */
static size_t buildFrame(uint8_t *frame, size_t cap,
                         const char *dst, const char *src,
                         const uint8_t *info, size_t infoLen)
{
    ax25_frame_t f;
    memset(&f, 0, sizeof(f));
    ax25_addr_set(&f.dst, dst);
    ax25_addr_set(&f.src, src);
    f.nDigis  = 0;
    f.info    = info;
    f.infoLen = infoLen;
    return ax25_build_ui(frame, cap, &f);
}

typedef struct
{
    uint8_t data[KISS_MAX_PAYLOAD];
    size_t  len;
    int     frames;
} rxSink_t;

static void onRxFrame(const uint8_t *payload, size_t len, void *user)
{
    rxSink_t *s = (rxSink_t *)user;
    if (s->frames == 0)
    {
        if (len > sizeof(s->data))
            len = sizeof(s->data);
        memcpy(s->data, payload, len);
        s->len = len;
    }
    s->frames++;
}

/*
 * Modulate one KISS payload and return the number of samples. `out` must have
 * room for a frame at the slowest rate: 330 bytes * 10 bits * 40 samples.
 */
static size_t modulate(pkt_modem_t mode, const uint8_t *payload, size_t len,
                       int16_t *out, size_t cap, uint32_t txdelayMs)
{
    pkt_engine_t e;
    if (!pkt_engine_init(&e, mode, FS))
        return 0;
    if (!pkt_tx_begin(&e, payload, len, txdelayMs, 20))
        return 0;

    size_t n = 0;
    while (n < cap)
    {
        size_t w = pkt_tx_block(&e, out + n, cap - n);
        if (w == 0)
            break;
        n += w;
    }
    return n;
}

static int demodulate(pkt_modem_t mode, const int16_t *in, size_t n,
                      rxSink_t *sink)
{
    pkt_engine_t e;
    if (!pkt_engine_init(&e, mode, FS))
        return -1;

    memset(sink, 0, sizeof(*sink));
    pkt_rx_begin(&e, onRxFrame, sink);

    for (size_t i = 0; i < n; i += 480)
    {
        size_t block = (n - i < 480) ? (n - i) : 480;
        pkt_rx_block(&e, in + i, block);
    }
    return sink->frames;
}

/* ------------------------------------------------------------ selftest --- */

static void testKissCodec(void)
{
    /* Round trip of a frame with every special byte in it */
    static const uint8_t payload[] =
        { 0x82, 0xA0, 0xC0, 0xDB, 0xDC, 0xDD, 0x03, 0xF0, 'h', 'i', 0xC0 };

    uint8_t enc[512];
    size_t n = kiss_enc_data(enc, sizeof(enc), 0, payload, sizeof(payload),
                             false);
    check(n > 0, "kiss: encode data");
    check(enc[0] == KISS_FEND, "kiss: leading FEND");
    check(enc[n - 1] == KISS_FEND, "kiss: trailing FEND");

    /* No bare 0xC0 inside the frame besides the delimiters */
    int inner = 0;
    for (size_t i = 1; i + 1 < n; i++)
        if (enc[i] == KISS_FEND)
            inner++;
    check(inner == 0, "kiss: FEND escaped inside the frame");

    kiss_dec_t d;
    kiss_dec_init(&d, false);
    kiss_ev_t ev = KISS_EV_NONE;
    for (size_t i = 0; i < n; i++)
    {
        kiss_ev_t got = kiss_dec_byte(&d, enc[i]);
        if (got != KISS_EV_NONE)
            ev = got;
    }
    check(ev == KISS_EV_DATA, "kiss: decode data");
    check(d.msg.len == sizeof(payload), "kiss: payload length");
    check(memcmp(d.msg.data, payload, sizeof(payload)) == 0,
          "kiss: payload bytes");
    check(d.msg.port == 0, "kiss: port 0");

    /* Port in the high nibble */
    n = kiss_enc_data(enc, sizeof(enc), 7, payload, 5, false);
    kiss_dec_init(&d, false);
    for (size_t i = 0; i < n; i++)
        ev = kiss_dec_byte(&d, enc[i]);
    check((ev == KISS_EV_DATA) && (d.msg.port == 7), "kiss: port 7");

    /* Parameters */
    static const uint8_t values[] = { 30, 63, 10, 5, 1 };
    static const uint8_t cmds[] =
        { KISS_CMD_TXDELAY, KISS_CMD_PERSIST, KISS_CMD_SLOTTIME,
          KISS_CMD_TXTAIL, KISS_CMD_FULLDUP };

    for (size_t i = 0; i < sizeof(cmds); i++)
    {
        n = kiss_enc_param(enc, sizeof(enc), cmds[i], values[i]);
        kiss_dec_init(&d, false);
        for (size_t j = 0; j < n; j++)
            ev = kiss_dec_byte(&d, enc[j]);
        check((ev == KISS_EV_PARAM) && (d.msg.cmd == cmds[i]) &&
              (d.msg.len == 1) && (d.msg.data[0] == values[i]),
              "kiss: parameter round trip");
        check(kiss_cmd_isParam(d.msg.cmd), "kiss: parameter classified");
    }

    /* Return, both spellings */
    n = kiss_enc_cmd(enc, sizeof(enc), 0, KISS_CMD_RETURN, NULL, 0);
    check(n == 3 && enc[1] == 0xFF, "kiss: 0xFF return");
    kiss_dec_init(&d, false);
    for (size_t i = 0; i < n; i++)
        ev = kiss_dec_byte(&d, enc[i]);
    check(ev == KISS_EV_RETURN, "kiss: return event");

    enc[1] = 0x0F;
    kiss_dec_init(&d, false);
    for (size_t i = 0; i < n; i++)
        ev = kiss_dec_byte(&d, enc[i]);
    check(ev == KISS_EV_RETURN, "kiss: 0x0F end-kiss");

    /* Unknown command */
    enc[1] = 0x09;
    kiss_dec_init(&d, false);
    for (size_t i = 0; i < n; i++)
        ev = kiss_dec_byte(&d, enc[i]);
    check(ev == KISS_EV_UNKNOWN, "kiss: unknown command");

    /* KISS simple: no command byte, no escaping. A 0xC0 in the payload cannot
     * be represented, so the first two bytes (0x82 0xA0) are used. */
    n = kiss_enc_data(enc, sizeof(enc), 0, payload, 2, true);
    check(n == 4, "kiss-simple: encoding");
    kiss_dec_init(&d, true);
    for (size_t i = 0; i < n; i++)
        ev = kiss_dec_byte(&d, enc[i]);
    check((ev == KISS_EV_DATA) && (d.msg.len == 2) &&
          (memcmp(d.msg.data, payload, 2) == 0), "kiss-simple: round trip");

    /* KISS simple refuses a payload it cannot carry */
    n = kiss_enc_data(enc, sizeof(enc), 0, payload, sizeof(payload), true);
    check(n == 0, "kiss-simple: FEND in payload refused");

    /* Two frames back to back, and one split across a FEND */
    n = kiss_enc_data(enc, sizeof(enc), 0, payload, 3, false);
    size_t n2 = kiss_enc_data(enc + n - 1, sizeof(enc) - n + 1, 0, payload, 4,
                              false);
    kiss_dec_init(&d, false);
    int data = 0;
    for (size_t i = 0; i < (n + n2 - 1); i++)
        if (kiss_dec_byte(&d, enc[i]) == KISS_EV_DATA)
            data++;
    check(data == 2, "kiss: back-to-back frames");

    /* Cheese */
    n = kiss_enc_cheese(enc, sizeof(enc), 10);
    check((n == 10) && (enc[9] == KISS_FEND), "kiss: cheese");
}

/* Loopback: KISS bytes -> AX.25 -> modem -> demodulator -> AX.25 -> KISS */
static void loopOnce(pkt_modem_t mode, size_t infoLen, unsigned trial)
{
    static int16_t audio[FS * 4];
    static uint8_t frame[HDLC_MAX_LEN];
    uint8_t info[AX25_MAX_INFO];

    for (size_t i = 0; i < infoLen; i++)
        info[i] = (uint8_t)('A' + (rnd() % 26));

    /* Bytes that must survive both the KISS escaping and the HDLC stuffing */
    const uint8_t specials[] = { 0xC0, 0xDB, 0x7E, 0xDC, 0xDD };
    for (size_t i = 0; i < sizeof(specials); i++)
        if ((trial % 3) == 0 && infoLen > 8)
            info[5 + i] = specials[i];

    size_t frameLen = buildFrame(frame, sizeof(frame), "APRS", "N0CALL-1",
                                 info, infoLen);
    check(frameLen > 0, "loop: frame built");

    /* The host hands the frame over without the FCS, as KISS defines */
    size_t kissLen = frameLen - 2;

    uint8_t kiss[600];
    size_t kn = kiss_enc_data(kiss, sizeof(kiss), 0, frame, kissLen, false);
    check(kn > 0, "loop: KISS encoded");

    /* Decode the KISS stream (what the TNC does on its serial port) */
    kiss_dec_t d;
    kiss_dec_init(&d, false);
    kiss_ev_t ev = KISS_EV_NONE;
    for (size_t i = 0; i < kn; i++)
    {
        kiss_ev_t got = kiss_dec_byte(&d, kiss[i]);
        if (got != KISS_EV_NONE)
            ev = got;
    }
    check((ev == KISS_EV_DATA) && (d.msg.len == kissLen),
          "loop: KISS decoded");

    size_t n = modulate(mode, d.msg.data, d.msg.len, audio, FS * 4, 300);
    check(n > 0, "loop: modulated");

    rxSink_t sink;
    int got = demodulate(mode, audio, n, &sink);
    check(got == 1, "loop: one frame received");

    if (got == 1)
    {
        check(sink.len == kissLen, "loop: frame length");
        check(memcmp(sink.data, frame, kissLen) == 0, "loop: frame bytes");

        /* And back out to the host as KISS */
        uint8_t out[600];
        size_t on = kiss_enc_data(out, sizeof(out), 0, sink.data, sink.len,
                                  false);
        check(on > 0, "loop: KISS re-encoded");
    }
}

static int cmdLoop(int argc, char **argv)
{
    pkt_modem_t mode = PKT_MODEM_AFSK1200;
    unsigned trials = 3;

    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--modem") && (i + 1 < argc))
            mode = parseModem(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && (i + 1 < argc))
            trials = (unsigned)atoi(argv[++i]);
    }

    static const size_t lengths[] = { 20, 64, 200, 256 };

    for (unsigned t = 0; t < trials; t++)
    {
        for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++)
            loopOnce(mode, lengths[l], t);
    }

    printf("loop %s: %d checks, %d failures\n", pkt_modem_name(mode), checks,
           failures);
    return failures ? 1 : 0;
}

/* ----------------------------------------------------------------- BER --- */

/* ----------------------------------------------------------------- BER --- */

/*
 * Bit error rate against additive white Gaussian noise.
 *
 * The bits are a pseudo random sequence, modulated and pushed through the
 * channel one bit at a time; the demodulator's decisions are collected and
 * compared with what was sent. The comparison is aligned by searching the
 * delay that minimises the errors (the receive filter and the cell decision
 * delay the stream by a few bits), and both the raw decisions (before the
 * descrambler) and the descrambled data bits are reported: one raw error
 * becomes up to three data errors, which is what the G3RUH scrambler does.
 */

typedef struct
{
    g3ruh_demod_t *dem;
    bool          *raw;
    bool          *data;
    long           n;
    long           cap;
} berSink_t;

static void onBerBit(bool bit, void *user)
{
    berSink_t *s = (berSink_t *)user;

    if (s->n >= s->cap)
        return;

    s->raw[s->n]  = s->dem->lastBit;
    s->data[s->n] = bit;
    s->n++;
}

static int cmdBer(int argc, char **argv)
{
    pkt_modem_t mode = PKT_MODEM_AFSK9600;
    double from = 0.0, to = 16.0, step = 2.0;
    long bits = 20000;

    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--modem") && (i + 1 < argc))
            mode = parseModem(argv[++i]);
        else if (!strcmp(argv[i], "--from") && (i + 1 < argc))
            from = atof(argv[++i]);
        else if (!strcmp(argv[i], "--to") && (i + 1 < argc))
            to = atof(argv[++i]);
        else if (!strcmp(argv[i], "--step") && (i + 1 < argc))
            step = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bits") && (i + 1 < argc))
            bits = atol(argv[++i]);
    }

    const uint32_t spb = pkt_modem_spb(mode, FS);
    if (spb == 0)
    {
        fprintf(stderr, "modem %s has no serial rate\n", pkt_modem_name(mode));
        return 2;
    }

    const long warmup = 300;              /* bits for the clock to settle */
    const long total  = bits + warmup;

    bool *txData = calloc((size_t)total, sizeof(bool));
    bool *txScr  = calloc((size_t)total, sizeof(bool));
    bool *rxRaw  = calloc((size_t)total + 64, sizeof(bool));
    bool *rxData = calloc((size_t)total + 64, sizeof(bool));

    if (!txData || !txScr || !rxRaw || !rxData)
        return 2;

    printf("# %s bit error rate, %ld bits per point, %u samples per bit\n",
           pkt_modem_name(mode), bits, spb);
    printf("# snr_dB  ebno_dB  ber_raw  ber_data  delay  bits\n");

    for (double snr = from; snr <= to + 0.001; snr += step)
    {
        g3ruh_mod_t mod;
        g3ruh_demod_t dem;
        afsk_mod_t amod;
        afsk_demod_t adem;

        g3ruh_mod_init(&mod, FS);
        g3ruh_mod_setGain(&mod, 0.8f);
        g3ruh_mod_reset(&mod);
        g3ruh_demod_init(&dem, FS);

        afsk_mod_init(&amod, FS, 0.0f);   /* no pre-emphasis in the test */
        afsk_mod_setGain(&amod, 0.8f);
        afsk_mod_reset(&amod);
        afsk_demod_init(&adem, FS);

        /* Signal power of one bit of the modem, for the SNR reference */
        int16_t tmp[64];
        uint32_t nSamples;

        if (mode == PKT_MODEM_AFSK9600)
        {
            g3ruh_mod_bit(&mod, true, tmp);
            nSamples = spb;
            g3ruh_mod_reset(&mod);        /* the power probe moved it */
        }
        else
        {
            afsk_mod_level(&amod, true, tmp);
            nSamples = amod.samplesPerBit;
            afsk_mod_reset(&amod);
        }

        double power = 0.0;
        for (uint32_t i = 0; i < nSamples; i++)
            power += (double)tmp[i] * (double)tmp[i];
        power /= (double)nSamples;
        if (power < 1.0)
            power = 1.0;

        const double sigma = sqrt(power / pow(10.0, snr / 10.0));

        berSink_t sink = { &dem, rxRaw, rxData, 0, total + 64 };
        long rawErr = 0, dataErr = 0;
        uint32_t nrzi = 1;
        bool prevLevel = false;
        bool haveLevel = false;
        int16_t samples[64];
        double noise[64];

        for (long b = 0; b < total; b++)
        {
            bool dataBit = (rnd() & 1u) != 0;
            txData[b] = dataBit;

            for (uint32_t i = 0; i < nSamples; i++)
                noise[i] = gauss() * sigma;

            if (mode == PKT_MODEM_AFSK9600)
            {
                g3ruh_mod_bit(&mod, dataBit, samples);
                txScr[b] = (mod.lfsr & 1u) != 0;

                for (uint32_t i = 0; i < spb; i++)
                {
                    double v = (double)samples[i] + noise[i];
                    if (v > 32767.0)
                        v = 32767.0;
                    if (v < -32768.0)
                        v = -32768.0;

                    g3ruh_demod_sample(&dem, (int16_t)v, onBerBit, &sink);
                }
            }
            else
            {
                /* 1200: NRZI level modulation, as the APRS path does */
                if (!dataBit)
                    nrzi ^= 1u;

                afsk_mod_level(&amod, nrzi != 0, samples);
                txScr[b] = nrzi != 0;

                for (uint32_t i = 0; i < amod.samplesPerBit; i++)
                {
                    double v = (double)samples[i] + noise[i];
                    if (v > 32767.0)
                        v = 32767.0;
                    if (v < -32768.0)
                        v = -32768.0;

                    bool level;
                    if (afsk_demod_sample(&adem, (int16_t)v, &level))
                    {
                        if (sink.n < sink.cap)
                        {
                            /* The 1200 demodulator outputs the NRZI level;
                             * undo it here so both columns are data bits */
                            sink.raw[sink.n] = level;
                            if (haveLevel)
                            {
                                sink.data[sink.n] = (level == prevLevel);
                                sink.n++;
                            }
                            prevLevel = level;
                            haveLevel = true;
                        }
                    }
                }
            }
        }

        /*
         * Align: the demodulator's bit k is the transmitter's bit k - delay,
         * with a delay of a few bits (the receive filter plus the cell
         * decision). Search both signs for the alignment with the fewest raw
         * errors and measure the data errors on that one.
         */
        int bestDelay = 0;
        long bestErr = 1L << 30;
        long counted = 0;

        for (int delay = -40; delay <= 40; delay++)
        {
            long err = 0, c = 0;

            for (long i = warmup; (i < total) && (i < sink.n); i++)
            {
                long j = i - delay;
                if ((j < warmup) || (j >= total))
                    continue;
                if (sink.raw[i] != txScr[j])
                    err++;
                c++;
            }

            if ((c > (bits / 2)) && (err < bestErr))
            {
                bestErr   = err;
                bestDelay = delay;
                counted   = c;
            }
        }

        for (long i = warmup; (i < total) && (i < sink.n); i++)
        {
            long j = i - bestDelay;
            if ((j < warmup) || (j >= total))
                continue;
            if (sink.data[i] != txData[j])
                dataErr++;
        }

        rawErr = bestErr;
        if (counted == 0)
            counted = 1;

        double ebno = snr + 10.0 * log10((double)spb);
        printf("%6.1f  %7.1f  %7.2e  %8.2e  %5d  %ld\n", snr, ebno,
               (double)rawErr / (double)counted,
               (double)dataErr / (double)counted, bestDelay, counted);
    }

    free(txData);
    free(txScr);
    free(rxRaw);
    free(rxData);
    return 0;
}

/* ----------------------------------------------------------------- WAV --- */

static int cmdWav(int argc, char **argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: wav <out.wav> <1200|9600> <string> ...\n");
        return 2;
    }

    pkt_modem_t mode = parseModem(argv[3]);
    size_t cap = FS * 8 * (size_t)(argc - 4);
    int16_t *audio = calloc(cap, sizeof(int16_t));
    if (audio == NULL)
        return 2;

    size_t n = 0;
    size_t used = 0;

    for (int i = 4; i < argc; i++)
    {
        uint8_t frame[HDLC_MAX_LEN];
        size_t frameLen = buildFrame(frame, sizeof(frame), "APRS", "N0CALL-1",
                                     (const uint8_t *)argv[i], strlen(argv[i]));
        if (frameLen == 0)
            continue;

        size_t w = modulate(mode, frame, frameLen - 2, audio + n, cap - n, 300);
        n += w;
        used++;
        /* 200 ms between packets */
        size_t gap = FS / 5;
        if (n + gap < cap)
        {
            memset(audio + n, 0, gap * sizeof(int16_t));
            n += gap;
        }
    }

    bool ok = wav_save(argv[2], audio, n, FS);
    printf("%s: %zu packets, %zu samples (%.2f s) at %u Hz\n", argv[2], used,
           n, (double)n / (double)FS, FS);
    free(audio);
    return ok ? 0 : 1;
}

static int cmdDecode(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: decode <in.wav> <1200|9600>\n");
        return 2;
    }

    wav_t w;
    if (!wav_load(argv[2], &w))
    {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }

    pkt_modem_t mode = parseModem(argv[3]);
    printf("%s: %zu samples at %u Hz, %s\n", argv[2], w.count, w.rate,
           pkt_modem_name(mode));

    /* The engine wants its own rate: resample by decimation if needed */
    int16_t *samples = w.samples;
    size_t count = w.count;

    if (w.rate != FS)
    {
        /* Only the rates that divide evenly: 24000 -> 48000 by repetition is
         * not needed for a check on the 9600 path, and 44100 has no integer
         * relation at all, so refuse instead of pretending. */
        fprintf(stderr, "sample rate %u not supported, use %u\n", w.rate, FS);
        wav_free(&w);
        return 2;
    }

    rxSink_t sink;
    memset(&sink, 0, sizeof(sink));
    int got = demodulate(mode, samples, count, &sink);
    printf("%d frame(s)\n", got);

    if (got > 0)
    {
        printf("first frame, %zu bytes:", sink.len);
        for (size_t i = 0; i < sink.len; i++)
            printf(" %02X", sink.data[i]);
        printf("\n");

        ax25_frame_t f;
        if (ax25_parse(sink.data, sink.len, &f))
        {
            char dst[16], src[16];
            ax25_addr_format(dst, sizeof(dst), &f.dst);
            ax25_addr_format(src, sizeof(src), &f.src);
            printf("%s>%s:%.*s\n", src, dst, (int)f.infoLen,
                   (const char *)f.info);
        }
    }

    wav_free(&w);
    return (got > 0) ? 0 : 1;
}

/* ------------------------------------------------------------ selftest --- */

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr,
                "usage: %s selftest|loop|ber|wav|decode ...\n", argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "selftest"))
    {
        testKissCodec();
        loopOnce(PKT_MODEM_AFSK1200, 32, 0);
        loopOnce(PKT_MODEM_AFSK9600, 32, 0);
        printf("selftest: %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }

    if (!strcmp(argv[1], "loop"))
        return cmdLoop(argc, argv);
    if (!strcmp(argv[1], "ber"))
        return cmdBer(argc, argv);
    if (!strcmp(argv[1], "wav"))
        return cmdWav(argc, argv);
    if (!strcmp(argv[1], "decode"))
        return cmdDecode(argc, argv);

    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
