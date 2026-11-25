/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/audio_stream.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "interfaces/delays.h"

#define MAX_NUM_STREAMS 3
#define MAX_NUM_DEVICES 3

/*
 * The table is shared: the thread that owns a stream starts and stops it, but a
 * supervisor may cancel it from another thread when the owner is blocked, which
 * is how a blocked caller is released. Every access to the table goes through
 * this lock. It is never held while a driver call waits for data -- see below:
 * the driver waits happen outside it, or in calls that are bounded.
 */
struct streamState
{
    const struct audioDevice *dev;
    struct streamCtx          ctx;
    pathId                    path;
};

static struct streamState streams[MAX_NUM_STREAMS] = {0};
static pthread_mutex_t streamsLock = PTHREAD_MUTEX_INITIALIZER;

/*
 * How long a caller waits for the table, and why it is not "until it is free".
 *
 * The lock is held across a driver stop and a driver sync (audioStream_stop),
 * and those are bounded but not short: a stalled codec can spend up to a
 * second in each.  A plain lock here hands that wait to every other caller,
 * including the UI thread playing a voice prompt and the RTX supervisor
 * cancelling a stream -- the thread that is supposed to unblock the owner ends
 * up behind it.  With a deadline the caller is told that the table is busy and
 * can answer (the protocol's status poll) or try again on its next pass, while
 * the owner finishes its driver call and releases.
 */
#define STREAMS_LOCK_MS 100
#define STREAMS_LOCK_STEP_MS 1

/*
 * Try-lock in a bounded loop rather than pthread_mutex_timedlock(): the host
 * resolves that one against CLOCK_REALTIME and Zephyr against CLOCK_MONOTONIC,
 * so the same absolute deadline means two different things.  This is the idiom
 * state_tryLock() uses, for the same reason.
 */
static bool lockStreams(void)
{
    if(pthread_mutex_trylock(&streamsLock) == 0)
        return true;

    uint32_t waited = 0;
    while(waited < STREAMS_LOCK_MS)
    {
        delayMs(STREAMS_LOCK_STEP_MS);
        waited += STREAMS_LOCK_STEP_MS;
        if(pthread_mutex_trylock(&streamsLock) == 0)
            return true;
    }
    return false;
}

static void unlockStreams(void)
{
    pthread_mutex_unlock(&streamsLock);
}


/**
 * \internal
 * Verify if the path associated to a given stream is still open and, if path is
 * closed or suspended, terminate the stream.
 *
 * @param id: stream ID.
 * @return true if the path is valid.
 */
static bool validateStream(const streamId id)
{
    if((id < 0) || (id >= MAX_NUM_STREAMS))
        return false;

    uint8_t status = audioPath_getStatus(streams[id].path);
    if(status != PATH_OPEN)
    {
        // Path has been closed or suspended: terminate the stream and free it
        streams[id].dev->driver->terminate(&(streams[id].ctx));
        streams[id].path = 0;

        return false;
    }

    // Path is still open
    return true;
}

/**
 * \internal
 * Start an audio stream. In case of failure, the stream is freed.
 *
 * @param id: stream ID.
 * @return zero if the stream started, a negative error code otherwise.
 */
static int startStream(const streamId id)
{
    const struct audioDevice *dev = streams[id].dev;

    int ret = dev->driver->start(dev->instance, dev->config, &streams[id].ctx);
    if(ret < 0)
    {
        streams[id].ctx.running = 0;
        streams[id].path = 0;
    }

    return ret;
}

