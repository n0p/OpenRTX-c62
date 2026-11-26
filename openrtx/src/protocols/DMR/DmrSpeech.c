/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/DmrSpeech.h"

#if defined(DMR_VOICE_ENABLED) || defined(DMR_VOICE_OWN)

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#define STATS_PRINT printk
#else
#define STATS_PRINT printf
#endif
#include <string.h>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "interfaces/delays.h"
#include "dmr_port.h"
#include "dmr_voice.h"

#define FRAME_SAMPLES 160 /* 20 ms at 8 kHz */
#define BURST_FRAMES 3    /* One voice burst carries 60 ms of speech */
#define BURST_SAMPLES (BURST_FRAMES * FRAME_SAMPLES)
/* Speech arrives in 60 ms bursts: wait for three before playing */
#define PREBUFFER_BURSTS 3
/*
 * Silence before the speaker is let go. Three seconds, not the one it was:
 * on a busy channel the gaps between transmissions run one to two seconds,
 * so at one second the output track was torn down and built again for
 * nearly every over, and starting it costs some 170 ms of audio - the start
 * of each transmission was clipped. The amplifier stays on a little longer
 * between overs, which is the trade.
 */
#define SILENCE_BURSTS 50 /* About three seconds */
#ifdef DMR_VOICE_ENABLED
/* mbelib's synthesis keeps a few KB of arrays on the stack */
#define THREAD_STACK 8192
#else
/*
 * lib/dmr_vocoder keeps its scratch in the instance (DTCM, dmr_voice.c), but
 * the synthesis is not flat: dmr_rfft256 puts 128+128 floats of its own on the
 * stack and synth_unvoiced calls it with the frame's buffers live.  A DMR
 * session with 4 KB overflowed exactly there -- the black box caught
 * K_ERR_STACK_CHK_FAIL with PC in dmr_rfft256 and LR in synth_unvoiced -- so
 * the thread gets what DTCM can still hold: 16 KB total, 8.5 KB of decoder
 * state beside it, 672 B left over after this.
 */
#define THREAD_STACK 7168
#endif
#define STATS_MS 10000

/*
 * The audio driver plays the circular buffer continuously, like a DMA: a
 * half not refilled in time is played again. Each half holds a whole burst,
 * decoded straight into it, so the vocoder has the 60 ms of the other half
 * to decode three frames.
 */
static stream_sample_t pcm[2 * BURST_SAMPLES];
static stream_sample_t *target; /* Half being filled by the vocoder sink */
static unsigned targetFrames;   /* Frames written to it */

static pthread_t thread;
static pthread_attr_t attr;
static volatile bool running;
static volatile bool stopRequest;
static volatile bool playing; /* Speech on the speaker */

static uint32_t lateBursts;   /* Halves refilled after they started playing */
#ifdef CONFIG_C62_VOICE_STATS
static long long nextStats;
#endif

/**
 * Vocoder sink: one 20 ms frame, +-1.0 float, to 16-bit samples.
 */
static void sink(const float *in, int n)
{
    if (target == NULL || targetFrames >= BURST_FRAMES || n != FRAME_SAMPLES)
        return;

    stream_sample_t *out = target + targetFrames++ * FRAME_SAMPLES;
    for (int i = 0; i < n; i++) {
        float v = in[i] * 32767.0f;
        if (v > 32767.0f)
            v = 32767.0f;
        else if (v < -32768.0f)
            v = -32768.0f;
        out[i] = (stream_sample_t)v;
    }
}

/**
 * Decode the next queued burst into a buffer half.
 *
 * @return false if no speech is queued (the half is then silent).
 */
static bool fillBurst(stream_sample_t *out)
{
    target = out;
    targetFrames = 0;
    bool ok = dmr_voice_decode_next();
    target = NULL;

    if (!ok)
        targetFrames = 0;
    if (targetFrames < BURST_FRAMES)
        memset(out + targetFrames * FRAME_SAMPLES, 0,
               (BURST_FRAMES - targetFrames) * FRAME_SAMPLES * sizeof(*out));
    return ok;
}

#if defined(__ZEPHYR__) && defined(CONFIG_THREAD_MONITOR) \
    && defined(CONFIG_THREAD_RUNTIME_STATS) && defined(CONFIG_C62_VOICE_STATS)
#include <zephyr/kernel.h>
/*
 * CPU use of every thread over the last period, on the console: which
 * threads take the CPU, and which of them run above the vocoder and keep
 * it waiting. Up to CPU_THREADS threads are followed.
 */
