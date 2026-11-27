/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/audio_stream.h"
#include "core/audio_codec.h"
#include <pthread.h>
#include "core/threads.h"
#include "interfaces/delays.h"
// codec2 system library has a weird include prefix
#if defined(PLATFORM_LINUX)
#include <codec2/codec2.h>
#else
#include "codec2.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "core/dsp.h"

#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#else
/* The host build has no console: the two diagnostics below are for the radio */
#define printk(...) do { } while (0)
#endif
#include "core/utils.h"
#include "hwconfig.h"

#define BUF_SIZE 4
#define CODEC2_FRAME_SAMPLES 160 /* Samples per codec2 3200-mode frame */
#define DMA_BUF_SAMPLES (CODEC2_FRAME_SAMPLES * 2) /* Double-buffered DMA */

#ifndef CONFIG_MIC_GAIN
#define CONFIG_MIC_GAIN 1
#endif

#ifndef CONFIG_MIC_OVERSAMPLE
#define CONFIG_MIC_OVERSAMPLE 1
#endif

static pathId audioPath;

static uint8_t initCnt = 0;
static bool running;

static bool reqStop;
static pthread_t codecThread;
static pthread_attr_t codecAttr;
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakeup_cond = PTHREAD_COND_INITIALIZER;

static uint8_t readPos;
static uint8_t writePos;
static uint8_t numElements;
static uint64_t dataBuffer[BUF_SIZE];

/* Stream the running thread reads or writes, so another thread can cancel it */
static volatile streamId activeStream = -1;

/**
 * One codec thread's own record: what it drives, whether it has finished, and
 * the stack it was given on this target.
 *
 * It exists because a thread cannot free the stack it is running on, and
 * because a codec thread can outlive its bounded stop: the stack of a thread
 * that is still inside an audio stream belongs to it, whatever the supervisor
 * does next.  The pointer is also the identity: a thread that comes back after
 * its stop timed out must not touch the `running` flag of the run that replaced
 * it, or the next codec_stop() would believe a live thread had exited.
 */
struct codecRun
{
    pathId path;          ///< Audio path the thread drives
    void *stack;          ///< Zephyr: the stack handed to the thread, else NULL
    volatile bool exited; ///< Set by the thread just before it returns
};

static struct codecRun *run;  ///< The run that owns the codec thread, or NULL

/* Wait for the codec thread to leave, in milliseconds, before giving up */
#define CODEC_STOP_TIMEOUT_MS 300

static void *encodeFunc(void *arg);
static void *decodeFunc(void *arg);
static bool startThread(const pathId path, void *(*func)(void *));
static void stopThread();
static void runExit(struct codecRun *self);
static void runReclaim(void);

void codec_init()
{
    pthread_mutex_lock(&init_mutex);
    initCnt += 1;
    pthread_mutex_unlock(&init_mutex);

    if (initCnt > 0)
        return;

    running = false;
    readPos = 0;
    writePos = 0;
    numElements = 0;
}

void codec_terminate()
{
    pthread_mutex_lock(&init_mutex);
    initCnt -= 1;
    pthread_mutex_unlock(&init_mutex);

    if (initCnt > 0)
        return;

    if (running)
        stopThread();
    else
        runReclaim();
}

bool codec_startEncode(const pathId path)
{
    return startThread(path, encodeFunc);
}

bool codec_startDecode(const pathId path)
{
    return startThread(path, decodeFunc);
}

void codec_stop(const pathId path)
{
    if (running == false) {
        /*
         * The thread can have ended on its own -- a stream that would not open,
         * a driver timeout -- and it cannot free the stack it was running on.
         * This is where the caller comes to say it is done with the codec, so
         * this is where that stack goes back: it used to stay allocated for
         * ever, 16 KiB of a 40 KiB heap per failed start.
         */
        runReclaim();
        return;
    }

    if (audioPath != path)
        return;

    stopThread();
}

void codec_abort()
{
    reqStop = true;

    const streamId id = activeStream;
    if (id >= 0)
        audioStream_terminate(id);
}

bool codec_running()
{
    return running;
}

bool codec_queueEmpty(const pathId path)
{
    if ((running == false) || (audioPath != path))
        return true;

    return numElements == 0;
}

