/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_MODES_H
#define UI_MODES_H

/**
 * @file ui_modes.h
 * @brief The operating modes this build has, in one table.
 *
 * Every screen that lists or names the modes reads this registry instead of
 * keeping its own copy: the channel editor's mode list, the side key's cycle,
 * the quick menu (one row table per mode) and the mode pages of Settings. One
 * mode is one entry of `uiModes[]` in ui_quickmenu.c, next to the row tables it
 * points to; if it also gets a page in Settings, one line in @ref UI_MODE_PAGES
 * below. docs/UI.md, "Añadir un modo", is the procedure.
 *
 * The registry does not carry the display name: that is `chstore_modeName()`
 * in core/channel_store.c, which the channel records, the channel list and the
 * header all use, so there is exactly one place where a mode is spelled.
 *
 * Which modes are compiled in is a build question and the UI keeps one flag per
 * mode for it. The firmware sets them from its own options (CONFIG_M17 comes
 * from hwconfig.h, CONFIG_DMR / CONFIG_TETRA / CONFIG_NXDN from the target's
 * CMakeLists.txt). The host test build cannot link a demodulator, so it turns
 * the *screens* of a mode on with UI_TEST_<MODE> and links only the status
 * module those screens read.
 */

#include <stdbool.h>
#include <stdint.h>

#include "rtx/rtx.h"
#include "ui/ui_quickmenu.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_M17)
#define UI_MODE_M17 1
#endif

#if defined(CONFIG_DMR)
#define UI_MODE_DMR 1
#endif

#if defined(CONFIG_TETRA)
#define UI_MODE_TETRA 1
#endif

#if defined(CONFIG_NXDN) || defined(UI_TEST_NXDN)
#define UI_MODE_NXDN 1
#endif

#if defined(CONFIG_DPMR) || defined(UI_TEST_DPMR)
#define UI_MODE_DPMR 1
#endif

#if defined(CONFIG_P25) || defined(UI_TEST_P25)
#define UI_MODE_P25 1
#endif

#if defined(CONFIG_YSF) || defined(UI_TEST_YSF)
#define UI_MODE_YSF 1
#endif

#if defined(CONFIG_DSTAR) || defined(UI_TEST_DSTAR)
#define UI_MODE_DSTAR 1
#endif

/*
 * Airband AM has no build option: the mode, the BK4819 AM path and its AGC are
 * always in the image, so a build that can tune 118-137 MHz can receive it.
 */
#define UI_MODE_AIRBAND 1

/*
 * Broadcast FM is the BK1080 tuner and its analog path, and it is always in the
 * image as well (radio_C62.cpp powers the tuner, core/wfm.c holds the band).
 * Letting the side key reach it costs a table, not a kilobyte.
 */
#define UI_MODE_WFM 1

/**
 * Where a mode shows up, as the flags of its registry entry.
 *
 * The three lists that used to be written by hand - the channel editor's mode
 * list, the side key's cycle and the Settings menu - are filtered from these.
 */
enum ui_mode_flags
{
    UI_MODE_TX      = 0x01, /**< Can key the transmitter                    */
    UI_MODE_CHANNEL = 0x02, /**< Offered by the channel editor              */
    UI_MODE_SELECT  = 0x04, /**< Reached by the side key, in registry order */
    UI_MODE_PAGE    = 0x08  /**< Has a page in Settings, its own rows       */
};

/** A registry entry is a mode's quick menu table plus its capabilities. */
typedef qm_menu_t ui_mode_t;

/*
 * Geometry of the mode label in the main screens' top bar: five characters of
 * the 6 pt font (35 px) plus a margin, which keeps it clear of the clock in the
 * middle of the 160 px bar. A longer name is cut by _ui_drawMainTop(), and
 * tests/unit/ui_modes.cpp fails if a registered one does not fit.
 */
#define UI_MODE_LABEL_LEN    6
#define UI_MODE_LABEL_MAX_W  40

/** Number of modes in this build, in the order UP/DOWN and the side key walk. */
uint8_t ui_mode_count(void);

/**
 * Mode at an index of the registry.
 *
 * @param index: 0..ui_mode_count()-1.
 * @return the entry, NULL past the end.
 */
const ui_mode_t *ui_mode_at(uint8_t index);

/**
 * Registry entry of an operating mode.
 *
 * @param id: OPMODE_*.
 * @return the entry, NULL when this build does not have that mode.
 */
const ui_mode_t *ui_mode_find(uint8_t id);

/** Registry entry of the mode the radio is in (state.channel.mode). */
const ui_mode_t *ui_mode_current(void);

/**
 * Short name of a mode for the header and for every list that shows one.
 *
 * @param id: OPMODE_*.
 * @return two to five characters ("FM", "NXDN", "AM"), never NULL: an unknown
 *         mode reads "----", and the name of a mode compiled out is still there
 *         for a channel record that carries it.
 */
const char *ui_mode_label(uint8_t id);

/**
 * Long name of a mode ("NXDN", "AM"), for the popup header and the settings
 * page title.
 *
 * @param id: OPMODE_*.
 * @return the title, or the short label for a mode that is not registered.
 */
const char *ui_mode_title(uint8_t id);

