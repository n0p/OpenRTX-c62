/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_UPLINK_H
#define OPENRTX_TETRA_UPLINK_H

#include <stdbool.h>
#include <stdint.h>

#include "protocols/TETRA/TetraStatus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The "Cell uplink" row of the TETRA quick menu (docs/TETRA_LIB.md, section
 * "Escuchar el uplink de la celda").
 *
 * Listening to the uplink of a cell is a tuning position, not a receiver mode:
 * while the radio is on the cell's uplink frequency libtetra decodes the MS to
 * BS direction, and the downlink it came from is remembered here so the row can
 * take the receiver back.  The session belongs to the operator, not to the
 * settings: it starts when the row is set to On, and ends when it is set to Off,
 * when another frequency or mode is selected, or when the mode reports that the
 * cell is gone.  Nothing here touches the radio or the settings; the quick menu
 * applies the frequency and the TETRA mode seeds libtetra with the colour code.
 */

/** What the quick menu row displays. */
enum tetraUlDisplay
{
    TETRA_UL_OFF = 0, /**< Off: receiving the downlink as usual            */
    TETRA_UL_ON,      /**< On: tuned to the cell's uplink                  */
    TETRA_UL_NO_FREQ, /**< On asked for, but the uplink is not known       */
};

/**
 * Uplink frequency matching a downlink frequency (TS 100 392-15, table 2).
 *
 * The same table libtetra applies when the cell publishes its SYSINFO, kept
 * here so the firmware can deduce the uplink from the downlink when an event
 * carries the band and the duplex spacing but no computed frequency, and so
 * that the conversion is covered by the host tests (the host build has no
 * libtetra).
 *
 * @param dlHz: downlink frequency in Hz.
 * @param band: frequency band (as in tetra_cell.band).
 * @param duplexSpacing: duplex spacing code, 3 bits.
 * @param reverse: reverse operation (the uplink sits above the downlink).
 * @return uplink frequency in Hz, 0 if the spacing code is reserved.
 */
uint32_t tetraUl_hzFromDownlink(uint32_t dlHz, uint8_t band,
                                uint8_t duplexSpacing, bool reverse);

/** Forget any session (power on, or a mode that never had one). */
void tetraUplink_reset(void);

/**
 * Start following the uplink of the cell the status holds.
 *
 * The uplink frequency is the one the cell published, or the one the duplex
 * conversion gives from its downlink; if neither is known the session is kept
 * as "asked, no frequency" and nothing is retuned.
 *
 * @param ts: what the TETRA receiver holds now.
 * @param fromHz: frequency the receiver is on (where Off goes back to).
 * @return 0 when there is a frequency to tune to, -1 when there is not.
 */
int tetraUplink_start(const struct tetraStatus *ts, uint32_t fromHz);

/**
 * End the session, if any.
 *
 * @return true when it was following (the caller puts the receiver back on
 *         tetraUplink_backHz()).
 */
bool tetraUplink_stop(void);

/** @return true while the receiver is following a cell's uplink. */
bool tetraUplink_active(void);

/**
 * Value the quick menu row shows.
 *
 * @param nowHz: frequency the receiver is on, unused today; kept so a row that
 *               wants to cross-check can.
 */
enum tetraUlDisplay tetraUplink_display(uint32_t nowHz);

/** @return uplink frequency of the session, 0 when there is none. */
uint32_t tetraUplink_freq(void);

/** @return downlink frequency the session goes back to, 0 when none. */
uint32_t tetraUplink_backHz(void);

/**
 * The cell whose uplink is decoded, for tetra_tmo_set_uplink_cell().
 *
 * @return false when not following.
 */
bool tetraUplink_cell(uint16_t *mcc, uint16_t *mnc, uint8_t *cc);

/**
 * The mode reports that the cell went away: end the session and let the UI put
 * the receiver back on the downlink.
 */
void tetraUplink_cellLost(void);

/**
 * Periodic part, run from the UI thread.
 *
 * Ends a session whose mode or frequency the operator changed, and carries the
 * "back to the downlink" the TETRA mode asked for when it saw the cell go.
 *
 * @param mode: current operating mode.
 * @param nowHz: frequency the receiver is tuned to.
 * @param hz: set to the frequency the UI must retune to, when it returns true.
 * @return true when the UI has to retune.
 */
bool tetraUplink_tick(uint8_t mode, uint32_t nowHz, uint32_t *hz);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_UPLINK_H */
