/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file P25Speech.c
 * @brief P25 voice playback, see P25Speech.h.
 *
 * The queue holds whole LDUs: nine 11-byte IMBE frames, each carrying a flag so
 * a frame the FEC could not fix still occupies its 20 ms in the timeline.  When
 * no decoder is linked in (p25Vocoder_available() false) the frames are queued
 * and counted but the speaker is never opened: the mode then proves the whole
 * chain up to the vocoder without pretending to play it.
 */

#include "protocols/P25/P25Speech.h"

#ifdef CONFIG_P25

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/stack_budget.h"
#include "interfaces/delays.h"
#include "protocols/P25/P25Vocoder.h"

/** Queue depth: 8 LDUs are 1.4 s, enough to ride out a lost frame. */
#define QUEUE_FRAMES 72
/** Frames buffered before the speaker opens: one LDU. */
#define PREBUFFER_FRAMES P25_FRAMES_PER_LDU
/** Frames of silence before the speaker is released: about one second. */
#define SILENCE_FRAMES 50
/* The size and its floor live in core/stack_budget.h. */
#define THREAD_STACK STACK_BUDGET_IMBE_SPEECH

struct frame {
    uint8_t params[11];
    bool valid;
};

/*
 * The queue and the output buffer are read at 50 Hz, not per sample: they live
 * in PSRAM (NOLOAD, cleared at start) so the mode costs SRAM only for the
 * things a sample touches.
 */
#ifdef __ZEPHYR__
#define P25_SPEECH_RAM __attribute__((section(".psram_section")))
#else
#define P25_SPEECH_RAM
#endif

static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
static struct frame queue[QUEUE_FRAMES] P25_SPEECH_RAM;
static unsigned queueHead;
static unsigned queueCount;

static int16_t pcm[2 * P25_VOCODER_PCM_SAMPLES] P25_SPEECH_RAM;

static pthread_t thread;
static pthread_attr_t attr;
static volatile bool running;
static volatile bool stopRequest;

static volatile uint32_t statQueued;
static volatile uint32_t statPlayed;
static volatile uint32_t statDropped;
static volatile uint32_t statUnderruns;

static unsigned queued(void)
{
    pthread_mutex_lock(&queueMutex);
    unsigned n = queueCount;
    pthread_mutex_unlock(&queueMutex);
    return n;
}

static bool pop(struct frame *out)
{
    bool ok = false;

    pthread_mutex_lock(&queueMutex);
    if (queueCount > 0) {
        *out = queue[queueHead];
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
static void playCall(void)
{
    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    if (audioPath_getStatus(path) != PATH_OPEN) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    memset(pcm, 0, sizeof(pcm));
    streamId stream = audioStream_start(path, pcm, 2 * P25_VOCODER_PCM_SAMPLES,
                                        P25_VOCODER_SAMPLE_RATE,
                                        STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (stream < 0) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    /* A new transmission: do not carry the previous talker's pitch and phase */
    p25Vocoder_reset();
    outputStream_sync(stream, false);

    unsigned silence = 0;
    while ((!stopRequest) && (silence < SILENCE_FRAMES)
           && (audioPath_getStatus(path) == PATH_OPEN)) {
        stream_sample_t *out = outputStream_getIdleBuffer(stream);
        if (out == NULL)
            break;

        struct frame frame;
        if (pop(&frame)) {
            /* A frame the FEC could not fix is handed over with an error count
             * over the repeat threshold: the decoder then repeats the previous
             * model or mutes, TIA-102.BABA 7.7-7.8, instead of the gap being
             * filled with digital silence.  The PHY reports the LDU's total,
             * not each frame's, so a good frame carries 0. */
            int nerr = frame.valid ? 0 : 8;
            if (p25Vocoder_decode_errs(frame.params, nerr, out) >= 0)
                statPlayed++;
            else
                memset(out, 0, P25_VOCODER_PCM_SAMPLES * sizeof(*out));
            silence = 0;
        } else {
            memset(out, 0, P25_VOCODER_PCM_SAMPLES * sizeof(*out));
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
            playCall();
        else
            sleepFor(0, 10);
    }

    return NULL;
}

void p25Speech_start(void)
{
    if (running)
        return;

    pthread_mutex_lock(&queueMutex);
    queueHead = 0;
    queueCount = 0;
    pthread_mutex_unlock(&queueMutex);

    statQueued = 0;
    statPlayed = 0;
    statDropped = 0;
    statUnderruns = 0;
    stopRequest = false;

    if (!p25Vocoder_available())
        return;     /* Nothing to play, nothing to start */

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

void p25Speech_stop(void)
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

void p25Speech_push(const uint8_t params[P25_FRAMES_PER_LDU][11],
                    const bool valid[P25_FRAMES_PER_LDU])
{
    pthread_mutex_lock(&queueMutex);
    for (unsigned i = 0; i < P25_FRAMES_PER_LDU; i++) {
        if (queueCount == QUEUE_FRAMES) {
            /* Overflow: drop the oldest frame, keep the audio running */
            queueHead = (queueHead + 1) % QUEUE_FRAMES;
            queueCount--;
            statDropped++;
        }
        struct frame *slot = &queue[(queueHead + queueCount) % QUEUE_FRAMES];
        memcpy(slot->params, params[i], 11);
        slot->valid = valid[i];
        queueCount++;
        statQueued++;
    }
    pthread_mutex_unlock(&queueMutex);
}

void p25Speech_stats(uint32_t *queuedFrames, uint32_t *playedFrames,
                     uint32_t *droppedFrames, uint32_t *underruns)
{
    *queuedFrames = statQueued;
    *playedFrames = statPlayed;
    *droppedFrames = statDropped;
    *underruns = statUnderruns;
}

#else /* CONFIG_P25 */

void p25Speech_start(void)
{
}

void p25Speech_stop(void)
{
}

void p25Speech_push(const uint8_t params[P25_FRAMES_PER_LDU][11],
                    const bool valid[P25_FRAMES_PER_LDU])
{
    (void)params;
    (void)valid;
}

void p25Speech_stats(uint32_t *queuedFrames, uint32_t *playedFrames,
                     uint32_t *droppedFrames, uint32_t *underruns)
{
    *queuedFrames = 0;
    *playedFrames = 0;
    *droppedFrames = 0;
    *underruns = 0;
}

#endif /* CONFIG_P25 */
