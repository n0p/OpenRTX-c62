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
 * Written by the rtx thread (one burst at a time), copied out with
 * dmr_getStatus(), which is safe to call from the UI thread.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dmrBurst;

struct dmrStatus {
    bool sync;            ///< A burst was decoded in the last 300 ms
    bool voice;           ///< A voice burst arrived in the last second
    int8_t colourCode;    ///< Colour code, -1 until a slot type decodes
    int8_t timeslot;      ///< Timeslot 1 or 2 from the CACH, 0 if unknown
    uint8_t accessType;   ///< CACH access type of the last burst
    bool lateEntry;       ///< CACH late entry flag of the last burst
    uint16_t evm;         ///< EVM of the last burst, in thousandths
    int8_t lastType;      ///< Last data type seen, -1 if none

    /*
     * Repeater operation, for the quick menu and the traces: the state the
     * transmit path is in, what the phase tracker has fitted and the delay it
     * is applying (docs/DMR_REPEATER.md).
     */
    int8_t  rptState;     ///< enum rptUiState of the mode, 0 when idle
    int8_t  rptGate;      ///< Last gate verdict (enum dmrRptGate)
    int16_t rptPpm;       ///< Drift of the repeater's clock, ppm
    uint8_t rptQuality;   ///< Accepted / measured bursts of the window, 0..100
    uint16_t rptShift;    ///< Baseband delay in use, samples
    int8_t  rptCalValid;  ///< Attempts of the last calibration that were good
    int16_t rptCalOffset; ///< ... and the offset it chose, samples
    int16_t rptCalSpread; ///< ... and their spread, samples

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
 * Account one decoded burst, call from the rtx thread.
 *
 * @param burst: the burst.
 */
void dmrStatus_burst(const struct dmrBurst *burst);

/**
 * Record the state of the repeater path, call from the rtx thread.
 *
 * @param state: enum rptUiState of the mode.
 * @param ppm: drift fitted by the phase tracker.
 * @param quality: share of the phase measurements that were accepted, 0..100.
 * @param shift: baseband delay in use, samples.
 */
void dmrStatus_repeater(int8_t state, int16_t ppm, uint8_t quality,
                        uint16_t shift);

/**
 * Record the verdict of the colour code / timeslot gate.
 *
 * @param gate: enum dmrRptGate of the last access.
 */
void dmrStatus_repeaterGate(int8_t gate);

/**
 * Record the result of a calibration pass, call from the rtx thread.
 *
 * @param valid: attempts that came back well.
 * @param offset: the offset chosen, samples.
 * @param spread: highest minus lowest of them, samples.
 */
void dmrStatus_repeaterCal(int valid, int16_t offset, int16_t spread);

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
