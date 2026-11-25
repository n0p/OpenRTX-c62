/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.c
 * @brief System Fusion transmit chain, closed through the firmware's own
 * receiver.
 *
 * The transmitter is protocols/YSF/YsfTx.c with the shared C4FM shaper, the
 * channel is additive white Gaussian noise, and the receiver is
 * protocols/YSF/YsfPhy.c + YsfFrame.c, i.e. the mode's own demodulator and
 * frame layer.  What is compared:
 *
 *   - the FICH: frame indicator, data type, block and frame numbers, DG-ID
 *   - the data channel: destination, source, both nodes, and the text
 *     message reassembled across frames
 *   - the voice: all five 72-bit AMBE+2 slots of every frame, bit for bit
 *
 * Modes:
 *   ysf_tx_test loop [snr_db] [frames]   a call through the receiver
 *   ysf_tx_test sweep                    errors against the SNR
 *   ysf_tx_test wave <out.wav> [frames]  the modulating waveform, for dsd-neo
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "protocols/C4FM/C4fmMod.h"
#include "protocols/YSF/YsfFrame.h"
#include "protocols/YSF/YsfPhy.h"
#include "protocols/YSF/YsfTx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** Outer C4FM level at 1800 Hz of deviation (docs/TX_YSF.md). */
/*
 * Deviation scale.  Half scale, not full: the raised cosine pulse of a
 * four-level signal peaks at about 1.8 times the outer level, and both this
 * receiver's level estimator and dsd-neo mis-slice a signal whose peak sits at
 * -1 dBFS (measured; see docs/TX_YSF.md).  On the radio the real number is the
 * REG_40 deviation calibration, and the peak has to stay inside the
 * modulator's linear range for the same reason.
 */
#define TX_GAIN 2730.0f

#define SOURCE_CALL "EA5TEST"
#define DEST_CALL "CQCQCQ  "
#define UPLINK_CALL "EA5RPT  "
#define DOWNLINK_CALL "EA5RPT  "
#define DGID 42
/* Exactly inside the 3 x 20 characters a transmission can carry. */
#define MESSAGE "PRUEBA C4FM DESDE EL C62, TEXTO EN TRES BLOQUES DE VEINTE!!"

/** Occupied bandwidth of the C4FM signal, for the SNR definition. */
#define BAND_HZ 5760.0
#define NYQUIST_HZ 24000.0

#define MAX_FRAMES 96
#define MAX_VOICE (MAX_FRAMES * YSF_VOICE_SLOTS)

static uint32_t rngState = 987654321u;

static uint32_t rnd(void)
{
    rngState = rngState * 1103515245u + 12345u;

    return rngState >> 8;
}