#define CPU_THREADS 32
static struct cpuThread {
    const struct k_thread *thread;
    uint64_t last;   /* execution cycles at the previous period */
    uint64_t delta;  /* during the last period */
} cpuThreads[CPU_THREADS];
static unsigned cpuCount;

static void cpuSample(const struct k_thread *thread, void *user)
{
    (void)user;
    k_thread_runtime_stats_t stats;
    if (k_thread_runtime_stats_get((k_tid_t)thread, &stats) != 0)
        return;
    struct cpuThread *t = NULL;
    for (unsigned i = 0; i < cpuCount; i++)
        if (cpuThreads[i].thread == thread)
            t = &cpuThreads[i];
    if (t == NULL) {
        if (cpuCount == CPU_THREADS)
            return;
        t = &cpuThreads[cpuCount++];
        t->thread = thread;
        t->last = stats.execution_cycles;
    }
    t->delta = stats.execution_cycles - t->last;
    t->last = stats.execution_cycles;
}

static void printCpu(void)
{
    for (unsigned i = 0; i < cpuCount; i++)
        cpuThreads[i].delta = 0;
    k_thread_foreach(cpuSample, NULL);
    uint64_t total = 0;
    for (unsigned i = 0; i < cpuCount; i++)
        total += cpuThreads[i].delta;
    if (total == 0)
        return;
    /* The six busiest, with their priority; "*" marks this thread */
    char line[200];
    int len = snprintf(line, sizeof(line), "CPU 10 s:");
    bool used[CPU_THREADS] = { false };
    for (unsigned n = 0; n < 6 && len > 0 && (size_t)len < sizeof(line); n++) {
        int best = -1;
        for (unsigned i = 0; i < cpuCount; i++)
            if (!used[i] && (best < 0
                             || cpuThreads[i].delta > cpuThreads[best].delta))
                best = (int)i;
        if (best < 0 || cpuThreads[best].delta == 0)
            break;
        used[best] = true;
        k_tid_t tid = (k_tid_t)cpuThreads[best].thread;
        const char *name = k_thread_name_get(tid);
        const unsigned permille =
            (unsigned)(cpuThreads[best].delta * 1000u / total);
        len += snprintf(line + len, sizeof(line) - (size_t)len,
                        " %s%s(p%d) %u.%u%%",
                        tid == k_current_get() ? "*" : "",
                        name && name[0] ? name : "?",
                        k_thread_priority_get(tid), permille / 10,
                        permille % 10);
    }
    STATS_PRINT("%s\n", line);
}
#else
static void printCpu(void)
{
}
#endif

/**
 * Vocoder and playback figures on the console, every STATS_MS.
 */
static void printStats(void)
{
#ifdef CONFIG_C62_VOICE_STATS
    long long now = getTick();
    if (now < nextStats)
        return;
    nextStats = now + STATS_MS;
    printCpu();

    uint32_t frames, avgUs, maxUs, drops;
    dmr_voice_get_timing(&frames, &avgUs);
    dmr_voice_get_debug(&maxUs, &drops);
    /* Bit error rate over this period, from the frame FEC corrections:
     * hundredths of a percent of the 47 protected bits per frame */
    static uint32_t lastFecBits, lastFecFrames;
    uint32_t fecBits, fecFrames;
    dmr_voice_get_fec(&fecBits, &fecFrames);
    const uint32_t dBits = fecBits - lastFecBits;
    const uint32_t dFrames = fecFrames - lastFecFrames;
    lastFecBits = fecBits;
    lastFecFrames = fecFrames;
    const uint32_t ber = dFrames ? (dBits * 10000u) / (dFrames * 47u) : 0;
    uint32_t ownAvg, ownMax;
    dmr_voice_get_own(&ownAvg, &ownMax);
    STATS_PRINT("DMR voice: %lu frames, %lu us avg, %lu us max (budget 20000), "
                "late %lu, queue drops %lu, concealed %lu | BER %lu.%02lu %% "
                "en %lu tramas | propio %lu us avg, %lu us max\n",
                (unsigned long)frames, (unsigned long)avgUs,
                (unsigned long)maxUs, (unsigned long)lateBursts,
                (unsigned long)drops, (unsigned long)dmr_voice_get_concealed(),
                (unsigned long)(ber / 100u), (unsigned long)(ber % 100u),
                (unsigned long)dFrames, (unsigned long)ownAvg,
                (unsigned long)ownMax);
#endif /* CONFIG_C62_VOICE_STATS */
}

/**
 * Play queued speech until a second of silence, then release the speaker.
 */
