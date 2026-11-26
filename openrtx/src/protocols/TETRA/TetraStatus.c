/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <tetra/tetra.h>
#include "interfaces/delays.h"
#include "protocols/TETRA/TetraSds.h"
#include "protocols/TETRA/TetraSpeech.h"
#include "protocols/TETRA/TetraStatus.h"

/* A call is shown this long after its last activity, if no release is seen */
#define CALL_TIMEOUT_MS 10000
#define VOICE_TIMEOUT_MS 1000
/* An uplink transmission stays flagged this long after its last PDU */
#define UPLINK_TIMEOUT_MS 3000

static pthread_mutex_t statusMutex = PTHREAD_MUTEX_INITIALIZER;
static struct tetraStatus status; /* Under statusMutex */
static long long lastCall;        /* Tick of the last call activity */
static long long lastVoice;       /* Tick of the last traffic slot */
static long long lastUplink;      /* Tick of the last uplink PDU */

void tetra_getStatus(struct tetraStatus *out)
{
    pthread_mutex_lock(&statusMutex);
    long long now = getTick();
    status.call = status.call && (now - lastCall < CALL_TIMEOUT_MS);
    status.voice = status.call && (now - lastVoice < VOICE_TIMEOUT_MS);
    status.uplink = status.uplink && (now - lastUplink < UPLINK_TIMEOUT_MS);
    *out = status;
    pthread_mutex_unlock(&statusMutex);
}

/**
 * An event decoded from the MS to base station direction: the radio is
 * listening to the uplink of the cell (see tetra_tmo_set_uplink_cell), and
 * what arrives is a transmitter of the network, not a downlink of it.
 */
static void markUplink(const struct tetra_event *ev)
{
    if (!ev->uplink)
        return;
    status.uplink = true;
    lastUplink = getTick();
    if (ev->type == TETRA_EV_CALL && ev->u.call.src_ssi != 0)
        status.uplinkSs = ev->u.call.src_ssi;
    else if (ev->type == TETRA_EV_SDS && ev->u.sds.src_ssi != 0)
        status.uplinkSs = ev->u.sds.src_ssi;
    else if (ev->type == TETRA_EV_TRAFFIC && ev->u.traffic.talker_ssi != 0)
        status.uplinkSs = ev->u.traffic.talker_ssi;
}

static void onSds(const struct tetra_sds *s)
{
    struct sds_msg m;

    /*
     * Keep the message for the list and the flash record; the caller holds
     * statusMutex, and TetraSds only touches its own RAM.
     */
    tetraSds_onEvent(s);

    if (sds_msg_decode(s, &m) != 0 || !sds_msg_has_text(&m))
        return; /* Location reports and empty fragments are not shown */

    /* One line, folded to what the font draws and cut to the field size. */
    memcpy(status.sds, m.text, sizeof(status.sds));
    status.sds[sizeof(status.sds) - 1] = '\0';
    status.sdsSrc = s->src_ssi;
    status.sdsTime = getTick();
}

static void onCall(const struct tetra_call *c)
{
    switch (c->what) {
        case TETRA_CALL_RELEASE:
            status.call = false;
            status.talker = 0;
            return;
        case TETRA_CALL_TX_CEASED:
            status.talker = 0;
            break;
        case TETRA_CALL_ALERT:
        case TETRA_CALL_PROCEEDING:
            return;
        default:
            if (c->src_ssi != 0)
                status.talker = c->src_ssi;
            break;
    }
    /* A new call identifier replaces the shown call */
    if (!status.call || c->call_id != status.callId) {
        status.callDst = 0;
        status.callEncrypted = false;
    }
    status.call = true;
    status.callId = c->call_id;
    if (c->dst_ssi != 0) {
        status.callDst = c->dst_ssi;
        status.callGroup = c->group;
    }
    status.callEncrypted |= c->encrypted;
    lastCall = getTick();
}

