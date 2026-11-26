/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file P25Status.c
 * @brief P25 receive status for the UI, see P25Status.h.
 */

#include "protocols/P25/P25Status.h"

#include <string.h>

#ifdef CONFIG_P25

#include <pthread.h>

#include "interfaces/delays.h"
#include "protocols/P25/P25Phy.h"

/** A voice frame this recent keeps the call shown */
#define VOICE_HOLD_MS 1000
/** A frame this recent keeps the synchronisation indicator on */
#define SYNC_HOLD_MS 300

static pthread_mutex_t statusMutex = PTHREAD_MUTEX_INITIALIZER;
static struct p25Status status; /* Under statusMutex */
static long long lastVoice;     /* Tick of the last voice frame */
static long long lastFrame;     /* Tick of the last frame */

void p25Status_clear(void)
{
    pthread_mutex_lock(&statusMutex);
    memset(&status, 0, sizeof(status));
    status.nac = -1;
    status.duid = -1;
    status.talkgroup = -1;
    status.source = -1;
    lastVoice = 0;
    lastFrame = 0;
    pthread_mutex_unlock(&statusMutex);
}

void p25Status_reset(void)
{
    pthread_mutex_lock(&statusMutex);
    status.sync = false;
    status.voice = false;
    status.encrypted = false;
    status.nac = -1;
    status.duid = -1;
    status.talkgroup = -1;
    status.source = -1;
    status.lcFormat = 0;
    status.hduAlgid = 0;
    pthread_mutex_unlock(&statusMutex);
}

void p25Status_frame(const struct p25Frame *frame)
{
    pthread_mutex_lock(&statusMutex);

    lastFrame = getTick();
    status.sync = true;
    status.frames++;
    status.syncErrors += (uint32_t)frame->syncErrors;
    status.nac = (int16_t)frame->nac;
    status.duid = (int8_t)frame->duid;

    if (frame->duid == P25_DUID_HDU)
    {
        /* 0x80 is the clear algorithm; anything else is encrypted */
        status.hduAlgid = frame->hduAlgid;
        if (frame->hdu && frame->hduAlgid != 0x80)
            status.encrypted = true;
    }

    if (frame->lcValid)
    {
        status.talkgroup = (int16_t)frame->talkgroup;
        status.source = (int32_t)frame->source;
        status.lcFormat = (int8_t)frame->lcFormat;
        if ((frame->lcService & 0x40) != 0)
            status.encrypted = true;
    }

    if (frame->duid == P25_DUID_LDU1 || frame->duid == P25_DUID_LDU2)
    {
        status.voiceFrames++;
        status.imbe += frame->voiceCount;
        status.imbeBad += frame->voiceBad;
        if (frame->imbeErrors > 0)
            status.imbeErrors += (uint32_t)frame->imbeErrors;
        if (frame->voiceCount > 0)
        {
            status.voice = true;
            lastVoice = getTick();
        }
    }

    if (frame->duid == P25_DUID_TSBK)
    {
        status.tsbk++;
        if (frame->tsbkValid)
            status.crcOk++;
    }

    pthread_mutex_unlock(&statusMutex);
}

void p25Status_get(struct p25Status *out)
{
    long long now = getTick();

    pthread_mutex_lock(&statusMutex);
    *out = status;
    if ((now - lastFrame) > SYNC_HOLD_MS)
        out->sync = false;
    if ((now - lastVoice) > VOICE_HOLD_MS)
        out->voice = false;
    pthread_mutex_unlock(&statusMutex);
}

bool p25Status_sync(void)
{
    bool sync = false;
    long long now = getTick();

    pthread_mutex_lock(&statusMutex);
    sync = ((now - lastFrame) <= SYNC_HOLD_MS);
    pthread_mutex_unlock(&statusMutex);

    return sync;
}

#else /* CONFIG_P25 */

/*
 * A build without the P25 engine decodes nothing. The simulator still has to
 * show what the mode's screen looks like when it does decode something, so
 * under SIM_INJECT the block can be filled by `inject p25` (emulator.h).
 */
#ifdef SIM_INJECT
static struct p25Status simStatus;
#endif

void p25Status_clear(void)
{
#ifdef SIM_INJECT
    memset(&simStatus, 0, sizeof(simStatus));
    simStatus.nac = -1;
    simStatus.duid = -1;
    simStatus.talkgroup = -1;
    simStatus.source = -1;
#endif
}

void p25Status_reset(void)
{
    p25Status_clear();
}

void p25Status_frame(const struct p25Frame *frame)
{
    (void)frame;
}

void p25Status_get(struct p25Status *out)
{
#ifdef SIM_INJECT
    *out = simStatus;
#else
    memset(out, 0, sizeof(*out));
    out->nac = -1;
    out->duid = -1;
    out->talkgroup = -1;
    out->source = -1;
#endif
}

bool p25Status_sync(void)
{
#ifdef SIM_INJECT
    return simStatus.sync;
#else
    return false;
#endif
}

#ifdef SIM_INJECT
void p25Status_simSet(bool sync, bool voice, int nac, int duid, long source,
                      int talkgroup, unsigned imbe, unsigned imbeBad,
                      unsigned frames)
{
    p25Status_clear();
    simStatus.sync = sync;
    simStatus.voice = voice;
    simStatus.nac = (int16_t)nac;
    simStatus.duid = (int8_t)duid;
    simStatus.source = (int32_t)source;
    simStatus.talkgroup = (int16_t)talkgroup;
    simStatus.imbe = imbe;
    simStatus.imbeBad = imbeBad;
    simStatus.frames = frames;
}
#endif /* SIM_INJECT */

#endif /* CONFIG_P25 */
