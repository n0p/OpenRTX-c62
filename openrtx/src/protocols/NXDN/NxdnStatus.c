/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file NxdnStatus.c
 * @brief What the NXDN receive mode has decoded, see NxdnStatus.h.
 *
 * One writer (the rtx thread) and many readers (the UI), so the copy-out is
 * done under a short critical section instead of a mutex: the UI polls this a
 * few times a second and must never block the demodulator.
 */

#include <string.h>

#include "protocols/NXDN/NxdnStatus.h"
#ifdef CONFIG_KISS
#include "core/packet_service.h"
#endif
#include "protocols/NXDN/NxdnPhy.h"

static struct nxdnStatus status;

void nxdnStatus_reset(void)
{
    memset(&status.message, 0, sizeof(status.message));
    status.sync = false;
    status.voice = false;
    status.ran = -1;
    status.rfType = 0;
    status.usc = 0;
    status.steal = 0;
    status.inverted = false;
    status.evm = 0;
}

void nxdnStatus_clear(void)
{
    memset(&status, 0, sizeof(status));
    status.ran = -1;
}

void nxdnStatus_frame(const struct nxdnFrame *frame)
{
    status.frames++;

    if (!frame->lichOk) {
        status.lichErrors++;
        return;
    }

    status.sync = true;
    status.rfType = frame->lich.rfType;
    status.usc = frame->lich.usc;
    status.steal = frame->lich.steal;
    status.inverted = frame->inverted;
    status.evm = (uint16_t)(frame->evm * 1000.0f);

    status.sacchChecks++;
    if (frame->sacchOk) {
        status.sacchOk++;
        /* The RAN rides in bits 2..7 of the 8-bit SR field of the SACCH */
        status.ran = (int8_t)(frame->sacchSr & 0x3Fu);
    }

    if (frame->voiceCount > 0)
        status.voice = true;

    if (frame->frameCount > 0) {
        status.voiceFrames++;
        status.ambeFrames += frame->frameCount;
    }
}

void nxdnStatusSacchSegment(int part, bool complete,
                            const struct nxdnSacchMessage *msg)
{
    (void)part;

    if (!complete)
        return;

    status.messages++;
    status.message.valid = msg->valid;
    status.message.type = msg->type;
    status.message.source = msg->source;
    status.message.dest = msg->dest;

#ifdef CONFIG_KISS
    /*
     * Packet modem, NXDN mode: the nine octets of the layer 3 message go to
     * the host as one KISS frame. Receive only: this firmware has no NXDN
     * transmitter and the short data call, which would be the natural payload,
     * travels in FACCH1 and is not decoded (docs/RX_NXDN.md, docs/KISS.md).
     */
    packet_service_nxdnPayload(msg->octets, sizeof(msg->octets));
#endif
}

void nxdn_getStatus(struct nxdnStatus *out)
{
    memcpy(out, &status, sizeof(*out));
}

#ifdef SIM_INJECT
/*
 * The simulator's `inject nxdn` command: what a receiver locked to a NXDN
 * carrier would hold, so the main screen's state block (RAN, sync, the SACCH
 * call, the EVM) can be looked at without a radio. Host only.
 */
void nxdnStatus_simSet(bool sync, int ran, bool voice, unsigned evm,
                       bool msgValid, unsigned source, unsigned dest,
                       unsigned type)
{
    status.sync = sync;
    status.voice = voice;
    status.ran = (int8_t)ran;
    status.evm = (uint16_t)evm;
    status.message.valid = msgValid;
    status.message.source = (uint16_t)source;
    status.message.dest = (uint16_t)dest;
    status.message.type = (uint8_t)type;
}
#endif /* SIM_INJECT */