void tetraStatus_onEvent(void *user, const struct tetra_event *ev)
{
    (void)user;

    /* Speech first, outside the status lock */
    if (ev->type == TETRA_EV_TRAFFIC)
        tetraSpeech_traffic(&ev->u.traffic);

    pthread_mutex_lock(&statusMutex);
    markUplink(ev);
    switch (ev->type) {
        case TETRA_EV_SYNC:
            status.sync = ev->u.sync.locked;
            if (!status.sync)
                status.call = false;
            break;

        case TETRA_EV_CELL: {
            const struct tetra_cell *c = ev->u.cell;
            status.cell = true;
            status.mcc = c->mcc;
            status.mnc = c->mnc;
            status.colourCode = c->colour_code;
            if (c->have_sysinfo) {
                status.sysinfo = true;
                status.la = c->location_area;
                status.dlFreq = c->dl_freq_hz;
                /* Published uplink and the raw values it derives from */
                status.ulFreq = c->ul_freq_hz;
                status.band = c->band;
                status.duplexSpacing = c->duplex_spacing;
                status.reverseOp = c->reverse_operation;
                status.airEncryption = c->air_encryption;
            }
            break;
        }

        case TETRA_EV_CALL:
            onCall(&ev->u.call);
            break;

        case TETRA_EV_TRAFFIC: {
            const struct tetra_traffic *t = &ev->u.traffic;
            if (t->talker_ssi != 0)
                status.talker = t->talker_ssi;
            status.callEncrypted |= t->encrypted;
            if (!status.call)
                status.callId = t->call_id;
            status.call = true;
            lastCall = lastVoice = getTick();
            break;
        }

        case TETRA_EV_SDS:
            onSds(&ev->u.sds);
            break;

        case TETRA_EV_MM: {
            const struct tetra_mm *m = ev->u.mm;
            if (m == NULL)
                break;
            switch (m->type) {
                case TETRA_MM_LOC_UPDATE_ACCEPT:
                    status.registered = true;
                    status.mmCause = 0;
                    break;
                case TETRA_MM_LOC_UPDATE_REJECT:
                    status.registered = false;
                    status.mmCause = m->cause;
                    break;
                case TETRA_MM_GROUP_ATTACH_DETACH:
                case TETRA_MM_GROUP_ATTACH_DETACH_ACK:
                    status.groups = m->ngroups;
                    status.detachAll = m->detach_all;
                    break;
                default:
                    break;
            }
            break;
        }

        case TETRA_EV_NETWORK: {
            const struct tetra_network *n = ev->u.network;
            if (n == NULL)
                break;
            status.haveNet = true;
            status.netLoad = n->load;
            status.neighbours = n->nneighbours;
            if (n->has_time) {
                status.netTime = n->time_valid;
                status.netYear = n->year;
                status.netUtc = n->utc_s;
                status.netOffsetMin = n->offset_min;
            }
            break;
        }

        case TETRA_EV_SIGNAL:
            /* An uplink burst: the receiver does not decode it without the
             * colour code of the cell, but it is activity on the channel */
            status.uplink = true;
            lastUplink = getTick();
            break;

        default:
            break;
    }
    pthread_mutex_unlock(&statusMutex);
}

void tetraStatus_reset(void)
{
    pthread_mutex_lock(&statusMutex);
    memset(&status, 0, sizeof(status));
    pthread_mutex_unlock(&statusMutex);
}

void tetraStatus_lostSync(bool overrun)
{
    pthread_mutex_lock(&statusMutex);
    status.sync = false;
    status.resyncs++;
    if (overrun)
        status.overruns++;
    pthread_mutex_unlock(&statusMutex);
}

void tetraStatus_setDemod(unsigned phaseErr, int carrierHz)
{
    pthread_mutex_lock(&statusMutex);
    status.phaseErr = phaseErr;
    status.carrierHz = carrierHz;
    pthread_mutex_unlock(&statusMutex);
}

bool tetraStatus_sync(void)
{
    pthread_mutex_lock(&statusMutex);
    bool sync = status.sync;
    pthread_mutex_unlock(&statusMutex);
    return sync;
}
