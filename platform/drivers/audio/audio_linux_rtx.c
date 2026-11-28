/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file audio_linux_rtx.c
 * @brief The emulator's transmitter and microphone: the two endpoints the C62
 *        has and the host did not.
 *
 * The Linux platform lists SINK_RTX and SOURCE_MIC among its audio endpoints
 * but had no driver behind either, so audioStream_start() answered -ENODEV for
 * every mode that renders its waveform into a stream.  FM keys the PA without a
 * stream and worked; DMR, NXDN, dPMR, D-STAR, YSF and P25 do not, and on the
 * host they all stopped at the same call -- one level below anything a mode
 * decides, which is why the emulator could never be asked what the radio was
 * doing.
 *
 * These two drivers are the C62's codec seen from the streams' side:
 *
 *  - playback into SINK_RTX: buffer in halves, the driver "plays" the half the
 *    caller has just filled and hands back the other one.  The number of halves
 *    and the peak they carried are what a test needs to say that samples left
 *    the mode and that they were not silence.
 *  - capture from SOURCE_MIC: 8 kHz of a square wave, so a voice path has
 *    something to encode that is not the same frame over and over.
 *
 * Deliberately no wall-clock pacing: the C62's sync() waits for the codec, and
 * here the "codec" is this function, so waiting would only make the bench slow
 * and its numbers non-deterministic.  The modes do not use sync() for timing,
 * they use it for the buffer boundary.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "interfaces/audio.h"
#include "emulator/txbench.h"

/* ------------------------------------------------------------------- state */

static unsigned benchBlocks;      ///< Halves the playback driver has taken.
static unsigned benchSamples;     ///< Samples those halves carried.
static unsigned benchMicSamples;  ///< Samples the microphone has handed out.
static int      benchPeak;        ///< Loudest sample played, absolute value.
static int      injectedStartRc;  ///< Non-zero: the playback driver refuses.
static bool     injectedRxDead;   ///< The receiver's stream refuses to start.

/* ------------------------------------------------------------------ playback */

static uint8_t sinkHalf; ///< Half the caller may fill next.

static int sinkStart(const uint8_t instance, const void *config,
                     struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    /* A codec that does not take the route: what the tests ask for to prove
     * that the mode names the call which failed. */
    if (injectedStartRc < 0)
        return injectedStartRc;

    ctx->priv    = NULL;
    ctx->running = 1;
    sinkHalf     = 0;

    return 0;
}

static int sinkData(struct streamCtx *ctx, stream_sample_t **buf)
{
    const size_t half = ctx->bufSize / 2;

    if ((ctx->running == 0) || (half == 0) || (ctx->buffer == NULL))
    {
        *buf = NULL;
        return -EIO;
    }

    *buf = ctx->buffer + (size_t)sinkHalf * half;

    return (int)half;
}

static int sinkSync(struct streamCtx *ctx, uint8_t dirty)
{
    const size_t half = ctx->bufSize / 2;

    if (ctx->running == 0)
        return -1;

    if ((dirty != 0) && (half > 0) && (ctx->buffer != NULL))
    {
        const stream_sample_t *played = ctx->buffer + (size_t)sinkHalf * half;

        for (size_t i = 0; i < half; i++)
        {
            const int v = (played[i] < 0) ? -(int)played[i] : (int)played[i];

            if (v > benchPeak)
                benchPeak = v;
        }

        benchBlocks++;
        benchSamples += (unsigned)half;
        sinkHalf ^= 1;
    }

    return 0;
}

static void sinkStop(struct streamCtx *ctx)
{
    ctx->running = 0;
}

static void sinkTerminate(struct streamCtx *ctx)
{
    ctx->running = 0;
}

const struct audioDriver audio_linux_rtxSinkDriver = {
    .start     = sinkStart,
    .data      = sinkData,
    .sync      = sinkSync,
    .stop      = sinkStop,
    .terminate = sinkTerminate,
};

/* ------------------------------------------------------------------- capture */

/*
 * The receiver's discriminator: SOURCE_RTX.
 *
 * On the radio this endpoint always exists -- the codec captures every frame
 * into the application buffers -- so a mode's receive loop always has samples
 * to read.  The host pointed it at a capture file that usually is not there
 * (/tmp/baseband.raw), the stream failed to start, and every mode that returns
 * early when no samples arrive never got past its own receive code: P25 and YSF
 * check the PTT at the end of that path, so on the host they could not key for
 * a reason that had nothing to do with the PTT.
 *
 * This driver keeps the capture file when it exists (the emulator's own
 * feature) and is a quiet noise floor when it does not, pacing like the codec
 * so the RTX loop runs at its real rate instead of spinning.
 */

static int rxSource_start(const uint8_t instance, const void *config,
                          struct streamCtx *ctx)
{
    (void)instance;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    /* A receiver whose stream does not start: the codec route the worker did
     * not apply, which is what the bench asks for to prove that a dead receiver
     * does not take the transmitter with it. */
    if (injectedRxDead)
        return -EIO;