/** True when the mode can transmit in this build. */
bool ui_mode_canTx(uint8_t id);

/** True when the mode has a page in Settings (drawn from its own rows). */
bool ui_mode_hasPage(uint8_t id);

/** True when the channel editor offers the mode. */
bool ui_mode_inChannelList(uint8_t id);

/**
 * Next mode of the side key's cycle.
 *
 * @param id: current mode, OPMODE_*; an unknown or compiled-out mode restarts
 *        the cycle at its first entry, so the key can never get stuck.
 * @param dir: +1 forward, -1 backward.
 * @return the mode to switch to.
 */
uint8_t ui_mode_next(uint8_t id, int dir);

/**
 * Quick menu row table of a mode.
 *
 * @param id: OPMODE_*.
 * @param count: filled with the number of rows; may be NULL.
 * @return the rows, NULL when the mode has no table of its own.
 */
const qm_item_t *ui_mode_rows(uint8_t id, uint8_t *count);

/*
 * The modes that have a page in Settings, in menu order. A page is the mode's
 * own quick menu table drawn as a list, so its content, its limits and its
 * setters are the ones already tested for the popup.
 *
 * X(id, title) for each; a mode that is compiled out expands to nothing.
 */
#ifdef UI_MODE_NXDN
#define UI_MODE_PAGE_NXDN(X) X(OPMODE_NXDN, "NXDN")
#else
#define UI_MODE_PAGE_NXDN(X)
#endif

#ifdef UI_MODE_DPMR
#define UI_MODE_PAGE_DPMR(X) X(OPMODE_DPMR, "dPMR")
#else
#define UI_MODE_PAGE_DPMR(X)
#endif

#ifdef UI_MODE_P25
#define UI_MODE_PAGE_P25(X) X(OPMODE_P25, "P25")
#else
#define UI_MODE_PAGE_P25(X)
#endif

#ifdef UI_MODE_YSF
#define UI_MODE_PAGE_YSF(X) X(OPMODE_YSF, "YSF")
#else
#define UI_MODE_PAGE_YSF(X)
#endif

#ifdef UI_MODE_DSTAR
#define UI_MODE_PAGE_DSTAR(X) X(OPMODE_DSTAR, "DSTAR")
#else
#define UI_MODE_PAGE_DSTAR(X)
#endif

#ifdef UI_MODE_AIRBAND
#define UI_MODE_PAGE_AIRBAND(X) X(OPMODE_AIRBAND, "AM")
#else
#define UI_MODE_PAGE_AIRBAND(X)
#endif

#ifdef UI_MODE_WFM
#define UI_MODE_PAGE_WFM(X) X(OPMODE_WFM_BC, "WFM")
#else
#define UI_MODE_PAGE_WFM(X)
#endif

#define UI_MODE_PAGES(X)                                                      \
    UI_MODE_PAGE_NXDN(X) UI_MODE_PAGE_DPMR(X) UI_MODE_PAGE_P25(X)             \
    UI_MODE_PAGE_YSF(X) UI_MODE_PAGE_DSTAR(X) UI_MODE_PAGE_AIRBAND(X)         \
    UI_MODE_PAGE_WFM(X)

/*
 * Row accessors of the modes that have their own controls.
 *
 * They live in ui_quickmenu.c with the rows that use them and are declared here
 * because ui_modes.h is the one header that knows which modes are
 * compiled in: a caller that wants to set a value without the popup open (the
 * tests, and any future screen) gets the same setter the row calls, never a
 * second copy of the limits.
 */
#ifdef UI_MODE_NXDN
int  qm_nxdn_get_ran(void);
void qm_nxdn_set_ran(int ran);
int  qm_nxdn_get_bw(void);
void qm_nxdn_set_bw(int narrow);
#endif

#ifdef UI_MODE_AIRBAND
int  qm_am_get_sql(void);
void qm_am_set_sql(int level);
int  qm_am_get_agc(void);
void qm_am_set_agc(int on);
int  qm_am_get_step(void);
void qm_am_set_step(int raster833);
int  qm_am_get_snap(void);
void qm_am_set_snap(int on);
int  qm_am_get_if(void);
void qm_am_set_if(int wide);
#endif

/*
 * Formatters of the receive-only digital modes. They are the rows' own, so a
 * test can check what a screen would print (and that nothing prints a zero
 * where there is no data) without opening the popup.
 */
#ifdef UI_MODE_DPMR
void qm_dpmr_fmt_sync(char *buf, int len);
#endif

#ifdef UI_MODE_P25
void qm_p25_fmt_sync(char *buf, int len);
void qm_p25_fmt_nac(char *buf, int len);
void qm_p25_fmt_type(char *buf, int len);
void qm_p25_fmt_call(char *buf, int len);
#endif

#ifdef UI_MODE_YSF
void qm_ysf_fmt_sync(char *buf, int len);
#endif

#ifdef UI_MODE_DSTAR
void qm_dstar_fmt_sync(char *buf, int len);
void qm_dstar_fmt_my(char *buf, int len);
void qm_dstar_fmt_ur(char *buf, int len);
void qm_dstar_fmt_rpt(char *buf, int len);
void qm_dstar_fmt_text(char *buf, int len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* UI_MODES_H */
