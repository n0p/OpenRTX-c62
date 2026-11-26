/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DpmrSpeech.c
 * @brief dPMR voice playback, see DpmrSpeech.h.
 *
 * Same shape as the TETRA speech path (protocols/TETRA/TetraSpeech.c): the rtx
 * thread fills a small queue and a playback thread owns the decoder and the
 * speaker.  The vocoder is stateful and not reentrant, so it is called from
 * the playback thread only.
 */

#include "protocols/DPMR/DpmrSpeech.h"
#include "protocols/DPMR/DpmrPort.h"

#ifdef CONFIG_DPMR

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/stack_budget.h"
#include "dmr_vocoder.h"
#include "interfaces/delays.h"

/** Queue depth: 32 frames are 640 ms, enough to ride out a lost frame. */
#define QUEUE_FRAMES 32
/** Frames to buffer before opening the speaker: one TCH field, 80 ms. */
#define PREBUFFER_FRAMES DPMR_AMBE_FRAMES
/** Frames of silence before the speaker is released: about one second. */
#define SILENCE_FRAMES 50
/* The size and its floor live in core/stack_budget.h. */
#define THREAD_STACK STACK_BUDGET_AMBE_SPEECH

static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * The queue and the playback block are touched once per 20 ms frame at most,
 * so on the C62 they live in PSRAM (DpmrPort.h), which is NOLOAD: both are
 * cleared in dpmrSpeech_start() before the playback thread exists.
 */
static EXT_RAM_BSS_ATTR uint8_t queue[QUEUE_FRAMES][DPMR_AMBE_BYTES];
static unsigned queueHead;
static unsigned queueCount;

static EXT_RAM_BSS_ATTR int16_t pcm[2 * DPMR_PCM_SAMPLES];

static pthread_t thread;
static pthread_attr_t attr;
static volatile bool running;
static volatile bool stopRequest;

/* Statistics, written by both threads, read by the diagnostics */
static volatile uint32_t statQueued;
static volatile uint32_t statDropped;
static volatile uint32_t statPlayed;
static volatile uint32_t statUnderruns;
static volatile uint32_t statFrames;

static unsigned queued(void)
{
    pthread_mutex_lock(&queueMutex);
    unsigned n = queueCount;
    pthread_mutex_unlock(&queueMutex);
    return n;
}

static bool pop(uint8_t frame[DPMR_AMBE_BYTES])
{
    bool ok = false;

    pthread_mutex_lock(&queueMutex);
    if (queueCount > 0) {
        memcpy(frame, queue[queueHead], DPMR_AMBE_BYTES);
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
static void playFrame(void)
{
    statFrames++;

    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    if (audioPath_getStatus(path) != PATH_OPEN) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    memset(pcm, 0, sizeof(pcm));
    streamId stream = audioStream_start(path, pcm, 2 * DPMR_PCM_SAMPLES,
                                        8000,
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

        uint8_t frame[DPMR_AMBE_BYTES];
        if (pop(frame)) {
            if (dmr_vocoder_decode(frame, out) == 0)
                statPlayed++;
            else
                memset(out, 0, DPMR_PCM_SAMPLES * sizeof(*out));
            silence = 0;
        } else {
            memset(out, 0, DPMR_PCM_SAMPLES * sizeof(*out));
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
            playFrame();
        else
            sleepFor(0, 10);
    }

    return NULL;
}

void dpmrSpeech_start(void)
{
    if (running)
        return;

    pthread_mutex_lock(&queueMutex);
    memset(queue, 0, sizeof(queue));
    memset(pcm, 0, sizeof(pcm));
    queueHead = 0;
    queueCount = 0;
    pthread_mutex_unlock(&queueMutex);

    statQueued = 0;
    statDropped = 0;
    statPlayed = 0;
    statUnderruns = 0;
    statFrames = 0;
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

void dpmrSpeech_stop(void)
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

void dpmrSpeech_push(const uint8_t frames[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES],
                    unsigned count)
{
    if (count > DPMR_AMBE_FRAMES)
        count = DPMR_AMBE_FRAMES;

    pthread_mutex_lock(&queueMutex);
    for (unsigned i = 0; i < count; i++) {
        if (queueCount == QUEUE_FRAMES) {
            /* Overflow: drop the oldest frame, keep the audio running */
            queueHead = (queueHead + 1) % QUEUE_FRAMES;
            queueCount--;
            statDropped++;
        }
        memcpy(queue[(queueHead + queueCount) % QUEUE_FRAMES], frames[i],
               DPMR_AMBE_BYTES);
        queueCount++;
        statQueued++;
    }
    pthread_mutex_unlock(&queueMutex);
}

void dpmrSpeech_stats(uint32_t *queuedFrames, uint32_t *playedFrames,
                     uint32_t *droppedFrames, uint32_t *underruns)
{
    *queuedFrames = statQueued;
    *playedFrames = statPlayed;
    *droppedFrames = statDropped;
    *underruns = statUnderruns;
}

#else  /* CONFIG_DPMR */

void dpmrSpeech_start(void)
{
}

void dpmrSpeech_stop(void)
{
}

void dpmrSpeech_push(const uint8_t frames[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES],
                    unsigned count)
{
    (void)frames;
    (void)count;
}

void dpmrSpeech_stats(uint32_t *queuedFrames, uint32_t *playedFrames,
                     uint32_t *droppedFrames, uint32_t *underruns)
{
    *queuedFrames = 0;
    *playedFrames = 0;
    *droppedFrames = 0;
    *underruns = 0;
}

#endif /* CONFIG_DPMR */
