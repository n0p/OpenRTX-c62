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
 * Both halves of the call have to be encodable. The **source** is the radio's
 * own 24-bit ID, which goes in the source field of every link control and data
 * header (TS 102 361-1 clause 9.3.1). Zero there is not "no address yet": it is
 * a call from nobody, which no repeater and no subscriber can answer, and the
 * air carries it just the same. The radio therefore refuses to key without one,
 * and says so: the DMR main screen prints "NO TX: DMR ID 0" while the field is
 * empty, and Settings -> Identity is where it is typed (or
 * `cps.py settings set --dmr-id`). A settings record erased by a firmware
 * update comes back with 0, so the operator finds out from the screen instead
 * of from a call nobody answered.
 *
 * The **destination** has to be encodable too: an unknown destination type, or
 * GROUP/USER with no identifier to put in the header. `DMR_DEST_NONE` means
 * "the default talkgroup of the settings", which dmr_tx_setup() resolves, so it
 * is allowed.
 *
 * @param destType  enum dmrDestType of the call destination
 * @param destId    destination identifier (talkgroup or subscriber)
 * @param sourceId  the radio's own identifier, 0 when it has none
 * @return true when the PTT can key the carrier
 */
static inline bool dmrKey_allowed(uint8_t destType, uint32_t destId,
                                  uint32_t sourceId)
{
    if (!settings_dmrIdValid(sourceId))
        return false;

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
