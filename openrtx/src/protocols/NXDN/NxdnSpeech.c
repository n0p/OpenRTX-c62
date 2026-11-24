/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file NxdnSpeech.c
 * @brief NXDN voice playback, see NxdnSpeech.h.
 *
 * Same shape as the DMR speech path (protocols/DMR/DmrSpeech.c): the rtx thread
 * fills a small queue and a playback thread owns the decoder and the speaker.
 * The vocoder is stateful and not reentrant, so it is called from the playback
 * thread only.
 */

#include "protocols/NXDN/NxdnSpeech.h"

#ifdef CONFIG_NXDN

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "core/audio_path.h"
#include "core/psram_state.h"
#include "core/audio_stream.h"
#include "dmr_vocoder.h"
#include "interfaces/delays.h"

/** Queue depth: 32 frames are 640 ms, a whole superframe plus slack. */
#define QUEUE_FRAMES 32
/** Frames to buffer before opening the speaker: one frame, 80 ms. */
#define PREBUFFER_FRAMES NXDN_VCH_PER_FRAME
/** Frames of silence before the speaker is released: about one second. */
#define SILENCE_FRAMES 50
#define THREAD_STACK 8192

static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
/*
 * The playout queue: 2.2 KB of AMBE frames, written by the RTX thread and
 * popped by the playback thread once per 20 ms slot.  PSRAM, as the YSF
 * playout queue already is; the PCM buffer handed to audioStream_start()
 * stays in SRAM.
 */
static uint8_t queue[QUEUE_FRAMES][NXDN_AMBE_BYTES] PSRAM_STATE;
static unsigned queueHead;
static unsigned queueCount;

static int16_t pcm[2 * DMR_VOCODER_PCM_SAMPLES];

static pthread_t thread;
static pthread_attr_t attr;
static volatile bool running;
static volatile bool stopRequest;

static volatile uint32_t statQueued;
static volatile uint32_t statDropped;
static volatile uint32_t statPlayed;
static volatile uint32_t statUnderruns;
static volatile uint32_t statBursts;

static unsigned queued(void)
{
    pthread_mutex_lock(&queueMutex);
    unsigned n = queueCount;
    pthread_mutex_unlock(&queueMutex);
    return n;
}

static bool pop(uint8_t frame[NXDN_AMBE_BYTES])
{
    bool ok = false;

    pthread_mutex_lock(&queueMutex);
    if (queueCount > 0) {
        memcpy(frame, queue[queueHead], NXDN_AMBE_BYTES);
        queueHead = (queueHead + 1) % QUEUE_FRAMES;
        queueCount--;
        ok = true;
    }
    pthread_mutex_unlock(&queueMutex);

    return ok;
}

/**
 * Play queued speech until a second of silence, then release the speaker.
 */
static void playBurst(void)
{
    statBursts++;

    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    if (audioPath_getStatus(path) != PATH_OPEN) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    memset(pcm, 0, sizeof(pcm));
    streamId stream = audioStream_start(path, pcm, 2 * DMR_VOCODER_PCM_SAMPLES,
                                        DMR_VOCODER_SAMPLE_RATE,
                                        STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (stream < 0) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    /* A new transmission: do not carry the previous talker's pitch and phase */
    dmr_vocoder_reset();
    outputStream_sync(stream, false);

    unsigned silence = 0;
    while ((!stopRequest) && (silence < SILENCE_FRAMES)
           && (audioPath_getStatus(path) == PATH_OPEN)) {
        stream_sample_t *out = outputStream_getIdleBuffer(stream);
        if (out == NULL)
            break;

        uint8_t frame[NXDN_AMBE_BYTES];
        if (pop(frame)) {
            if (dmr_vocoder_decode(frame, out) == 0)
                statPlayed++;
            else
                memset(out, 0, DMR_VOCODER_PCM_SAMPLES * sizeof(*out));
            silence = 0;
        } else {
            memset(out, 0, DMR_VOCODER_PCM_SAMPLES * sizeof(*out));
            statUnderruns++;
            silence++;
        }

        outputStream_sync(stream, true);
    }

    audioStream_stop(stream);
    audioPath_release(path);
}

static void *playback(void *arg)
{
    (void)arg;

    while (!stopRequest) {
        if (queued() >= PREBUFFER_FRAMES)
            playBurst();
        else
            sleepFor(0, 10);
    }

    return NULL;
}

void nxdnSpeech_start(void)
{
    if (running)
        return;

    pthread_mutex_lock(&queueMutex);
    queueHead = 0;
    queueCount = 0;
    pthread_mutex_unlock(&queueMutex);

    statQueued = 0;
    statDropped = 0;
    statPlayed = 0;
    statUnderruns = 0;
    statBursts = 0;
    stopRequest = false;

    pthread_attr_init(&attr);
#ifdef __ZEPHYR__
    void *stack = malloc(THREAD_STACK);
    if (stack == NULL)
        return;
    pthread_attr_setstack(&attr, stack, THREAD_STACK);
#else
    pthread_attr_setstacksize(&attr, THREAD_STACK);
#endif

    running = pthread_create(&thread, &attr, playback, NULL) == 0;
#ifdef __ZEPHYR__
    if (!running)
        free(stack);
#endif
}

void nxdnSpeech_stop(void)
{
    if (!running)
        return;

    stopRequest = true;
    pthread_join(thread, NULL);
    running = false;

#ifdef __ZEPHYR__
    void *stack;
    size_t size;
    pthread_attr_getstack(&attr, &stack, &size);
    free(stack);
#endif
}

void nxdnSpeech_push(const uint8_t frames[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES],
                     unsigned count)
{
    if (count > NXDN_VCH_PER_FRAME)
        count = NXDN_VCH_PER_FRAME;

    pthread_mutex_lock(&queueMutex);
    for (unsigned i = 0; i < count; i++) {
        if (queueCount == QUEUE_FRAMES) {
            /* Overflow: drop the oldest frame, keep the audio running */
            queueHead = (queueHead + 1) % QUEUE_FRAMES;
            queueCount--;
            statDropped++;
        }
        memcpy(queue[(queueHead + queueCount) % QUEUE_FRAMES], frames[i],
               NXDN_AMBE_BYTES);
        queueCount++;
        statQueued++;
    }
    pthread_mutex_unlock(&queueMutex);
}

void nxdnSpeech_stats(uint32_t *queuedFrames, uint32_t *playedFrames,
                      uint32_t *droppedFrames, uint32_t *underruns,
                      uint32_t *bursts)
{
    *queuedFrames = statQueued;
    *playedFrames = statPlayed;
    *droppedFrames = statDropped;
    *underruns = statUnderruns;
    *bursts = statBursts;
}

#else  /* CONFIG_NXDN */

void nxdnSpeech_start(void)
{
}

void nxdnSpeech_stop(void)
{
}

void nxdnSpeech_push(const uint8_t frames[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES],
                     unsigned count)
{
    (void)frames;
    (void)count;
}

void nxdnSpeech_stats(uint32_t *queuedFrames, uint32_t *playedFrames,
                      uint32_t *droppedFrames, uint32_t *underruns,
                      uint32_t *bursts)
{
    *queuedFrames = 0;
    *playedFrames = 0;
    *droppedFrames = 0;
    *underruns = 0;
    *bursts = 0;
}

#endif /* CONFIG_NXDN */