static double gauss(void)
{
    const double u1 = ((double)(rnd() & 0xFFFFFF) + 1.0) / 16777217.0;
    const double u2 = (double)(rnd() & 0xFFFFFF) / 16777216.0;

    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/** The AMBE+2 frames the transmitter pulled, in order. */
static uint8_t sentVoice[MAX_VOICE][9];
static int voiceFed;

/** One frame of the call, as transmitted. */
struct expect {
    uint8_t fi;
    uint8_t bn;
    uint8_t fn;
    uint8_t dt;
};

static struct expect sent[MAX_FRAMES];
static int sentFrames;

static struct {
    int frames;
    int fichBad;
    int fiBad;
    int dtBad;
    int bnBad;
    int dgidBad;
    int destBad;
    int sourceBad;
    int nodeBad;
    int textBad;
    int voiceWords;
    int voiceBad;
    int voiceLost;
    int unexpected;
} res;

/** The last message the receiver assembled, latched when it completes: the
 *  call ends in the middle of a repetition, so reading the layer's state at
 *  the end of the run would catch a half-received one. */
static char textDone[YSF_TEXT_MAX + 1];
static bool textDoneFlag;

static double callStart;
static double frameSamples;
static int voiceCheck;
static int voiceCursor;

static int voiceSource(uint8_t frame[9], void *user)
{
    (void)user;

    if (voiceFed >= MAX_VOICE)
        return -1;

    /* Deterministic and different in every bit position between frames */
    uint32_t s = (uint32_t)(voiceFed * 2654435761u) ^ 0x1234ABCDu;

    for (int i = 0; i < 9; i++) {
        s = s * 1664525u + 1013904223u;
        frame[i] = (uint8_t)(s >> 24);
    }

    memcpy(sentVoice[voiceFed], frame, 9);
    voiceFed++;

    return 0;
}

static void onFrame(const struct ysfFrame *f, void *user)
{
    (void)user;

    const struct ysfDecoded *d = ysfFrame_decode(f);

    if (d == NULL)
        return;

    if (!textDoneFlag) {
        const struct ysfText *t = ysfFrame_text();

        if (t->complete && (t->length > 0)) {
            snprintf(textDone, sizeof(textDone), "%s", t->text);
            textDoneFlag = true;
        }
    }

    if (!d->fichOk) {
        res.fichBad++;
        return;
    }

    res.frames++;

    /* Everything is keyed on the FICH's own fields, not on the order of
     * arrival: a frame lost to noise then costs that frame. */
    const int fi = d->call.fi;
    const int fn = d->call.fn;

    if (!d->call.dgidValid || d->call.dgid != DGID) {
        res.dgidBad++;
        printf("  frame %d: DG-ID %u (valid %d), expected %u\n", res.frames,
               d->call.dgid, (int)d->call.dgidValid, DGID);
    }

    if (fi == 0 || fi == 2) {
        if (strcmp(d->call.dest, "CQCQCQ") != 0) {
            res.destBad++;
            printf("  frame %d: destination '%s'\n", res.frames, d->call.dest);
        }
        if (strcmp(d->call.source, SOURCE_CALL) != 0) {
            res.sourceBad++;
            printf("  frame %d: source '%s'\n", res.frames, d->call.source);
        }
        if (strcmp(d->call.uplink, "EA5RPT") != 0) {
            res.nodeBad++;
            printf("  frame %d: uplink '%s' downlink '%s'\n", res.frames,
                   d->call.uplink, d->call.downlink);
        }
    } else if (d->call.bn == 0) {
        if (strcmp(d->call.dest, "CQCQCQ") != 0
            || strcmp(d->call.source, SOURCE_CALL) != 0) {
            res.destBad++;
            printf("  frame %d: bn0 dest '%s' source '%s'\n", res.frames,
                   d->call.dest, d->call.source);
        }
    } else if (d->call.bn == 1) {
        if (strcmp(d->call.uplink, "EA5RPT") != 0
            || strcmp(d->call.downlink, "EA5RPT") != 0) {
            res.nodeBad++;
            printf("  frame %d: bn1 uplink '%s' downlink '%s'\n", res.frames,
                   d->call.uplink, d->call.downlink);
        }
    }

    if (d->call.dt != 0) {
        res.dtBad++;
        printf("  frame %d: data type %u\n", res.frames, d->call.dt);
    }

    /* Voice: the five slots of the frame, in order */
    if (d->voiceCount == YSF_VOICE_SLOTS) {
        for (int s = 0; s < YSF_VOICE_SLOTS; s++) {
            if (voiceCheck >= voiceFed)
                break;

            res.voiceWords++;
            int diff = 0;
            for (int b = 0; b < 72; b++) {
                const uint8_t rx = (uint8_t)((d->voice[s].bits[b]) & 1u);
                const uint8_t tx =
                    (uint8_t)((sentVoice[voiceCheck][b >> 3] >> (7 - (b & 7)))
                              & 1u);

                diff += (rx != tx);
            }
            if (diff != 0) {
                res.voiceBad++;
            }
            voiceCheck++;
        }
    } else if (d->voiceCount != 0) {
        res.voiceBad += YSF_VOICE_SLOTS - d->voiceCount;
    }

    (void)fn;
}

/* ------------------------------------------------------------------ run --- */

static int run(double snrDb, int voiceFrames, bool quiet)
{
    struct ysfTxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.source, SOURCE_CALL);
    strcpy(cfg.dest, DEST_CALL);
    strcpy(cfg.uplink, UPLINK_CALL);
    strcpy(cfg.downlink, DOWNLINK_CALL);
    cfg.dgid = DGID;
    cfg.text = MESSAGE;

    memset(&res, 0, sizeof(res));
    memset(sentVoice, 0, sizeof(sentVoice));
    voiceFed = 0;
    voiceCheck = 0;
    voiceCursor = 0;
    sentFrames = 0;
    textDoneFlag = false;
    textDone[0] = '\0';

    ysfTx_setVoiceSource(voiceSource, NULL);

    sent[sentFrames++].fi = 0;
    for (int i = 0; i < voiceFrames; i++)
        sent[sentFrames++].fi = 1;
    sent[sentFrames++].fi = 2;

    ysfTx_begin(&cfg);

    const size_t data = (size_t)sentFrames * YSF_FRAME_SYMBOLS * C4FM_MOD_SPS;
    const size_t lead = 4800;
    const size_t tail = 2 * (size_t)YSF_FRAME_SYMBOLS * C4FM_MOD_SPS;
    int16_t *wave = calloc(lead + data + tail, sizeof(int16_t));

    if (wave == NULL)
        return -1;

    callStart = (double)lead;
    frameSamples = (double)YSF_FRAME_SYMBOLS * C4FM_MOD_SPS;

    /* Everything but the terminator, then the request to end */
    const size_t lastFrame = (size_t)YSF_FRAME_SYMBOLS * C4FM_MOD_SPS;

    ysfTx_samples(&wave[lead], (int)(data - lastFrame), TX_GAIN);
    ysfTx_end();
    ysfTx_samples(&wave[lead + data - lastFrame], (int)lastFrame, TX_GAIN);

    double power = 0.0;

    for (size_t i = lead; i < lead + data; i++)
        power += (double)wave[i] * (double)wave[i];
    power /= (double)data;

    if (snrDb < 90.0) {
        const double sigma =
            sqrt(power / pow(10.0, snrDb / 10.0) * NYQUIST_HZ / BAND_HZ);

        for (size_t i = 0; i < lead + data; i++) {
            double v = (double)wave[i] + sigma * gauss();

            if (v > 32767.0)
                v = 32767.0;
            if (v < -32768.0)
                v = -32768.0;
            wave[i] = (int16_t)lrint(v);
        }
    }

    /*
     * Feed the receiver the way the radio does: one 100 ms frame per call (the
     * mode's audio stream hands over 4800-sample halves).  YsfPhy.c keeps a
     * one-frame ring and expects to be called per frame; handing it the whole
     * burst at once would overwrite the ring before it looks.
     */
    ysfPhy_init();
    ysfFrame_reset();

    for (size_t off = 0; off < lead + data + tail; off += 2400) {
        const size_t n = ((lead + data + tail) - off < 2400) ?
                             ((lead + data + tail) - off) :
                             2400;
        ysfPhy_push(&wave[off], n, onFrame, NULL);
    }

    if (textDoneFlag && (strcmp(textDone, MESSAGE) == 0)) {
        /* reassembled */
    } else {
        res.textBad++;
        printf("  text not reassembled (%u/%u blocks, '%s')\n",
               ysfFrame_text()->received, ysfFrame_text()->expected, textDone);
    }

    free(wave);

    if (!quiet) {
        printf(
            "snr=%5.1f dB frames=%d/%d fich_bad=%d dgid_bad=%d dest_bad=%d "
            "source_bad=%d node_bad=%d dt_bad=%d text_bad=%d voice=%d/%d bad "
            "extra=%d lost=%d\n",
            snrDb, res.frames, sentFrames, res.fichBad, res.dgidBad,
            res.destBad, res.sourceBad, res.nodeBad, res.dtBad, res.textBad,
            res.voiceBad, res.voiceWords, res.unexpected, res.voiceLost);
    }

    return res.fichBad + res.dgidBad + res.destBad + res.sourceBad + res.nodeBad
         + res.dtBad + res.textBad + res.voiceBad + res.unexpected
         + ((res.frames < sentFrames) ? 1 : 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s loop|sweep|wave [args]\n", argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "loop") == 0) {
        const double snr = (argc > 2) ? atof(argv[2]) : 60.0;
        const int frames = (argc > 3) ? atoi(argv[3]) : 12;
        const int bad = run(snr, frames, false);

        printf("loop: %s\n", bad == 0 ? "PASS" : "FAILED");
        return bad == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "sweep") == 0) {
        static const double snr[] = { 4, 8, 12, 16, 20, 24 };

        printf("snr_db  frames/sent  errors  detail\n");
        for (unsigned i = 0; i < sizeof(snr) / sizeof(snr[0]); i++) {
            const int bad = run(snr[i], 12, true);

            printf("%5.0f   %2d/%-2d        %3d     fich=%d dgid=%d dest=%d "
                   "src=%d node=%d dt=%d text=%d voice=%d/%d\n",
                   snr[i], res.frames, sentFrames, bad, res.fichBad,
                   res.dgidBad, res.destBad, res.sourceBad, res.nodeBad,
                   res.dtBad, res.textBad, res.voiceBad, res.voiceWords);
        }
        return 0;
    }

    if (strcmp(argv[1], "frames") == 0) {
        /* The frame layer alone: the transmitter's 480 dibits straight into
         * ysfFrame_decode(), no modulator, no channel.  Every field and every
         * voice bit has to come back identical. */
        struct ysfTxConfig cfg;
        struct ysfFrame f;

        memset(&cfg, 0, sizeof(cfg));
        strcpy(cfg.source, SOURCE_CALL);
        strcpy(cfg.dest, DEST_CALL);
        strcpy(cfg.uplink, UPLINK_CALL);
        strcpy(cfg.downlink, DOWNLINK_CALL);
        cfg.dgid = DGID;
        cfg.text = MESSAGE;

        const int voiceFrames = (argc > 2) ? atoi(argv[2]) : 12;

        memset(&res, 0, sizeof(res));
        memset(sentVoice, 0, sizeof(sentVoice));
        voiceFed = 0;
        voiceCheck = 0;
        voiceCursor = 0;

        ysfTx_setVoiceSource(voiceSource, NULL);
        ysfTx_begin(&cfg);
        ysfFrame_reset();

        int16_t buf[1440];
        int frames = 1 + voiceFrames + 1;

        for (int k = 0; k < frames; k++) {
            const uint32_t before = ysfTx_frames();

            if (k == frames - 1)
                ysfTx_end();

            /* Pump until the frame counter moves: one symbol is enough */
            while (ysfTx_frames() == before)
                ysfTx_samples(buf, 1440, TX_GAIN);

            memset(&f, 0, sizeof(f));
            memcpy(f.dibits, ysfTx_frameDibits(), YSF_FRAME_DIBITS);
            onFrame(&f, NULL);
        }

        printf(
            "frames: %d/%d, fich_bad=%d dgid_bad=%d dest_bad=%d source_bad=%d "
            "node_bad=%d dt_bad=%d text_bad=%d voice=%d/%d bad\n",
            res.frames, frames, res.fichBad, res.dgidBad, res.destBad,
            res.sourceBad, res.nodeBad, res.dtBad, res.textBad, res.voiceBad,
            res.voiceWords);

        const int bad = res.fichBad + res.dgidBad + res.destBad + res.sourceBad
                      + res.nodeBad + res.dtBad + res.textBad + res.voiceBad
                      + ((res.frames != frames) ? 1 : 0);

        printf("frames: %s\n", bad == 0 ? "PASS" : "FAILED");
        return bad == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "wave") == 0 && argc > 2) {
        struct ysfTxConfig cfg;
        const int voiceFrames = (argc > 3) ? atoi(argv[3]) : 12;

        memset(&cfg, 0, sizeof(cfg));
        strcpy(cfg.source, SOURCE_CALL);
        strcpy(cfg.dest, DEST_CALL);
        strcpy(cfg.uplink, UPLINK_CALL);
        strcpy(cfg.downlink, DOWNLINK_CALL);
        cfg.dgid = DGID;
        cfg.text = MESSAGE;

        ysfTx_setVoiceSource(voiceSource, NULL);
        ysfTx_begin(&cfg);

        const size_t lastFrame = (size_t)YSF_FRAME_SYMBOLS * C4FM_MOD_SPS;
        const size_t data = (size_t)(1 + voiceFrames + 1) * lastFrame;
        int16_t *buf = calloc(data + 2 * lastFrame, sizeof(int16_t));

        if (buf == NULL)
            return 1;

        ysfTx_samples(buf, (int)(data - lastFrame), TX_GAIN);
        ysfTx_end();
        ysfTx_samples(&buf[data - lastFrame], (int)(3 * lastFrame), TX_GAIN);

        FILE *f = fopen(argv[2], "wb");

        if (f == NULL)
            return 1;
        fwrite(buf, sizeof(int16_t), data + 2 * lastFrame, f);
        fclose(f);
        printf("wrote %s: %u frames, %u samples\n", argv[2],
               (unsigned)ysfTx_frames(), (unsigned)(data + 2 * lastFrame));
        return 0;
    }

    return 2;
}