int codec_popFrame(uint8_t *frame, const bool blocking)
{
    if (running == false)
        return -EPERM;

    uint64_t element;

    // No data available and non-blocking call: just return false.
    if ((numElements == 0) && (blocking == false))
        return -EAGAIN;

    // Blocking call: wait until some data is pushed
    pthread_mutex_lock(&data_mutex);
    while (numElements == 0) {
        pthread_cond_wait(&wakeup_cond, &data_mutex);
    }

    element = dataBuffer[readPos];
    readPos = (readPos + 1) % BUF_SIZE;
    numElements -= 1;
    pthread_mutex_unlock(&data_mutex);

    // Do memcpy after mutex unlock to reduce execution time spent inside the
    // critical section
    memcpy(frame, &element, 8);

    return 0;
}

int codec_pushFrame(const uint8_t *frame, const bool blocking)
{
    if (running == false)
        return -EPERM;

    // Copy data to a temporary variable before mutex lock to reduce execution
    // time spent inside the critical section
    uint64_t element;
    memcpy(&element, frame, 8);

    // No space available and non-blocking call: return
    if ((numElements >= BUF_SIZE) && (blocking == false))
        return -EAGAIN;

    // Blocking call: wait until there is some free space
    pthread_mutex_lock(&data_mutex);
    while (numElements >= BUF_SIZE) {
        pthread_cond_wait(&wakeup_cond, &data_mutex);
    }

    // There is free space, push data into the queue
    dataBuffer[writePos] = element;
    writePos = (writePos + 1) % BUF_SIZE;
    numElements += 1;

    pthread_mutex_unlock(&data_mutex);
    return 0;
}

