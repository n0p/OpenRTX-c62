/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_P25_STATUS_H
#define OPENRTX_P25_STATUS_H

/**
 * @file P25Status.h
 * @brief What the P25 receive mode has decoded, for the UI.
 *
 * Written by the rtx thread (one frame at a time), copied out with
 * p25Status_get(), which is safe to call from the UI thread.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct p25Frame;

struct p25Status {
    bool sync;            ///< A frame was decoded in the last 300 ms
    bool voice;           ///< A voice frame arrived in the last second
    bool encrypted;       ///< The header announced encryption
    int16_t nac;          ///< Network access code, -1 until one decodes
    int8_t duid;          ///< Data unit ID of the last frame, -1 if none
    int16_t talkgroup;    ///< Group address of the last link control, -1 none
    int32_t source;       ///< Source subscriber unit, -1 if none
    int8_t lcFormat;      ///< Link control format of the last LDU1
    uint8_t hduAlgid;     ///< Encryption algorithm of the last header

    uint32_t frames;      ///< Frames decoded since the mode was enabled
    uint32_t voiceFrames; ///< LDU1/LDU2 frames
    uint32_t imbe;        ///< IMBE frames handed to the vocoder
    uint32_t imbeBad;     ///< Of which uncorrectable
    uint32_t imbeErrors;  ///< Bits corrected by the IMBE FEC
    uint32_t tsbk;        ///< TSBK blocks decoded
    uint32_t crcOk;       ///< Of which with a good CRC-16
    uint32_t syncErrors;  ///< Sign errors in the sync fields
};

/**
 * Clear the status, keeping the counters (new frequency, mode change).
 */
void p25Status_reset(void);

/**
 * Clear everything, counters included.
 */
void p25Status_clear(void);

/**
 * Account one decoded frame, call from the rtx thread.
 *
 * @param frame: the frame.
 */
void p25Status_frame(const struct p25Frame *frame);

/**
 * Copy the status out, safe from any thread.
 *
 * @param out: destination.
 */
void p25Status_get(struct p25Status *out);

/**
 * true while a frame has been decoded recently.
 */
bool p25Status_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_P25_STATUS_H */
