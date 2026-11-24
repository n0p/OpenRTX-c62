/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_KEY_H
#define DMR_KEY_H

#include <stdbool.h>
#include <stdint.h>

#include "core/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * May the DMR transmitter key with this call configuration?
 *
 * This is the whole PTT gate of OpMode_DMR, in a header so tests/unit can walk
 * it: the mode itself needs the audio streams, the radio and the vocoder and
 * cannot be linked on the host.
 *
 * The source identifier is **not** a condition. A radio whose settings record
 * was erased (which is what a firmware update that grows the record does: the
 * CRC refuses the old one and the defaults come back) has no personal DMR ID,
 * and refusing to key for that reason is a silent dead PTT: no carrier, no
 * error, nothing on screen. This fork transmitted with 0 in the source field
 * of the link control before the protocol package replaced the chain, and a
 * call with 0 there is still a well formed burst -- the destination is what a
 * repeater or a subscriber filters on. Whoever wants their ID on the air sets
 * it in the DMR page of the quick menu.
 *
 * What does stop a call is a destination that cannot be encoded at all: an
 * unknown destination type, or GROUP/USER with no identifier to put in the
 * header. `DMR_DEST_NONE` means "the default talkgroup of the settings", which
 * dmr_tx_setup() resolves, so it is allowed.
 *
 * @param destType  enum dmrDestType of the call destination
 * @param destId    destination identifier (talkgroup or subscriber)
 * @param sourceId  the radio's own identifier, 0 when it has none
 * @return true when the PTT can key the carrier
 */
static inline bool dmrKey_allowed(uint8_t destType, uint32_t destId,
                                  uint32_t sourceId)
{
    (void)sourceId; /* 0 is allowed: the LC carries what there is */

    switch (destType) {
        case DMR_DEST_NONE:
            return true;
        case DMR_DEST_GROUP:
        case DMR_DEST_USER:
            return settings_dmrIdValid(destId);
        default:
            return false;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DMR_KEY_H */
