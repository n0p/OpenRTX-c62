/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_STATUS_H
#define OPENRTX_DMR_STATUS_H

/**
 * @file DmrStatus.h
 * @brief What the DMR receive mode has decoded, for the UI.
 *
 * Ours, not the package's: the screen and the quick menu were written against
 * this struct, and the package has its own snapshots (dmr_proto_get_slot(),
 * dmr_proto_get_log(), dmr_proto_get_stats()).  DmrStatus.c translates the
 * package's snapshot into this one, so the UI keeps reading what it read and
 * the package keeps its API.  See docs/DMR_PKG.md, "Hooks".
 *
 * Written by the rtx thread (one burst at a time), copied out with
 * dmr_getStatus(), which is safe to call from the UI thread.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dmrStatus {
    bool sync;            ///< A burst was decoded in the last 300 ms
    bool voice;           ///< A voice burst arrived in the last second
    int8_t colourCode;    ///< Colour code, -1 until a slot decodes
    int8_t timeslot;      ///< Timeslot 1 or 2 from the CACH, 0 if unknown
    uint8_t accessType;   ///< CACH access type of the last burst
    bool lateEntry;       ///< CACH late entry flag of the last burst
    uint16_t evm;         ///< Link quality of the last bursts, thousandths
    int8_t lastType;      ///< Last data type seen, -1 if none

    uint32_t bursts;      ///< Bursts decoded since the mode was enabled
    uint32_t voiceBursts; ///< Of which voice
    uint32_t frames;      ///< AMBE+2 frames handed to the vocoder
    uint32_t crcChecks;   ///< Data blocks with a CRC, and those that passed
    uint32_t crcOk;
};

/**
 * Clear the status, keeping the counters (new frequency, mode change).
 */
void dmrStatus_reset(void);

/**
 * Clear everything, counters included.
 */
void dmrStatus_clear(void);

/**
 * Refresh the snapshot from the decoder, call from the rtx thread after the
 * samples of a block have been fed to dmr_demod_feed_disc().
 */
void dmrStatus_update(void);

/**
 * Copy the current status.
 *
 * @param out: destination.
 */
void dmr_getStatus(struct dmrStatus *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_STATUS_H */