    ctx->priv    = NULL;
    ctx->running = 1;

    /* An optional capture: silence is a valid discriminator, a missing file is
     * not an error here. */
    if (config != NULL)
        ctx->priv = fopen((const char *)config, "rb");

    return 0;
}

static uint32_t rxNoiseState = 12345u;

static int rxSource_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if ((ctx->running == 0) || (ctx->bufSize < 2) || (ctx->buffer == NULL))
    {
        *buf = NULL;
        return -EIO;
    }

    const size_t half = ctx->bufSize / 2;
    stream_sample_t *out = ctx->buffer;

    FILE *fp = (FILE *)ctx->priv;
    if (fp != NULL)
    {
        size_t filled = 0;

        while (filled < half)
        {
            const size_t n = fread(out + filled, sizeof(stream_sample_t),
                                   half - filled, fp);

            if (n == 0)
            {
                if (fseek(fp, 0, SEEK_SET) != 0)
                    break; /* not seekable: stop serving the file */
            }
            else
            {
                filled += n;
            }
        }

        if (filled < half)
        {
            fclose(fp);
            ctx->priv = NULL;
        }
    }

    if (ctx->priv == NULL)
    {
        /* +-1 LSB of noise: a receiver with nothing on the channel, which is
         * what the modes have to decode as "no sync" without stopping. */
        for (size_t i = 0; i < half; i++)
        {
            rxNoiseState = rxNoiseState * 1664525u + 1013904223u;
            out[i] = (stream_sample_t)((rxNoiseState >> 31) & 1u);
        }
    }

    *buf = out;

    return (int)half;
}

static int rxSource_sync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    if (ctx->running == 0)
        return -1;

    /* The codec hands over one half per period: pace the caller like it. */
    const size_t half = ctx->bufSize / 2;
    if ((ctx->sampleRate != 0) && (half != 0))
        usleep((useconds_t)((1000000ULL * half) / ctx->sampleRate));

    return 0;
}

static void rxSource_stop(struct streamCtx *ctx)
{
    if (ctx->priv != NULL)
        fclose((FILE *)ctx->priv);

    ctx->priv    = NULL;
    ctx->running = 0;
}

const struct audioDriver audio_linux_rxSourceDriver = {
    .start     = rxSource_start,
    .data      = rxSource_data,
    .sync      = rxSource_sync,
    .stop      = rxSource_stop,
    .terminate = rxSource_stop,
};

/* ------------------------------------------------------------------- capture */

/*
 * 20 ms of 8 kHz: the AMBE+2 frame every mode's voice path consumes.  A square
 * wave at 25 Hz, loud enough that an encoder sees something and quiet enough
 * that the modulator is not driven to its limit by the test itself.
 */
#define MIC_HALF_SAMPLES 160

static stream_sample_t micBuffer[2 * MIC_HALF_SAMPLES];
static uint32_t micPhase;

static int micStart(const uint8_t instance, const void *config,
                    struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    ctx->priv    = NULL;
    ctx->running = 1;

    return 0;
}

static int micData(struct streamCtx *ctx, stream_sample_t **buf)
{
    if ((ctx->running == 0) || (ctx->bufSize < 2))
    {
        *buf = NULL;
        return -EIO;
    }

    /* In circular double buffered mode the driver hands out one half at a
     * time, the one the capture has just filled -- which is what
     * inputStream_getData() expects to receive. */
    const size_t half = ctx->bufSize / 2;
    stream_sample_t *out = (ctx->buffer != NULL) ? ctx->buffer : micBuffer;

    for (size_t i = 0; i < half; i++)
    {
        micPhase = (micPhase + 1) % 320;
        out[i] = (micPhase < 160) ? 2000 : -2000;
    }

    benchMicSamples += (unsigned)half;
    *buf = out;

    return (int)half;
}

static int micSync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    return (ctx->running == 0) ? -1 : 0;
}

static void micStop(struct streamCtx *ctx)
{
    ctx->running = 0;
}

static void micTerminate(struct streamCtx *ctx)
{
    ctx->running = 0;
}

const struct audioDriver audio_linux_micDriver = {
    .start     = micStart,
    .data      = micData,
    .sync      = micSync,
    .stop      = micStop,
    .terminate = micTerminate,
};

/* --------------------------------------------------------------------- knobs */

void txbench_failStreamStart(int rc)
{
    injectedStartRc = (rc < 0) ? rc : 0;
}

void txbench_failReceiveStream(bool dead)
{
    injectedRxDead = dead;
}

void txbench_clear(void)
{
    benchBlocks     = 0;
    benchSamples    = 0;
    benchMicSamples = 0;
    benchPeak       = 0;
    injectedStartRc = 0;
    injectedRxDead  = false;
}

unsigned txbench_blocks(void)
{
    return benchBlocks;
}

unsigned txbench_samples(void)
{
    return benchSamples;
}

unsigned txbench_micSamples(void)
{
    return benchMicSamples;
}

int txbench_peak(void)
{
    return benchPeak;
}
