/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DStarStatus.c
 * @brief Status, received-station list and the bridge into the message store,
 * see DStarStatus.h.
 *
 * D-STAR has no addressed messaging: the 20-character text is a label on the
 * caller's own transmission, repeated every superframe, and it is received by
 * whoever is listening.  So the inbox entry is keyed by the caller's callsign
 * and deduplicated on the text itself -- a message that repeats 15 times a
 * minute must not fill the inbox, while a caller who changes the text mid
 * transmission does produce a new entry.
 */

#include "protocols/DSTAR/DStarStatus.h"

#include <stdio.h>
#include <string.h>

#ifdef CONFIG_DSTAR_MESSAGING
#include "core/messaging.h"
#endif

static struct dstarStatus status;
static struct dstarSlow slow;
static struct dstarHeardStation heard[DSTAR_HEARD_MAX];
static unsigned heardCount;
static char lastText[DSTAR_TEXT_LEN + 1];
static char lastTextCall[DSTAR_CALL_LEN + 1];

void dstarStatus_reset(void)
{
    memset(&status, 0, sizeof(status) - 0);
    memset(status.my, 0, sizeof(status.my));
    memset(&slow, 0, sizeof(slow));
    dstarSlow_reset(&slow);
}

void dstarStatus_clear(void)
{
    dstarStatus_reset();
    memset(heard, 0, sizeof(heard));
    heardCount = 0;
    memset(lastText, 0, sizeof(lastText));
    memset(lastTextCall, 0, sizeof(lastTextCall));
    memset(&status, 0, sizeof(status));
}

struct dstarSlow *dstarStatus_slow(void)
{
    return &slow;
}

static uint32_t hashCall(const char *call)
{
    uint32_t h = 2166136261u;
    while (*call != '\0') {
        h ^= (uint8_t)*call++;
        h *= 16777619u;
    }
    return h;
}

/* Create or touch the entry of a station, most recent first. */
static struct dstarHeardStation *touchStation(const struct dstarHeader *hdr,
                                              uint32_t now)
{
    for (unsigned i = 0; i < heardCount; i++) {
        if (strcmp(heard[i].call, hdr->my) == 0 && strcmp(heard[i].sfx, hdr->sfx) == 0) {
            heard[i].heard = now;
            heard[i].frames++;
            return &heard[i];
        }
    }

    /* Make room: drop the oldest entry (the list is short and the UI shows it
     * newest first, so shifting is cheaper than a ring with a moving head). */
    if (heardCount == DSTAR_HEARD_MAX) {
        memmove(&heard[1], &heard[0], sizeof(heard[0]) * (DSTAR_HEARD_MAX - 1));
        heardCount--;
    } else {
        memmove(&heard[1], &heard[0], sizeof(heard[0]) * heardCount);
    }
    struct dstarHeardStation *st = &heard[0];
    memset(st, 0, sizeof(*st));
    snprintf(st->call, sizeof(st->call), "%.8s", hdr->my);
    snprintf(st->sfx, sizeof(st->sfx), "%.4s", hdr->sfx);
    snprintf(st->rpt1, sizeof(st->rpt1), "%.8s", hdr->rpt1);
    snprintf(st->rpt2, sizeof(st->rpt2), "%.8s", hdr->rpt2);
    snprintf(st->ur, sizeof(st->ur), "%.8s", hdr->ur);
    st->heard = now;
    st->frames = 1;
    heardCount++;
    return st;
}

void dstarStatus_header(const struct dstarHeader *hdr, int syncErrors, uint32_t now)
{
    (void)syncErrors;

    status.headers++;
    status.sync = true;
    status.flags[0] = hdr->flags[0];
    status.flags[1] = hdr->flags[1];
    status.flags[2] = hdr->flags[2];
    status.viterbi = hdr->viterbi;
    snprintf(status.my, sizeof(status.my), "%.8s", hdr->my);
    snprintf(status.sfx, sizeof(status.sfx), "%.4s", hdr->sfx);
    snprintf(status.ur, sizeof(status.ur), "%.8s", hdr->ur);
    snprintf(status.rpt1, sizeof(status.rpt1), "%.8s", hdr->rpt1);
    snprintf(status.rpt2, sizeof(status.rpt2), "%.8s", hdr->rpt2);

    dstarSlow_reset(&slow);
    touchStation(hdr, now);
}

void dstarStatus_frame(const struct dstarPhyFrame *frame, uint32_t now)
{
    status.frames++;
    status.sync = true;
    if (frame->index < 4)
        status.voice = true;

    if (frame->sync) {
        status.resyncs++;
        return;
    }

    if (dstarSlow_push(&slow, frame->slow))
        status.slowBlocks++;

    const char *text = dstarSlow_text(&slow);
    if (text != NULL) {
        status.texts++;
        snprintf(status.text, sizeof(status.text), "%s", text);

        /* Trim the space padding a radio uses to fill the 20 characters. */
        int n = (int)strlen(status.text);
        while (n > 0 && (status.text[n - 1] == ' ' || status.text[n - 1] == '\0'))
            status.text[--n] = '\0';

        if (n > 0) {
            bool same = (strcmp(text, lastText) == 0) &&
                        (strcmp(status.my, lastTextCall) == 0);
            if (!same) {
                snprintf(lastText, sizeof(lastText), "%s", text);
                snprintf(lastTextCall, sizeof(lastTextCall), "%s", status.my);
#ifdef CONFIG_DSTAR_MESSAGING
                messaging_received(MSG_TRANSPORT_DSTAR, hashCall(status.my), 0,
                                   status.text, false, now);
#endif
                if (heardCount > 0 && strcmp(heard[0].call, status.my) == 0)
                    heard[0].heard = now;
            }
        }
    }

    const char *sentence = dstarSlow_sentence(&slow);
    if (sentence != NULL) {
        struct dstarFix fix;
        if (dstarFix_parse(sentence, &fix)) {
            status.sentences++;
            status.gpsValid = fix.valid;
            status.gpsAprs = fix.fromAprs;
            status.lat = fix.lat;
            status.lon = fix.lon;
            status.alt = fix.alt;
            status.speed = fix.speed;
            status.course = fix.course;
            if (heardCount > 0) {
                heard[0].gpsValid = true;
                heard[0].lat = fix.lat;
                heard[0].lon = fix.lon;
                heard[0].alt = fix.alt;
            }
        }
    }
}

void dstar_getStatus(struct dstarStatus *out)
{
    if (out != NULL)
        *out = status;
}

const struct dstarHeardStation *dstarHeard(int i)
{
    if (i < 0 || (unsigned)i >= heardCount)
        return NULL;
    return &heard[i];
}

void dstarStatus_setStation(const char *my, const char *sfx)
{
    (void)my;
    (void)sfx;
}