static void playBurst(void)
{
    /*
     * Both of these used to fail without a word, retrying every 100 ms for
     * ever: speech was decoded and never heard, with nothing in the log to
     * say why. Said once, and again only after it has worked in between.
     */
    static bool pathSaid, streamSaid;

    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    if (audioPath_getStatus(path) != PATH_OPEN) {
        if (!pathSaid) {
            pathSaid = true;
            STATS_PRINT("DMR voice: sin altavoz (ruta %d): sin audio\n",
                        (int)path);
        }
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }
    pathSaid = false;

    /* Both halves start with speech: the first two bursts */
    fillBurst(pcm);
    fillBurst(pcm + BURST_SAMPLES);
    streamId stream = audioStream_start(path, pcm, 2 * BURST_SAMPLES, 8000,
                                        STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (stream < 0) {
        if (!streamSaid) {
            streamSaid = true;
            STATS_PRINT("DMR voice: el flujo de salida no abrio (%d): sin "
                        "audio\n", (int)stream);
        }
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }
    streamSaid = false;

    outputStream_sync(stream, false);
    playing = true;

    unsigned silence = 0;
    while (!stopRequest && silence < SILENCE_BURSTS
           && audioPath_getStatus(path) == PATH_OPEN) {
        stream_sample_t *out = outputStream_getIdleBuffer(stream);
        if (out == NULL)
            break;

        long long t0 = getTick();
        if (fillBurst(out))
            silence = 0;
        else
            silence++;
        if (getTick() - t0 >= 60)
            lateBursts++;

        if (!outputStream_sync(stream, true))
            break;
        printStats();
    }

    playing = false;
    audioStream_stop(stream);
    audioPath_release(path);
}

bool dmrSpeech_playing(void)
{
    return playing;
}

static void *playback(void *arg)
{
    (void)arg;
    dmr_voice_set_sink(sink);
    while (!stopRequest) {
        if (dmr_voice_queued() >= PREBUFFER_BURSTS)
            playBurst();
        else
            sleepFor(0, 10);
        printStats();
    }
    dmr_voice_set_sink(NULL);
    return NULL;
}

void dmrSpeech_start(void)
{
    if (running)
        return;

    stopRequest = false;

    pthread_attr_init(&attr);
#ifdef __ZEPHYR__
    /*
     * A static stack, not malloc()ed: on the C62 malloc() takes from PSRAM,
     * and mbelib keeps its synthesis arrays on the stack - from PSRAM it
     * took 16 ms of each 20 ms frame. On the C62 in DTCM (dmr_port.h).
     */
    static uint8_t stack[THREAD_STACK] DMR_FAST_BSS __attribute__((aligned(8)));
    pthread_attr_setstack(&attr, stack, THREAD_STACK);
#else
    pthread_attr_setstacksize(&attr, THREAD_STACK);
#endif
    /*
     * Above the UI thread, as the TETRA speech thread: the UI redraws the
     * screen every 100 ms (bit-banged SPI) at the same priority, and with no
     * time slicing the vocoder waited for each redraw - 16 ms per 20 ms
     * frame with the screen on, against 1.5 ms of its own work. It blocks
     * on the audio buffer and sleeps when idle, so it never starves the UI.
     */
    /*
     * POSIX 2, as the TETRA speech thread (TetraSpeech.c): Zephyr 12, above
     * the rtx thread even while TETRA and T.HAM hold it at 13, and above the
     * ui thread at 14. It used to ask for POSIX 1, which lands on 13 itself:
     * level with the rtx thread of those modes and, with no time slicing,
     * starved by it. TETRA speech was never hit because it already sat at
     * 12 - which is why TETRA could be heard and NXDN could not.
     */
    struct sched_param priority = { .sched_priority = 2 };
    pthread_attr_setschedparam(&attr, &priority);
    const int rc = pthread_create(&thread, &attr, playback, NULL);
    running = rc == 0;
    /* Without the thread nothing ever reaches the speaker while everything
     * else (sync, call IDs, the screen) keeps working: say so, it used to
     * fail without a word */
    if (!running)
        STATS_PRINT("DMR voice: el hilo de voz no arranco (%d): sin audio\n",
                    rc);
#if defined(__ZEPHYR__) && defined(CONFIG_THREAD_NAME)
    if (running)
        pthread_setname_np(thread, "voz");
#endif
}

void dmrSpeech_stop(void)
{
    if (!running)
        return;

    stopRequest = true;
    pthread_join(thread, NULL);
    running = false;
}

#else  /* DMR_VOICE_ENABLED */

bool dmrSpeech_playing(void)
{
    return false;
}

void dmrSpeech_start(void)
{
}

void dmrSpeech_stop(void)
{
}

#endif /* DMR_VOICE_ENABLED */
