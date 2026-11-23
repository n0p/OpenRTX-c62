/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrStatus.c
 * @brief DMR receive status for the UI, see DmrStatus.h.
 */

#include "protocols/DMR/DmrStatus.h"

#include <string.h>

#ifdef CONFIG_DMR

#include <pthread.h>

#include "interfaces/delays.h"
#include "protocols/DMR/DmrPhy.h"

/** A voice burst this recent keeps the call shown */
#define VOICE_HOLD_MS 1000
/** A burst this recent keeps the synchronisation indicator on */
#define SYNC_HOLD_MS 300

static pthread_mutex_t statusMutex = PTHREAD_MUTEX_INITIALIZER;
static struct dmrStatus status; /* Under statusMutex */
static long long lastVoice;     /* Tick of the last voice burst */
static long long lastBurst;     /* Tick of the last burst */

void dmrStatus_clear(void)
{
    pthread_mutex_lock(&statusMutex);
    memset(&status, 0, sizeof(status));
    status.colourCode = -1;
    status.lastType = -1;
    lastVoice = 0;
    pthread_mutex_unlock(&statusMutex);
}

void dmrStatus_reset(void)
{
    pthread_mutex_lock(&statusMutex);
    status.sync = false;
    status.voice = false;
    status.colourCode = -1;
    status.timeslot = 0;
    status.accessType = 0;
    status.lateEntry = false;
    status.evm = 0;
    status.lastType = -1;
    pthread_mutex_unlock(&statusMutex);
}

void dmrStatus_burst(const struct dmrBurst *burst)
{
    pthread_mutex_lock(&statusMutex);

    lastBurst = getTick();
    status.evm = (uint16_t)(burst->evm * 1000.0f);
    status.phase = dmrPhy_phase();

    if (burst->cachValid) {
        status.accessType = burst->accessType;
        status.timeslot = burst->timeslot2 ? 2 : 1;
        status.lateEntry = burst->lateEntry;
    }

    if (burst->slotTypeValid) {
        status.colourCode = (int8_t)burst->colourCode;
        status.lastType = (int8_t)burst->dataType;
    }

    if (burst->voice) {
        status.voiceBursts++;
        status.frames += burst->frameCount;
        status.voice = true;
        lastVoice = getTick();
    } else {
        if (burst->crcChecked) {
            status.crcChecks++;
            if (burst->crcOk)
                status.crcOk++;
        }
    }

    status.bursts++;
    pthread_mutex_unlock(&statusMutex);
}

void dmr_getStatus(struct dmrStatus *out)
{
    pthread_mutex_lock(&statusMutex);
    long long now = getTick();
    status.sync = (now - lastBurst) < SYNC_HOLD_MS;
    status.voice = status.voice && ((now - lastVoice) < VOICE_HOLD_MS);
    *out = status;
    pthread_mutex_unlock(&statusMutex);
}

#else /* CONFIG_DMR */

void dmrStatus_clear(void)
{
}

void dmrStatus_reset(void)
{
}

void dmrStatus_burst(const struct dmrBurst *burst)
{
    (void)burst;
}

void dmr_getStatus(struct dmrStatus *out)
{
    memset(out, 0, sizeof(*out));
    out->colourCode = -1;
    out->lastType = -1;
}

#endif /* CONFIG_DMR */
