/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DStarSpeech.c
 * @brief D-STAR voice playback, see DStarSpeech.h.
 */

#include "protocols/DSTAR/DStarSpeech.h"

#ifdef CONFIG_DSTAR

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/stack_budget.h"
#include "interfaces/delays.h"
#include "protocols/DSTAR/DStarVocoder.h"

/** Frames to buffer before opening the speaker: 60 ms. */
#define PREBUFFER 3
/** Frames of silence before the speaker is released: about a second. */
#define SILENCE_FRAMES 50
/* The size and its floor live in core/stack_budget.h: parms_repeat_model alone
 * puts 2 456 B of frames on this thread at Cortex-M33 -Os. */
#define THREAD_STACK STACK_BUDGET_DSTAR_SPEECH

static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t queue[DSTAR_SPEECH_QUEUE][DSTAR_VOICE_BYTES];
static unsigned queueHead;
static unsigned queueCount;

static int16_t pcm[2 * DSTAR_PCM_SAMPLES];

static pthread_t thread;
static pthread_attr_t attr;
static volatile bool running;
static volatile bool stopRequest;

static volatile uint32_t statQueued;
static volatile uint32_t statPlayed;
static volatile uint32_t statDropped;

void dstarSpeech_push(const uint8_t frame[DSTAR_VOICE_BYTES])
{
    pthread_mutex_lock(&queueMutex);
    if (queueCount == DSTAR_SPEECH_QUEUE) {
        queueHead = (queueHead + 1) % DSTAR_SPEECH_QUEUE;
        queueCount--;
        statDropped++;
    }
    memcpy(queue[(queueHead + queueCount) % DSTAR_SPEECH_QUEUE], frame,
           DSTAR_VOICE_BYTES);
    queueCount++;
    statQueued++;
    pthread_mutex_unlock(&queueMutex);
}

static bool pop(uint8_t frame[DSTAR_VOICE_BYTES])
{
    bool got = false;

    pthread_mutex_lock(&queueMutex);
    if (queueCount > 0) {
        memcpy(frame, queue[queueHead], DSTAR_VOICE_BYTES);
        queueHead = (queueHead + 1) % DSTAR_SPEECH_QUEUE;
        queueCount--;
        got = true;
    }
    pthread_mutex_unlock(&queueMutex);
    return got;
}

/**
 * Play queued speech until there is a second of nothing to play, then hand the
 * speaker back.
 *
 * Same shape as NxdnSpeech.c's playBurst(), which is what this file should
 * have been: one over owns the path and the stream from the first prebuffered
 * frame to the last silent one, decodes into the idle half of the stream and
 * hands it over with sync(stream, true).  Writing into pcm[0..159] and syncing
 * with bufChanged = false played the first half of a double-buffered stream
 * and left the other one as whatever it held: 20 ms of audio, 20 ms of
 * nothing, which is the "it decodes but does not sound" report.
 */
static void playBurst(void)
{
    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);

    if (audioPath_getStatus(path) != PATH_OPEN) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    memset(pcm, 0, sizeof(pcm));
    streamId stream = audioStream_start(path, pcm, 2 * DSTAR_PCM_SAMPLES,
                                        DSTAR_VOCODER_SAMPLE_RATE,
                                        STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (stream < 0) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    /* A new transmission is a new talker: no previous pitch, phases or noise */
    dstarVocoder_reset();
    outputStream_sync(stream, false);

    unsigned silence = 0;
    while ((!stopRequest) && (silence < SILENCE_FRAMES)
           && (audioPath_getStatus(path) == PATH_OPEN)) {
        stream_sample_t *out = outputStream_getIdleBuffer(stream);
        if (out == NULL)
            break;

        uint8_t frame[DSTAR_VOICE_BYTES];
        if (pop(frame)) {
            if (dstarVocoder_decode(frame, out) != 0)
                memset(out, 0, DSTAR_PCM_SAMPLES * sizeof(*out));
            silence = 0;
            statPlayed++;
        } else {
            memset(out, 0, DSTAR_PCM_SAMPLES * sizeof(*out));
            silence++;
        }

        outputStream_sync(stream, true);
    }

    audioStream_stop(stream);
    audioPath_release(path);
}

static void *playbackThread(void *arg)
{
    (void)arg;

    while (!stopRequest) {
        unsigned pending;
        pthread_mutex_lock(&queueMutex);
        pending = queueCount;
        pthread_mutex_unlock(&queueMutex);

        if (pending < PREBUFFER) {
            sleepFor(0, 10);
            continue;
        }

        playBurst();
    }

    running = false;
    return NULL;
}

void dstarSpeech_start(void)
{
    if (running)
        return;

    stopRequest = false;
    queueHead = 0;
    queueCount = 0;
    statQueued = statPlayed = statDropped = 0;

    /*
     * The decoder's state sits in PSRAM, which the C62 does not clear at
     * start-up (lib/dstar_vocoder/dstar_vocoder.h).  Whatever the previous
     * owner left there -- or the boot loader -- is pitch, phases and noise
     * gains until it is initialised, and a NaN in any of them poisons every
     * frame after it.  This is the only place the decoder is brought up.
     */
    dstarVocoder_reset();

    pthread_attr_init(&attr);
#ifdef __ZEPHYR__
    /*
     * Zephyr's pthread_create() refuses an attribute set without a stack
     * buffer (EINVAL, lib/posix/pthread.c), and this thread had one attribute
     * set and no stack: the call failed every time and nothing played.  The
     * stack comes from the heap as in NxdnSpeech.c, and a failure no longer
     * passes unnoticed: `running` stays false, the queue fills and drops its
     * oldest frames, and the mode keeps what it never needed the speaker for
     * (callsigns, slow data).
     */
    void *stack = malloc(THREAD_STACK);

    if (stack == NULL)
        return;

    pthread_attr_setstack(&attr, stack, THREAD_STACK);
#else
    pthread_attr_setstacksize(&attr, THREAD_STACK);
#endif

    running = pthread_create(&thread, &attr, playbackThread, NULL) == 0;
#ifdef __ZEPHYR__
    if (!running)
        free(stack);
#endif
}

void dstarSpeech_stop(void)
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

void dstarSpeech_stats(uint32_t *queued, uint32_t *played, uint32_t *dropped)
{
    if (queued != NULL)
        *queued = statQueued;
    if (played != NULL)
        *played = statPlayed;
    if (dropped != NULL)
        *dropped = statDropped;
}

#else /* !CONFIG_DSTAR */

void dstarSpeech_start(void) {}
void dstarSpeech_stop(void) {}
void dstarSpeech_push(const uint8_t frame[DSTAR_VOICE_BYTES])
{
    (void)frame;
}
void dstarSpeech_stats(uint32_t *queued, uint32_t *played, uint32_t *dropped)
{
    (void)queued;
    (void)played;
    (void)dropped;
}

#endif /* CONFIG_DSTAR */
