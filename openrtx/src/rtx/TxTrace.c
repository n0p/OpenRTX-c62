/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx/TxTrace.h"

#include <string.h>

/*
 * One record, overwritten by every startTx().  The pointers are string
 * literals that live in the image, so the whole thing is a few words of .bss:
 * no allocation, no formatting, nothing that can fail while reporting a
 * failure.
 */
static struct txTraceReport last;

void txTrace_reset(const char *mode)
{
    last.mode       = mode;
    last.lastStep   = NULL;
    last.failedCall = NULL;
    last.failedRc   = 0;
    last.blockedBy  = NULL;
    last.steps      = 0;
    last.keyed      = false;
}

void txTrace_step(const char *call, int rc)
{
    last.lastStep = call;
    if (last.steps < UINT8_MAX)
        last.steps++;

    /* The first refusal is the one that ended the transmission: what the mode
     * does to unwind afterwards is cleanup, not the reason. */
    if ((rc < 0) && (last.failedCall == NULL))
    {
        last.failedCall = call;
        last.failedRc   = rc;
    }
}

void txTrace_block(const char *why)
{
    if (last.blockedBy == NULL)
        last.blockedBy = why;
}

void txTrace_keyed(void)
{
    last.keyed = true;
}

void txTrace_report(struct txTraceReport *out)
{
    if (out != NULL)
        *out = last;
}