static void *encodeFunc(void *arg)
{
    struct codecRun *self = (struct codecRun *)arg;
    const pathId iPath = self->path;
    streamId iStream;
    struct CODEC2 *codec2;
    struct dcBlock dcBlock;
    struct decimatorState decimator;

    // Allocate on the heap, as the stack isn't big enough for oversampling
    // above 2 or so.
    stream_sample_t *adcBuf = malloc(DMA_BUF_SAMPLES * CONFIG_MIC_OVERSAMPLE
                                     * sizeof(stream_sample_t));
    if (adcBuf == NULL)
        goto exit;

    iStream = audioStream_start(iPath, adcBuf,
                                DMA_BUF_SAMPLES * CONFIG_MIC_OVERSAMPLE,
                                8000 * CONFIG_MIC_OVERSAMPLE,
                                STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (iStream < 0)
        goto exit;

    activeStream = iStream;

    dsp_resetState(dcBlock);
    dsp_resetState(decimator);
    codec2 = codec2_create(CODEC2_MODE_3200);
    if (codec2 == NULL) {
        /* Same rule as the stack above: no vocoder, no encoding, and the
         * caller is told instead of the radio taking a fault in codec2_encode */
        printk("codec: codec2_create() failed, no encoder\n");
        audioStream_terminate(iStream);
        free(adcBuf);
        runExit(self);
        return NULL;
    }

    while (reqStop == false) {
        // Invalid audio path, quit
        if (audioPath_getStatus(iPath) != PATH_OPEN)
            break;

        dataBlock_t audio = inputStream_getData(iStream);
        if (audio.data == NULL || audio.len == 0)
            break;

        // Each inputStream_getData call returns exactly
        // CODEC2_FRAME_SAMPLES * CONFIG_MIC_OVERSAMPLE raw samples
        // (one half of the double-buffered DMA transfer).  Decimate
        // by averaging each group of CONFIG_MIC_OVERSAMPLE samples.
        size_t outIdx = 0;
        for (size_t i = 0; i < audio.len; i++) {
            int16_t sample = audio.data[i];

#if CONFIG_MIC_OVERSAMPLE > 1
            if (!dsp_decimator(&decimator, &sample, CONFIG_MIC_OVERSAMPLE))
                continue;
#endif

            sample = dsp_dcBlockFilter(&dcBlock, sample);
            audio.data[outIdx++] = s16_mulSat(sample, CONFIG_MIC_GAIN);
        }

        // CODEC2 encodes 160 speech samples (20 ms), into 8 bytes: here we
        // write the new encoded data into a buffer of 16 bytes, writing the
        // first half and then the second one, sequentially.
        // Data ready flag is rised once all the 16 bytes contain new data.
        uint64_t frame = 0;
        codec2_encode(codec2, ((uint8_t *)&frame), audio.data);

        pthread_mutex_lock(&data_mutex);

        // If buffer is full erase the oldest frame
        if (numElements >= BUF_SIZE) {
            readPos = (readPos + 1) % BUF_SIZE;
        }

        dataBuffer[writePos] = frame;
        writePos = (writePos + 1) % BUF_SIZE;

        if (numElements == 0)
            pthread_cond_signal(&wakeup_cond);

        if (numElements < BUF_SIZE)
            numElements += 1;

        pthread_mutex_unlock(&data_mutex);
    }

    audioStream_terminate(iStream);
    codec2_destroy(codec2);

exit:
    free(adcBuf);
    runExit(self);
    return NULL;
}

static void *decodeFunc(void *arg)
{
    struct codecRun *self = (struct codecRun *)arg;
    const pathId oPath = self->path;
    streamId oStream;
    int16_t audioBuf[DMA_BUF_SAMPLES];
    struct CODEC2 *codec2;

    // Open output stream
    memset(audioBuf, 0x00, DMA_BUF_SAMPLES * sizeof(stream_sample_t));
    oStream = audioStream_start(oPath, audioBuf, DMA_BUF_SAMPLES, 8000,
                                STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (oStream < 0) {
        runExit(self);
        return NULL;
    }

    activeStream = oStream;

    codec2 = codec2_create(CODEC2_MODE_3200);
    if (codec2 == NULL) {
        /* The encoder branch has always answered this; the decoder did not, and
         * codec2_decode(NULL) is a fault in the middle of a voice prompt. */
        printk("codec: codec2_create() failed, no decoder\n");
        audioStream_stop(oStream);
        runExit(self);
        return NULL;
    }

    // Ensure that thread start is correctly synchronized with the output
    // stream to avoid having the decode function writing in a memory area
    // being read at the same time by the output stream system causing cracking
    // noises at speaker output. Behaviour observed on both Module17 and MD-UV380
    outputStream_sync(oStream, false);

    while (reqStop == false) {
        // Invalid path, quit
        if (audioPath_getStatus(oPath) != PATH_OPEN)
            break;

        // Try popping data from the queue
        uint64_t frame = 0;
        bool newData = false;

        pthread_mutex_lock(&data_mutex);

        if (numElements != 0) {
            frame = dataBuffer[readPos];
            readPos = (readPos + 1) % BUF_SIZE;
            if (numElements >= BUF_SIZE)
                pthread_cond_signal(&wakeup_cond);

            numElements -= 1;
            newData = true;
        }

        pthread_mutex_unlock(&data_mutex);

        stream_sample_t *audioBuf = outputStream_getIdleBuffer(oStream);
        if (audioBuf == NULL)
            break;

        if (newData) {
            codec2_decode(codec2, audioBuf, ((uint8_t *)&frame));

#ifdef PLATFORM_MD3x0
            // Bump up volume a little bit, as on MD3x0 is quite low
            for (size_t i = 0; i < CODEC2_FRAME_SAMPLES; i++)
                audioBuf[i] *= 2;
#endif

        } else {
            memset(audioBuf, 0x00, CODEC2_FRAME_SAMPLES * sizeof(int16_t));
        }

        outputStream_sync(oStream, true);
    }

    // Stop stream and wait until its effective termination
    audioStream_stop(oStream);
    codec2_destroy(codec2);

    runExit(self);
    return NULL;
}

static bool startThread(const pathId path, void *(*func)(void *))
{
    // Bad incoming path
    if (audioPath_getStatus(path) != PATH_OPEN)
        return false;

    // Handle access contention when starting the codec thread to ensure that
    // only one call at a time can effectively start the thread.
    pthread_mutex_lock(&init_mutex);
    if (running) {
        // Same path as before, path open, codec already running: all good.
        if (path == audioPath) {
            pthread_mutex_unlock(&init_mutex);
            return true;
        }

        // New path takes over the current one only if it has an higher priority
        // or the current one is closed/suspended.
        pathInfo_t newPath = audioPath_getInfo(path);
        pathInfo_t curPath = audioPath_getInfo(audioPath);
        if ((curPath.status == PATH_OPEN) && (curPath.prio >= newPath.prio)) {
            pthread_mutex_unlock(&init_mutex);
            return false;
        } else {
            stopThread();
        }
    }

    running = true;
    audioPath = path;
    pthread_mutex_unlock(&init_mutex);

    /*
     * A previous thread can have ended on its own -- the stream refused to open,
     * the driver timed it out -- and codec_stop() returned before it could free
     * that thread's stack, because the thread had already cleared the running
     * flag.  Give it back here: a thread cannot free the stack it is running on,
     * so somebody else has to, and this is the moment the previous run is over.
     */
    runReclaim();

    readPos = 0;
    writePos = 0;
    numElements = 0;
    reqStop = false;

    pthread_attr_init(&codecAttr);

    void *stack = NULL;

#if defined(_MIOSIX)
    // Set stack size of CODEC2 thread to 16kB.
    pthread_attr_setstacksize(&codecAttr, CODEC2_THREAD_STKSIZE);

    // Set priority of CODEC2 thread to the maximum one, the same of RTX thread.
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&codecAttr, &param);
#elif defined(__ZEPHYR__)
    /*
     * Allocate and set the stack for the CODEC2 thread.  The result is checked:
     * handing a NULL stack to pthread_attr_setstack() puts the first context
     * switch of the thread on address 0, which on this SoC is an immediate
     * fault and a reboot -- the radio restarted the instant PTT was pressed in
     * M17, the only mode that starts this thread.  A codec with no stack is a
     * codec that cannot run, not a reason to lose the radio: the caller is told
     * and refuses the transmission.
     *
     * The size cannot go down to 8 KiB: the decoder's real high-water mark is
     * 15 484 B of this 16 384 (tests/qemu/codec2_stack.c, see threads.h).
     */
    stack = malloc(CODEC2_THREAD_STKSIZE);
    if (stack == NULL) {
        printk("codec: no stack for the codec thread (%u B)\n",
               (unsigned)CODEC2_THREAD_STKSIZE);
        running = false;
        audioPath = -1;
        return false;
    }
    pthread_attr_setstack(&codecAttr, stack, CODEC2_THREAD_STKSIZE);
#endif

    struct codecRun *self = malloc(sizeof(struct codecRun));
    if (self == NULL) {
        free(stack);
        running = false;
        audioPath = -1;
        return false;
    }
    self->path   = path;
    self->stack  = stack;
    self->exited = false;

    /* Before the create: the thread can report its exit before it returns here */
    run = self;

    // Start thread
    int ret = pthread_create(&codecThread, &codecAttr, func, self);
    if (ret != 0) {
        /*
         * POSIX returns the error number itself, not -1 with errno set: tested
         * against zero, the refusal used to leave the caller believing a codec
         * thread existed.  The stack and the run go back with it.
         */
        printk("codec: cannot start the codec thread (%d)\n", ret);
        run = NULL;
        free(self->stack);
        free(self);
        running = false;
        audioPath = -1;
        return false;
    }

    return true;
}

/**
 * The thread says it is done, and stops touching the run if it is still the
 * one in charge.
 *
 * @param self: the run this thread was started with.
 */
static void runExit(struct codecRun *self)
{
    self->exited = true;

    /*
     * A thread whose bounded stop timed out comes back after another run has
     * taken over: its flags are not the radio's any more.  Writing them anyway
     * is how a zombie made a live codec thread look stopped -- and then its
     * stack would be freed under it.
     */
    if (run != self)
        return;

    activeStream = -1;
    running = false;
}

/**
 * Give back the stack of a codec thread that has finished.
 *
 * A thread cannot free the stack it is running on, so the one that supervises
 * it does, and only once the thread is gone: pthread_join() is what says so,
 * and it cannot block for longer than the few instructions the thread has left
 * (it has already set exited).  A run whose thread never came back is not
 * touched: it is detached, nobody can join it, and its stack stays with it --
 * the bounded price of a bounded stop.
 */
static void runReclaim(void)
{
    struct codecRun *self = run;

    if ((self == NULL) || (self->exited == false))
        return;

    pthread_join(codecThread, NULL);

    free(self->stack);
    free(self);
    run = NULL;
}

/**
 * Stop the codec thread, with a bound.
 *
 * pthread_join() has no timeout on this kernel, and the thread can be blocked
 * in an audio stream read or in a DSP call: joining it for ever is how the RTX
 * thread used to hang with the PA keyed. The stream is cancelled first, which
 * is what releases the thread in the normal case, then it is given
 * CODEC_STOP_TIMEOUT_MS to unwind. A thread that does not come back is left
 * detached: its 16 KiB stack is not recovered, which is a bounded price for not
 * hanging the radio.
 */
static void stopThread()
{
    reqStop = true;
    codec_abort();

    for (unsigned int i = 0; (i < CODEC_STOP_TIMEOUT_MS) && running; i++)
        sleepFor(0, 1);

    if (running) {
        /*
         * Still inside the stream or the DSP. Nobody will join it: detach it,
         * and let the caller go on. running goes false here so a new operation
         * can be started, and the run is forgotten with it: the thread still
         * owns its stack, and a later run must not be mistaken for this one.
         */
        pthread_detach(codecThread);
        running = false;
        run = NULL;
        return;
    }

    /* It exited on its own: join it and give the stack back */
    runReclaim();
}
