/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_NXDN_STATUS_H
#define OPENRTX_NXDN_STATUS_H

/**
 * @file NxdnStatus.h
 * @brief What the NXDN receive mode has decoded, for the UI.
 *
 * Written by the rtx thread (one frame at a time) and copied out with
 * nxdn_getStatus(), which is safe to call from the UI thread.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/NXDN/NxdnFec.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nxdnFrame;

/** One layer 3 message the SACCH of a superframe carried. */
struct nxdnStatusMessage {
    bool valid;      ///< Assembled and all four block CRCs passed
    uint8_t type;    ///< Message type: 0x01 VCALL, 0x08 TX_REL, 0x11 DISC
    uint16_t source; ///< Source unit ID
    uint16_t dest;   ///< Destination or talkgroup
};

struct nxdnStatus {
    bool sync;          ///< A frame decoded in the last 300 ms
    bool voice;         ///< A voice frame arrived in the last second
    int8_t ran;         ///< RAN, -1 until a SACCH block decodes
    uint8_t rfType;     ///< Raw LICH fields of the last frame
    uint8_t usc;
    uint8_t steal;
    bool inverted;      ///< The discriminator is inverted
    uint16_t evm;       ///< EVM of the last frame, in thousandths

    uint32_t frames;      ///< Frames decoded since the mode was enabled
    uint32_t voiceFrames; ///< Of which carried voice
    uint32_t ambeFrames;  ///< AMBE+2 frames handed to the vocoder
    uint32_t lichErrors;  ///< Frames whose LICH parity failed
    uint32_t sacchChecks; ///< SACCH blocks with a CRC, and those that passed
    uint32_t sacchOk;
    uint32_t messages;    ///< Superframes whose four blocks all passed

    struct nxdnStatusMessage message; ///< Last assembled SACCH message
};

/** Clear the status, keeping the counters (new frequency, mode change). */
void nxdnStatus_reset(void);

/** Clear everything, counters included. */
void nxdnStatus_clear(void);

/**
 * @brief Account one decoded frame, call from the rtx thread.
 *
 * Counters, sync, RAN and EVM, plus the layer 3 message: the SACCH segments of
 * a superframe are assembled here, so a message is published (and, with KISS,
 * handed to the packet modem) the moment its fourth block closes.
 */
void nxdnStatus_frame(const struct nxdnFrame *frame);

/** Copy the current status. */
void nxdn_getStatus(struct nxdnStatus *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_NXDN_STATUS_H */
