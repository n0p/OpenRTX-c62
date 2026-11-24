/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.c
 * @brief Host harness for the transmit hang: a sink that never consumes.
 *
 * Builds the real core/audio_stream.c and the real rtx/TxGuard.c against a stub
 * audio device, and runs a miniature version of what OpMode_DMR and OpMode_M17
 * do when the PTT is pressed:
 *
 *   PTT -> audioStream_start(SINK_RTX) -> loop { getIdleBuffer(); sync(); }
 *
 * The stub has two modes. In "stalled" mode its sync() never returns, which is
 * what the firmware's DSP side looks like when the playback stream never starts
 * (or the codec worker is stuck inside a DSP call): the caller waits for a
 * buffer boundary that nobody will ever produce. In "running" mode it advances
 * a boundary every slot, and the loop paces at 30 ms per block like the DMR
 * transmitter.
 *
 * Three scenarios:
 *
 *   A. stalled sink, no supervisor  -- what the firmware did: the RTX thread
 *      never comes back, the PA stays keyed, and the radio is hung.
 *   B. stalled sink, supervisor     -- the guard aborts the overs on a deadline
 *      and the supervisor cancels the stream from outside: the caller returns,
 *      the PA is off, and it all happens inside the first-block budget.
 *   C. healthy sink                 -- the numbers the radio has to meet: first
 *      block in well under 300 ms, out of TX in well under 200 ms.
 *
 * Build and run: tests/host/tx_lifecycle_host/run.sh
 * Exit code is 0 only if every scenario behaves as documented.
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "rtx/TxGuard.h"

/* ------------------------------------------------------------- test clock -- */

static uint32_t nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void sleepMs(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000u,
                           .tv_nsec = (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

/* --------------------------------------------------------- stub audio path -- */

/* One path is enough: the harness only opens MCU -> RTX */
static atomic_bool pathOpen;

pathId audioPath_request(enum AudioSource source, enum AudioSink sink,
                         enum AudioPriority prio)
{
    (void)source;
    (void)sink;
    (void)prio;
    atomic_store(&pathOpen, true);
    return 1;
}

pathInfo_t audioPath_getInfo(const pathId id)
{
    pathInfo_t info = { .source = SOURCE_MCU,
                        .sink = SINK_RTX,
                        .prio = PRIO_TX,
                        .status = PATH_CLOSED };
    if ((id == 1) && atomic_load(&pathOpen))
        info.status = PATH_OPEN;
    return info;
}

enum PathStatus audioPath_getStatus(const pathId id)
{
    return audioPath_getInfo(id).status;
}

void audioPath_release(const pathId id)
{
    if (id == 1)
        atomic_store(&pathOpen, false);
}

/* -------------------------------------------------------- stub audio device -- */

#define SLOT_SAMPLES 1440   /* 30 ms at 48 kHz, the DMR slot */
#define SLOT_MS 30

/* What the fake DSP does: consume slots, or stall for ever */
static atomic_bool sinkConsumes;
/* Boundary counter, so a waiter can tell a new one from the last */
static atomic_uint boundaries;
static atomic_uint blocksPlayed;

static int stub_start(const uint8_t instance, const void *config,
                      struct streamCtx *ctx)
{
    (void)instance;
    (void)config;
    /* The real drivers attach the stream and mark it running here */
    ctx->priv = NULL;
    ctx->running = 1;
    return 0;
}

static int stub_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (!ctx->running) {
        *buf = NULL;
        return -1;
    }

    /*
     * The harness does not care which half is "idle": what it models is a
     * caller that waits for a boundary, so any valid half will do.
     */
    const int half = (int)(ctx->bufSize / 2);
    *buf = ctx->buffer + half;
    return half;
}

static int stub_sync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    if (!ctx->running)
        return -1;

    const unsigned start = atomic_load(&boundaries);

    if (!atomic_load(&sinkConsumes)) {
        /*
         * The failure this harness is about: a sink whose consumer never runs.
         * The driver of a stalled device waits for a boundary that never
         * arrives, and the caller waits with it.
         */
        while (ctx->running && (atomic_load(&boundaries) == start))
            sleepMs(1);
        return ctx->running ? 0 : -1;
    }

    /* A healthy device: one slot period per boundary */
    sleepMs(SLOT_MS);
    atomic_fetch_add(&boundaries, 1);
    atomic_fetch_add(&blocksPlayed, 1);
    return 0;
}

static void stub_stop(struct streamCtx *ctx)
{
    ctx->running = 0;
}

static void stub_terminate(struct streamCtx *ctx)
{
    ctx->running = 0;
    /* Wake a sync() blocked on this stream */
    atomic_fetch_add(&boundaries, 1);
}

static const struct audioDriver stubDriver = {
    .start = stub_start,
    .data = stub_data,
    .sync = stub_sync,
    .stop = stub_stop,
    .terminate = stub_terminate,
};