streamId audioStream_start(const pathId path, stream_sample_t * const buf,
                           const size_t length, const uint32_t sampleRate,
                           const uint8_t mode)
{
    // Check for invalid stream mode or invalid buffer handling mode
    if(((mode & 0xF0) == 0) || ((mode & 0x0F) == 0))
        return -EINVAL;

    pathInfo_t pathInfo = audioPath_getInfo(path);
    if(pathInfo.status != PATH_OPEN)
        return -EPERM;

    if(!lockStreams())
        return -EBUSY;

    // Search for an audio device serving the correct output endpoint.
    const struct audioDevice *dev  = NULL;
    const struct audioDevice *devs = inputDevices;
    const uint8_t streamMode = (mode & 0xF0);
    const uint8_t bufMode = (mode & 0x0F);
    uint8_t endpoint = pathInfo.source;

    if(streamMode == STREAM_OUTPUT)
    {
        devs = outputDevices;
        endpoint = pathInfo.sink;
    }

    for(size_t i = 0; i < MAX_NUM_DEVICES; i++)
    {
        if(devs[i].endpoint == endpoint)
            dev = &devs[i];
    }

    // No audio device found
    if((dev == NULL) || (dev->driver == NULL))
    {
        unlockStreams();
        return -ENODEV;
    }

    // Search for an empty audio stream slot
    streamId id = -1;
    for(size_t i = 0; i < MAX_NUM_STREAMS; i++)
    {
        // While searching, cleanup dead streams
        if(streams[i].path > 0)
        {
            if(audioPath_getStatus(streams[i].path) != PATH_OPEN)
            {
                streams[i].dev->driver->terminate(&(streams[i].ctx));
                streams[i].path = 0;
            }
        }

        // Empty stream found
        if((streams[i].path <= 0) && (streams[i].ctx.running == 0))
            id = i;
    }

    // No stream slots available
    if(id < 0)
    {
        unlockStreams();
        return -EBUSY;
    }

    // Setup new stream and start it
    streams[id].path           = path;
    streams[id].dev            = dev;
    streams[id].ctx.buffer     = buf;
    streams[id].ctx.bufMode    = bufMode;
    streams[id].ctx.bufSize    = length;
    streams[id].ctx.sampleRate = sampleRate;

    // In circular mode, start immediately
    if(bufMode == BUF_CIRC_DOUBLE)
    {
        int ret = startStream(id);
        if(ret < 0)
        {
            unlockStreams();
            return ret;
        }
    }

    unlockStreams();
    return id;
}

void audioStream_stop(const streamId id)
{
    if((id < 0) || (id >= MAX_NUM_STREAMS))
        return;

    /*
     * If the table is busy the stream keeps running: the caller asked to stop
     * it and could not, which is a failure the mode has to see (it answers -1
     * to its own caller) instead of a wait with the PA keyed.  The path is
     * closed by the mode anyway, which is what validateStream() acts on.
     */
    if(!lockStreams())
        return;

    if(streams[id].path != 0)
    {
        streams[id].dev->driver->stop(&(streams[id].ctx));
        streams[id].dev->driver->sync(&(streams[id].ctx), false);
        streams[id].path = 0;
    }
    unlockStreams();
}

void audioStream_terminate(const streamId id)
{
    if((id < 0) || (id >= MAX_NUM_STREAMS))
        return;

    /*
     * Held across the driver call on purpose: the slot can be reused as soon as
     * path goes to zero, and a terminate that arrived late would then cancel the
     * new stream. The driver's terminate is the non-blocking one, by contract.
     */
    if(!lockStreams())
        return;

    if(streams[id].path != 0)
    {
        streams[id].dev->driver->terminate(&(streams[id].ctx));
        streams[id].path = 0;
    }
    unlockStreams();
}

dataBlock_t inputStream_getData(streamId id)
{
    dataBlock_t block = {NULL, 0};
    int ret;

    /*
     * The waits below can take a whole buffer period, and on a stalled device
     * longer: they must not hold the table lock, or a cancel from another
     * thread could not release this call. The device and context are stable for
     * the lifetime of the stream, and the driver checks that the context is
     * still the one attached to it.
     */
    if(!lockStreams())
        return block;

    if(validateStream(id) == false)
    {
        unlockStreams();
        return block;
    }

    if(streams[id].ctx.bufMode == BUF_LINEAR)
    {
        ret = startStream(id);
        if(ret < 0)
        {
            streams[id].ctx.running = 0;
            streams[id].path = 0;
            unlockStreams();
            return block;
        }
    }

    const struct audioDevice *dev = streams[id].dev;
    struct streamCtx *ctx = &(streams[id].ctx);
    unlockStreams();

    ret = dev->driver->sync(ctx, false);
    if(ret < 0)
        return block;

    ret = dev->driver->data(ctx, &block.data);
    if(ret < 0)
    {
        block.data = NULL;
        return block;
    }

    block.len = (size_t) ret;
    return block;
}

stream_sample_t *outputStream_getIdleBuffer(const streamId id)
{
    if(!lockStreams())
        return NULL;

    if(validateStream(id) == false)
    {
        unlockStreams();
        return NULL;
    }
    const struct audioDevice *dev = streams[id].dev;
    struct streamCtx *ctx = &(streams[id].ctx);
    unlockStreams();

    stream_sample_t *buf;
    int ret = dev->driver->data(ctx, &buf);
    if(ret < 0)
        return NULL;

    return buf;
}

bool outputStream_sync(const streamId id, const bool bufChanged)
{
    if(!lockStreams())
        return false;

    if(validateStream(id) == false)
    {
        unlockStreams();
        return false;
    }
    const struct audioDevice *dev = streams[id].dev;
    struct streamCtx *ctx = &(streams[id].ctx);
    unlockStreams();

    int ret = dev->driver->sync(ctx, bufChanged);
    if(ret < 0)
        return false;

    return true;
}
