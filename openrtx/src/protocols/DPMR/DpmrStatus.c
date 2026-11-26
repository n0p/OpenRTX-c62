/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DpmrStatus.c
 * @brief What the dPMR receive mode has decoded, see DpmrStatus.h.
 *
 * One writer (the rtx thread, one frame at a time) and one reader (the UI,
 * which copies the struct out a few times a second). There is no lock: the
 * reader takes a memcpy of a handful of words and a value one frame old is
 * still a valid answer, which is the same shape NxdnStatus.c uses.
 */

#include <string.h>

#include "protocols/DPMR/DpmrStatus.h"
#include "protocols/DPMR/DpmrDefs.h"

static struct dpmrStatus status;

void dpmrStatus_reset(void)
{
    status.sync = false;
    status.receiving = false;
    status.voice = false;
    status.channelCode = -1;
    status.callType = DPMR_CALLTYPE_NONE;
    status.idsValid = false;
    status.source = 0;
    status.dest = 0;
    status.mode = 0xFF;
    status.ht = 0xFF;
    status.endSeen = false;
    status.endHasStatus = false;
    status.endStatus = 0;
    status.endWait = 0;
    status.evm = 0;
}

void dpmrStatus_clear(void)
{
    memset(&status, 0, sizeof(status));
    status.channelCode = -1;
    status.mode = 0xFF;
    status.ht = 0xFF;
}

void dpmrStatus_receiving(bool on)
{
    status.receiving = on;
}

void dpmrStatus_frame(const struct dpmrFrame *frame)
{
    status.frames++;

    if (frame->sync == DPMR_SYNC_NONE)
        return;

    /* A frame of any kind decoded: from here the call fields mean something */
    status.sync = true;
    status.evm = (uint16_t)(frame->evm * 1000.0f);

    /*
     * A header carries the whole call: both 24 bit addresses, the
     * communication mode and the header type, and the type is what says
     * whether a call opens (start) or is being torn down (disconnect).
     */
    if (frame->headerOk) {
        status.headers++;
        status.idsValid = true;
        status.source = frame->header.ownId;
        status.dest = frame->header.calledId;
        status.mode = frame->header.mode;
        status.ht = frame->header.ht;

        if (frame->header.ht == DPMR_HT_START)
            status.callType = DPMR_CALLTYPE_VCALL;
        else if (frame->header.ht == DPMR_HT_DISCONNECT)
            status.callType = DPMR_CALLTYPE_DISC;
    }

    /*
     * The END frame closes the transmission. It is published as TX_REL, except
     * after a disconnect request: there the DISC is the more specific fact and
     * the END only carries it out, so overwriting it would lose the reason the
     * call ended.
     */
    if (frame->endOk) {
        status.ends++;
        status.endSeen = true;
        status.endWait = frame->end.wait;

        /* Table 5.12: only ET = 01 carries a status message (clause 5.15) */
        if (frame->end.et == 1) {
            status.endHasStatus = true;
            status.endStatus = frame->end.status;
        }

        if (status.callType != DPMR_CALLTYPE_DISC)
            status.callType = DPMR_CALLTYPE_TX_REL;
    }

    /*
     * The channel code opens frames 2 and 4 of a superframe. The header frame
     * carries it too, but the PHY does not read it into the frame, so these are
     * the first frames that can publish it. It is taken only from a frame whose
     * CCH closed its CRC-7, so "a code was decoded" means what it says: a
     * mistimed CC sync alone is not enough.
     */
    if (frame->cchOk) {
        if (frame->sync == DPMR_SYNC_CC)
            status.channelCode = (int16_t)frame->channelCode;
    } else if ((frame->sync != DPMR_SYNC_FS1)
               && (frame->sync != DPMR_SYNC_FS3)) {
        status.cchErrors++;
    }

    if (frame->voice) {
        status.voice = true;
        status.voiceFrames++;
        status.ambeFrames += DPMR_AMBE_FRAMES;
    }
}

void dpmr_getStatus(struct dpmrStatus *out)
{
    memcpy(out, &status, sizeof(*out));
}

const char *dpmrStatus_callName(uint8_t callType)
{
    switch (callType) {
        case DPMR_CALLTYPE_VCALL:  return "VCALL";
        case DPMR_CALLTYPE_TX_REL: return "TX_REL";
        case DPMR_CALLTYPE_DISC:   return "DISC";
        default:                   return NULL;
    }
}

#ifdef SIM_INJECT
/*
 * The simulator's `inject dpmr` command: what a receiver locked to a dPMR
 * carrier would hold, so the main screen's state block (channel code, the
 * call, its type, the END status) can be looked at without a radio. Host only.
 */
void dpmrStatus_simSet(bool sync, bool receiving, int channelCode,
                       unsigned evm, unsigned frames, bool idsValid,
                       unsigned long source, unsigned long dest,
                       unsigned callType, int endStatus)
{
    status.sync = sync;
    status.receiving = receiving;
    status.channelCode = (int16_t)channelCode;
    status.evm = (uint16_t)evm;
    status.frames = frames;
    status.idsValid = idsValid;
    status.source = (uint32_t)source;
    status.dest = (uint32_t)dest;
    status.callType = (uint8_t)callType;
    status.voice = sync && (callType == DPMR_CALLTYPE_VCALL);
    status.endSeen = (callType == DPMR_CALLTYPE_TX_REL);
    status.endHasStatus = endStatus >= 0;
    status.endStatus = (uint8_t)((endStatus < 0) ? 0 : endStatus);
}
#endif /* SIM_INJECT */