const struct audioDevice outputDevices[] = {
    { .driver = NULL, .config = NULL, .instance = 0, .endpoint = SINK_MCU },
    { .driver = &stubDriver, .config = NULL, .instance = SINK_RTX,
      .endpoint = SINK_RTX },
    { .driver = NULL, .config = NULL, .instance = 0, .endpoint = SINK_SPK },
};

const struct audioDevice inputDevices[] = {
    { .driver = NULL, .config = NULL, .instance = 0, .endpoint = SOURCE_MIC },
    { .driver = NULL, .config = NULL, .instance = 0, .endpoint = SOURCE_RTX },
    { .driver = NULL, .config = NULL, .instance = 0, .endpoint = SOURCE_MCU },
};

/* ------------------------------------------------------ the fake transmit -- */

/* Stands in for the PA: the whole point is that this comes back down */
static atomic_bool paKeyed;
static atomic_uint unkeyTime;

static stream_sample_t txBuffer[2 * SLOT_SAMPLES];
static streamId txStream = -1;

struct scenarios {
    bool supervisor;   ///< Run the transmit supervisor
    bool consumes;     ///< The fake sink plays slots
    unsigned holdMs;   ///< How long the PTT stays down
};

static struct scenarios scenario;

/* How the run ended, for the assertions in main() */
static atomic_uint rtxReturned;    ///< Time the transmit loop left
static atomic_uint releaseTime;    ///< PTT release, or the abort request
static atomic_uint firstBlockTime; ///< Time the first block was handed over
static atomic_bool rtxStuck;

static void *supervisorThread(void *arg)
{
    struct txGuard *guard = arg;
    const uint32_t start = nowMs();

    while (atomic_load(&paKeyed) || (nowMs() - start) < 3000) {
        const uint32_t now = nowMs();
        if (txg_tick(guard, now, true) == TXG_ACTION_ABORT) {
            /* The over ended here, by deadline: the tail is measured from it */
            if (atomic_load(&releaseTime) == 0)
                atomic_store(&releaseTime, now);

            /* Exactly what rtx.cpp does: PA first, then cancel the stream */
            if (atomic_exchange(&paKeyed, false))
                atomic_store(&unkeyTime, now);
            audioStream_terminate(txStream);
        }
        if (!atomic_load(&paKeyed) && atomic_load(&rtxReturned))
            break;
        sleepMs(5);
    }
    return NULL;
}

/**
 * The RTX thread of a digital mode: key, open the baseband stream, render slots
 * into it until the PTT goes up.
 */
