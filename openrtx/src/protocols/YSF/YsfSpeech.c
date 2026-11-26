/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file YsfSpeech.c
 * @brief System Fusion voice playback, see YsfSpeech.h.
 *
 * The queue holds voice slots, not decoded audio: a slot is 49 or 72 bits and
 * a flag saying which, so the playback thread can take either System Fusion
 * voice format through the one vocoder.
 */

#include "protocols/YSF/YsfSpeech.h"
#include "protocols/YSF/YsfMem.h"

#ifdef CONFIG_YSF

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/stack_budget.h"
#include "dmr_decoder.h"
#include "dmr_vocoder.h"
#include "interfaces/delays.h"

/** Queue depth: 20 slots are 400 ms, four frames of margin over a lost one. */
#define QUEUE_SLOTS 20
/** Slots to buffer before opening the speaker: one frame, 100 ms. */
#define PREBUFFER_SLOTS YSF_VOICE_SLOTS
/** Silent slots before the speaker is released: about one second. */
#define SILENCE_SLOTS 50
/* The size and its floor live in core/stack_budget.h. */
#define THREAD_STACK STACK_BUDGET_IMBE_SPEECH

static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
/* Queue and PCM scratch: 2.1 KB touched once per 20 ms slot, so PSRAM.  Both
 * are written before they are read (the queue only pops what it pushed, pcm is
 * cleared before the stream starts). */
static struct ysfVoiceSlot queue[QUEUE_SLOTS] YSF_EXT_RAM;
static unsigned queueHead;
static unsigned queueCount;

static int16_t pcm[2 * DMR_VOCODER_PCM_SAMPLES] YSF_EXT_RAM;

static pthread_t thread;
static pthread_attr_t attr;
static volatile bool running;
static volatile bool stopRequest;

static volatile unsigned statPlayed;
static volatile unsigned statDropped;
static volatile unsigned statUnderruns;

static unsigned queued(void)
{
    pthread_mutex_lock(&queueMutex);
    unsigned n = queueCount;
    pthread_mutex_unlock(&queueMutex);

    return n;
}

static bool pop(struct ysfVoiceSlot *slot)
{
    bool ok = false;

    pthread_mutex_lock(&queueMutex);
    if (queueCount > 0) {
        *slot = queue[queueHead];
        queueHead = (queueHead + 1) % QUEUE_SLOTS;
        queueCount--;
        ok = true;
    }
    pthread_mutex_unlock(&queueMutex);

    return ok;
}

/** Decode one slot into 160 samples of PCM. */
static int decodeSlot(const struct ysfVoiceSlot *slot, int16_t *out)
{
    if (slot->nbits == 72) {
        /* V/D mode 1: the canonical 72-bit AMBE+2 channel frame */
        uint8_t frame[DMR_FRAME_BYTES];

        memset(frame, 0, sizeof(frame));
        for (int i = 0; i < 72; i++)
            frame[i >> 3] = (uint8_t)((frame[i >> 3] << 1) | (slot->bits[i] & 1u));

        return dmr_vocoder_decode(frame, out);
    }

    if (slot->nbits == 49) {
        /* V/D mode 2: the parameter bits, already protected by the mode */
        return dmr_vocoder_decode_ambe(slot->bits, out);
    }

    return -1;
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
    streamId stream = audioStream_start(path, pcm, 2 * DMR_VOCODER_PCM_SAMPLES,
                                        DMR_VOCODER_SAMPLE_RATE,
                                        STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (stream < 0) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    /*
     * No reset here: a System Fusion transmission is a stream of 20 ms voice
     * slots and a gap inside it is a lost frame, not a new talker.  Resetting
     * on every gap makes a healthy decoder sound four times quiet (the cold
     * fade-in the DMR path already paid for); ysfSpeech_reset() is the only
     * way to start from scratch.
     */
    outputStream_sync(stream, false);

    unsigned silence = 0;
    while ((!stopRequest) && (silence < SILENCE_SLOTS)
           && (audioPath_getStatus(path) == PATH_OPEN)) {
        stream_sample_t *out = outputStream_getIdleBuffer(stream);
        if (out == NULL)
            break;

        struct ysfVoiceSlot slot;
        if (pop(&slot)) {
            if (decodeSlot(&slot, out) == 0)
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
        if (queued() >= PREBUFFER_SLOTS)
            playCall();
        else
            sleepFor(0, 10);
    }

    return NULL;
}

void ysfSpeech_start(void)
{
    if (running)
        return;

    pthread_mutex_lock(&queueMutex);
    queueHead = 0;
    queueCount = 0;
    pthread_mutex_unlock(&queueMutex);

    statPlayed = 0;
    statDropped = 0;
    statUnderruns = 0;
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

    if (pthread_create(&thread, &attr, playback, NULL) == 0) {
        running = true;
        dmr_vocoder_reset();
    }
}

void ysfSpeech_stop(void)
{
    if (!running)
        return;

    stopRequest = true;
    pthread_join(thread, NULL);
    running = false;
}

void ysfSpeech_flush(void)
{
    pthread_mutex_lock(&queueMutex);
    queueHead = 0;
    queueCount = 0;
    pthread_mutex_unlock(&queueMutex);
}

void ysfSpeech_reset(void)
{
    ysfSpeech_flush();
    dmr_vocoder_reset();
}

void ysfSpeech_push(const struct ysfVoiceSlot *slots, unsigned count)
{
    pthread_mutex_lock(&queueMutex);
    for (unsigned i = 0; i < count; i++) {
        if (queueCount >= QUEUE_SLOTS) {
            /* Full: drop the oldest, the talker keeps talking */
            queueHead = (queueHead + 1) % QUEUE_SLOTS;
            queueCount--;
            statDropped++;
        }
        queue[(queueHead + queueCount) % QUEUE_SLOTS] = slots[i];
        queueCount++;
    }
    pthread_mutex_unlock(&queueMutex);
}

void ysfSpeech_stats(unsigned *played, unsigned *dropped)
{
    if (played != NULL)
        *played = statPlayed;
    if (dropped != NULL)
        *dropped = statDropped;
}

#else /* CONFIG_YSF */

void ysfSpeech_start(void) {}
void ysfSpeech_stop(void) {}
void ysfSpeech_flush(void) {}
void ysfSpeech_reset(void) {}
void ysfSpeech_push(const struct ysfVoiceSlot *slots, unsigned count)
{
    (void)slots;
    (void)count;
}
void ysfSpeech_stats(unsigned *played, unsigned *dropped)
{
    if (played != NULL)
        *played = 0;
    if (dropped != NULL)
        *dropped = 0;
}

#endif /* CONFIG_YSF */
