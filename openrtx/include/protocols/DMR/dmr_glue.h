/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_GLUE_H
#define DMR_GLUE_H

/**
 * @file dmr_glue.h
 * @brief The radio's DMR settings, as the protocol package reads them.
 *
 * The DMR package (protocols/DMR/dmr_*.c, trunk/) is the protocol layer of
 * another C62 tree; what it read from that tree's non-volatile memory
 * (ui/C62Extras.h: @c c62_dmr and @c c62_t3) is ours.  Those two records are
 * declared here with the same field names, so the package keeps reading them
 * and the only edit to its files is the include of this header (dmr_trunk.c).
 * The mode fills them from the channel, the settings and the quick menu; see
 * docs/DMR_PKG.md, "Hooks".
 *
 * Nothing here allocates or blocks: it is a plain, always-present record.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Control channels kept besides the channel's own frequency (their
 * C62_T3_CTRL_MORE) */
#define C62_T3_CTRL_MORE 3

/** Tier III operating modes, as the engine's listen/registered split */
enum c62T3Mode {
    C62_T3_OFF = 0,
    C62_T3_LISTEN = 1, /**< Follow the system, never transmit */
    C62_T3_REG = 2     /**< Register and answer it (needs a radio ID) */
};

/* The DMR channel and call settings the package reads */
typedef struct {
    uint32_t id;        /**< Own DMR ID, 0 unset */
    uint8_t cc;         /**< Colour code, 0..15 */
    uint8_t slot;       /**< Timeslot 1 or 2 */
    uint32_t tg;        /**< Talkgroup or called ID */
    bool privateCall;   /**< tg is a unit (private call), not a talkgroup */

    /*
     * Transmit air delay: from writing a slot into the baseband stream to
     * that slot leaving the PA, in microseconds.  Positive moves the bursts
     * later.  One number, one unit, and the same quantity the branch
     * c62/dmr-rpt-timing carries as DMR_RPT_TX_LATENCY in 48 kHz samples
     * (1440 samples = 30000 us), see docs/DMR_PKG.md "Transmit air delay".
     */
    int32_t txAirDelayUs;
    bool txAirDelayAuto; /**< Search it against a repeater's answer */

    bool txFlip;        /**< Invert the transmitted 4FSK (polarity setting) */
} dmrRadioSettings_t;

/* Tier III, as Ajustes > DMR would carry it (their c62_t3) */
typedef struct {
    uint8_t mode;       /**< enum c62T3Mode */
    bool followAll;     /**< Monitor the system, not only the talkgroup */
    uint32_t baseHz;    /**< Channel plan: base frequency of LCN 1 */
    uint16_t first;     /**< Channel plan: first LCN */
    uint8_t sep;        /**< Channel plan: LCN step, 2^(sep * 2.5 kHz) */
    bool txLow;         /**< Base station transmits below the MS */
    uint32_t splitHz;   /**< Duplex split of the fixed plan */
    uint32_t ctrlHz[C62_T3_CTRL_MORE]; /**< Extra control channels */
} dmrT3Settings_t;

extern dmrRadioSettings_t c62_dmr;
extern dmrT3Settings_t c62_t3;

/**
 * Defaults of both records: no ID, colour code 0, TS1, no talkgroup, package
 * off, the calibrated air delay of this audio path and its plan.
 */
void dmrGlue_reset(void);

/**
 * The air delay to send with: the persistent trim when it is set, the
 * measured default otherwise.  Microseconds.
 */
int32_t dmrGlue_airDelayUs(void);

/** Change the persistent trim (0..60000 us). */
void dmrGlue_setAirDelayUs(int32_t us);

/**
 * Samples of silence to emit before the first burst of a call through a
 * repeater: the air delay modulo one TDMA frame (60 ms at 48 kHz), so the
 * bursts land in the repeater's slot instead of a fixed number of
 * milliseconds late.  Simplex does not use it.
 */
uint32_t dmrGlue_preRollSamples(void);

/**
 * Our SMS format setting (enum dmrSmsFmtSetting, eight flavours) onto the
 * package's three (enum dmrSmsFormat).  AUTO is what this firmware has always
 * sent, so it keeps going out as ETSI IP text.
 */
uint8_t dmrGlue_smsFormat(uint8_t setting);

#ifdef __cplusplus
}
#endif

#endif /* DMR_GLUE_H */