static void *rtxThread(void *arg)
{
    struct txGuard *guard = arg;
    struct txGuard local;

    if (guard == NULL) {
        txg_init(&local, nowMs());
        guard = &local;
    }

    const uint32_t pttTime = nowMs();
    txg_enter(guard, pttTime);

    audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    txStream = audioStream_start(1, txBuffer, 2 * SLOT_SAMPLES, 48000,
                                 STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (txStream < 0) {
        fprintf(stderr, "audioStream_start failed: %d\n", (int)txStream);
        atomic_store(&rtxReturned, nowMs());
        return NULL;
    }

    txg_streamsOpen(guard, nowMs());

    /* Key the PA last, as the modes do */
    atomic_store(&paKeyed, true);

    for (;;) {
        stream_sample_t *idle = outputStream_getIdleBuffer(txStream);
        if (idle == NULL)
            break;

        /* A slot of baseband, as the modulator would render it */
        for (unsigned i = 0; i < SLOT_SAMPLES; i++)
            idle[i] = (stream_sample_t)((i % 64) * 256 - 8192);

        if (!outputStream_sync(txStream, true))
            break;

        if (atomic_load(&firstBlockTime) == 0)
            txg_firstBlock(guard, nowMs());
        else
            txg_block(guard, nowMs());
        if (atomic_load(&firstBlockTime) == 0)
            atomic_store(&firstBlockTime, nowMs());

        if ((nowMs() - pttTime) >= scenario.holdMs)
            break;
    }

    atomic_store(&releaseTime, nowMs());

    txg_finishing(guard, nowMs());
    audioStream_stop(txStream);
    audioPath_release(1);
    txg_exit(guard, nowMs(), TXG_REASON_PTT);

    if (atomic_exchange(&paKeyed, false) && (atomic_load(&unkeyTime) == 0))
        atomic_store(&unkeyTime, nowMs());

    atomic_store(&rtxReturned, nowMs());
    return NULL;
}

/* ------------------------------------------------------------------- main -- */

struct result {
    unsigned firstBlockMs; ///< PTT to the first block on the air
    unsigned keyedMs;      ///< PTT to the PA going off
    unsigned tailMs;       ///< PTT release (or abort) to the PA going off
    unsigned returnMs;     ///< PA off to the transmitter loop returning
    bool keyedAtEnd;
    const char *reason;
};

static struct result run(const char *name, struct scenarios s, bool watchdog)
{
    struct result r = { 0 };
    struct txGuard guard;

    /* Reset the fake hardware */
    atomic_store(&pathOpen, false);
    atomic_store(&sinkConsumes, s.consumes);
    atomic_store(&boundaries, 0);
    atomic_store(&blocksPlayed, 0);
    atomic_store(&paKeyed, false);
    atomic_store(&unkeyTime, 0);
    atomic_store(&rtxReturned, 0);
    atomic_store(&firstBlockTime, 0);
    atomic_store(&releaseTime, 0);
    atomic_store(&rtxStuck, false);
    txStream = -1;
    scenario = s;
    txg_init(&guard, nowMs());

    pthread_t rtx;
    pthread_t sup;
    pthread_create(&rtx, NULL, rtxThread, &guard);

    if (s.supervisor)
        pthread_create(&sup, NULL, supervisorThread, &guard);

    if (watchdog) {
        /*
         * Watchdog for the harness only: a stuck RTX thread would hang the test
         * run, and the firmware's own watchdog turns that into a reset.
         */
        sleepMs(3000);
        if (!atomic_load(&rtxReturned)) {
            printf("%-28s RTX thread never returned after 3 s\n", name);
            atomic_store(&sinkConsumes, false);
            audioStream_terminate(txStream);
            rtxStuck = true;
            r.keyedAtEnd = atomic_load(&paKeyed);
            if (s.supervisor)
                pthread_join(sup, NULL);
            pthread_join(rtx, NULL);
            return r;
        }
    }

    pthread_join(rtx, NULL);
    if (s.supervisor)
        pthread_join(sup, NULL);

    r.keyedAtEnd = atomic_load(&paKeyed);
    r.reason = txg_reasonName(guard.reason);
    if (atomic_load(&firstBlockTime) != 0)
        r.firstBlockMs = atomic_load(&firstBlockTime) - guard.enterTime;
    if (atomic_load(&unkeyTime) != 0) {
        r.keyedMs = atomic_load(&unkeyTime) - guard.enterTime;
        r.tailMs = atomic_load(&unkeyTime) - atomic_load(&releaseTime);
        r.returnMs = atomic_load(&rtxReturned) - atomic_load(&unkeyTime);
    }

    printf("%-28s first=%-5u tail=%-5u keyed=%-5u ret=%-4u keyednow=%s "
           "reason=%s\n",
           name, r.firstBlockMs, r.tailMs, r.keyedMs, r.returnMs,
           r.keyedAtEnd ? "YES" : "no", r.reason);
    return r;
}

int main(void)
{
    int failures = 0;

    printf("transmit lifecycle harness: %s sink\n\n",
           "stub");
    printf("%-28s %s\n", "scenario",
           "numbers (ms, measured from the PTT)");

    /*
     * A. The firmware before this change: no supervisor, a sink that never
     *    consumes. The loop is expected to be stuck: the PA is still keyed when
     *    the watchdog of the harness stops waiting.
     */
    struct result a = run("A stalled, no supervisor",
                          (struct scenarios){ false, false, 200 }, true);
    if (!a.keyedAtEnd) {
        printf("  FAIL: a stalled sink with no supervisor left the PA off\n");
        failures++;
    } else {
        printf("  ok: reproduces the hang -- the PA is still keyed\n");
    }

    /*
     * B. The fix: the same stalled sink, with the supervisor running. The PA
     *    goes off inside the first-block budget and the caller comes back.
     */
    struct result b = run("B stalled, supervised",
                          (struct scenarios){ true, false, 200 }, true);
    if (b.keyedAtEnd)
        failures++;
    if (b.keyedMs == 0 || b.keyedMs > TXG_FIRST_MS + 20)
        failures++;
    if (b.returnMs > 60)
        failures++;
    if (strcmp(b.reason, "to-first") != 0)
        failures++;
    printf("  %s: PA off %u ms after the PTT (budget %u), loop back %u ms "
           "later, reason %s\n",
           failures ? "FAIL" : "ok", b.keyedMs, TXG_FIRST_MS, b.returnMs,
           b.reason);

    /*
     * C. A sink that plays: the two numbers the radio is required to meet.
     */
    int before = failures;
    struct result c = run("C healthy, 1.5 s of slots",
                          (struct scenarios){ true, true, 1500 }, false);
    if (c.keyedAtEnd)
        failures++;
    if (c.firstBlockMs == 0 || c.firstBlockMs > TXG_FIRST_MS)
        failures++;
    if (c.tailMs > 200)
        failures++;
    if (strcmp(c.reason, "ptt") != 0)
        failures++;
    printf("  %s: first block %u ms (budget %u), out of TX %u ms after the "
           "release (budget 200)\n",
           failures > before ? "FAIL" : "ok", c.firstBlockMs, TXG_FIRST_MS,
           c.tailMs);

    printf("\n%s\n", failures ? "FAILED" : "all scenarios behaved as expected");
    return failures ? 1 : 0;
}
