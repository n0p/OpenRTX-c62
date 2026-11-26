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

static void *playbackThread(void *arg)
{
    (void)arg;

    int speakerId = -1;
    int  silence = 0;
    uint8_t frame[DSTAR_VOICE_BYTES];

    while (!stopRequest) {
        unsigned pending;
        pthread_mutex_lock(&queueMutex);
        pending = queueCount;
        pthread_mutex_unlock(&queueMutex);

        if (speakerId < 0) {
            if (pending < PREBUFFER) {
                sleepFor(0, 10);
                continue;
            }
            int path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
            if (path < 0) {
                sleepFor(0, 20);
                continue;
            }
            speakerId = audioStream_start(path, pcm, 2 * DSTAR_PCM_SAMPLES,
                                          DSTAR_VOCODER_SAMPLE_RATE,
                                          STREAM_OUTPUT | BUF_CIRC_DOUBLE);
            if (speakerId < 0) {
                audioPath_release(path);
                sleepFor(0, 20);
                continue;
            }
            silence = 0;
        }

        if (pop(frame)) {
            if (dstarVocoder_decode(frame, pcm) != 0)
                memset(pcm, 0, sizeof(pcm));
            outputStream_sync(speakerId, false);
            silence = 0;
            statPlayed++;
        } else {
            if (++silence < SILENCE_FRAMES) {
                sleepFor(0, 10);
                continue;
            }
            memset(pcm, 0, sizeof(pcm));
            outputStream_sync(speakerId, false);
            audioStream_terminate(speakerId);
            speakerId = -1;
        }
    }

    if (speakerId >= 0)
        audioStream_terminate(speakerId);
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

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    running = true;
    if (pthread_create(&thread, &attr, playbackThread, NULL) != 0)
        running = false;
}

void dstarSpeech_stop(void)
{
    if (!running)
        return;

    stopRequest = true;
    pthread_join(thread, NULL);
    running = false;
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
