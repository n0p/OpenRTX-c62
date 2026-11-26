/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_ZONES_H
#define UI_ZONES_H

/*
 * Zone browser (ui_zones.c): the repeaters and the PMR channels the radio can
 * tune to, listed by zone with their nomenclature and their frequency.
 *
 * The browser owns no frequency of its own. The tables - which repeaters exist
 * in R0..R7, U0..U100 and PMR1..PMR16, their split and their power - come from
 * the data model (`core/zones.h`: zones_at(), zones_channels(),
 * zones_channel()). When that header is in the tree the browser uses it
 * directly; when it is not (this branch merged alone, or a target without the
 * zone plan) the list is empty and says so ("NO ZONES"). The UI only decides
 * how a row is named, laid out and scrolled, and it draws whatever power the
 * model reports: the PMR entries read "500mW" because the plan says 500 mW,
 * not because the browser assumes it.
 *
 * `ui_zones_setSource()` is the seam for a target that gets its tables from
 * somewhere else, and for the tests. It takes precedence over the model; NULL
 * falls back to the model when it is compiled in.
 *
 * Keys, the same as every other list of this UI (docs/UI.md):
 *   UP/DOWN  previous / next entry
 *   green    zones: open the zone; entries: tune it and leave
 *   P1/P2    unused on the zone list, P2 latches nothing
 *   red      entries: back to the zone list; zones: back to the menu
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One row of a zone. */
typedef struct
{
    char     label[8];   /**< "R0".."R7", "U0".."U100", "PMR1".."PMR16";
                              empty when the label must be derived     */
    uint32_t rx_hz;      /**< Receive frequency                        */
    uint32_t tx_hz;      /**< Transmit frequency, equal to rx if FM    */
    uint16_t power_mw;   /**< Transmit power, mW                       */
    uint8_t  mode;       /**< OPMODE_* (rtx/rtx.h), 0 = FM             */
} ui_zone_entry_t;

/** One zone. */
typedef struct
{
    const char *title;   /**< "R", "U", "PMR" or a descriptive name    */
    uint16_t    count;
    const ui_zone_entry_t *entries;
} ui_zone_t;

/**
 * Where the browser gets its tables from.
 *
 * Implemented by the data model. Every call is allowed to be NULL: the browser
 * then reports an empty zone instead of dereferencing it.
 */
typedef struct
{
    uint8_t  zones;                                    /**< zone count     */
    const char *(*name)(uint8_t zone);                 /**< zone title     */
    uint16_t    (*size)(uint8_t zone);                 /**< entries in it  */
    const ui_zone_entry_t *(*at)(uint8_t zone, uint16_t index);
} ui_zones_source_t;

/** Register the zone tables. NULL restores the empty browser. */
void ui_zones_setSource(const ui_zones_source_t *src);

/** The registered source, NULL when there is none. */
const ui_zones_source_t *ui_zones_source(void);

/** Open on the zone list. */
void ui_zones_open(void);

/** True while the browser owns the keypad. */
bool ui_zones_isOpen(void);

/** Handle one key event. Always consumes it while open. */
bool ui_zones_handleEvent(kbd_msg_t *msg, bool *sync_rtx);

/** Draw the browser. */
void ui_zones_draw(void);

/**
 * Nomenclature of entry `index` of a zone whose title is `zone_title`.
 *
 * "R0".."R7" and "U0".."U100" for the repeater zones, "PMR1".."PMR16" for the
 * PMR zone, "<title><index+1>" for anything else. Never writes outside `len`.
 */
void ui_zones_label(const char *zone_title, uint16_t index, char *buf,
                    size_t len);

/**
 * Power text of a row: "500mW" under 1 W, "1W", "2.5W" above. Returns `buf`.
 */
const char *ui_zones_powerText(uint16_t mw, char *buf, size_t len);

/** True when the row is the 500 mW (QRP) mark of the PMR zone. */
bool ui_zones_isQrp(uint16_t mw);

/** Frequency text, "446.00625" with the trailing zeros of the step kept. */
void ui_zones_fmtFreq(uint32_t hz, char *buf, size_t len);

/** Rows visible in a zone: 8 with the shared 12 px row (docs/UI.md). */
uint8_t ui_zones_visibleRows(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_ZONES_H */
