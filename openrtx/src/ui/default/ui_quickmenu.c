/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contextual quick menu (see ui/ui_quickmenu.h and docs/UI.md).
 *
 * One table per operating mode, in the order the settings are reached for in
 * the field. Every setter writes state.* and either requests a resync (values
 * that travel in rtxStatus_t) or applies the change directly (brightness, the
 * keypad lock, the TETRA AFC flag, the fine RX offset).
 *
 * Platform features are compile-time: a platform that does not define
 * CONFIG_KEY_BACKLIGHT or CONFIG_RX_FINE_OFFSET does not get those rows, so no
 * item is ever a dead option.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <hwconfig.h>
#include "core/state.h"
#include "core/cps.h"
#include "core/messaging.h"
#include "core/ui.h"
#include "core/beeps.h"
#include "core/graphics.h"
#include "core/voicePromptUtils.h"
#include "interfaces/display.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
#include "ui/ui_hints.h"
#include "ui/ui_messages.h"
#include "ui/ui_quickmenu.h"
#ifdef CONFIG_TETRA
#include "protocols/TETRA/TetraStatus.h"
#endif
#ifdef CONFIG_DMR
#include "protocols/DMR/DmrStatus.h"
#endif

#define QM_TOP_Y (layout.top_h)
#define QM_ROW_Y(i) (QM_TOP_Y + QM_HEADER_H + (i) * QM_ROW_H)

qm_state_t qm_state;

static const ui_hints_t qm_hints = { "OK", "-", "+", "Close" };

static void qm_scroll(void);

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

void qm_requestSync(void)
{
    ui_rtxSyncRequest = true;
}

static enum vpQueueFlags qm_vpFlags(void)
{
    return vp_getVoiceLevelQueueFlags();
}

static void qm_clamp(int *value, const qm_item_t *item)
{
    if (*value < item->min)
        *value = item->min;
    if (*value > item->max)
        *value = item->max;
}

static int qm_current(const qm_item_t *item)
{
    return (item->get != NULL) ? item->get() : 0;
}

static const qm_menu_t *qm_active(void)
{
    return qm_state.qm;
}

/* ------------------------------------------------------------------------- */
/* Items shared by all modes                                                  */
/* ------------------------------------------------------------------------- */

const char *const qm_names_onoff[] = { "OFF", "ON" };
const char *const qm_names_mode[] = { "FM", "M17", "TETRA", "DMR" };
const char *const qm_names_bw[] = { "12.5", "25", "6.25" };
const char *const qm_names_ctcss[] = { "OFF", "TX", "RX", "BOTH" };
const char *const qm_names_shift[] = { "OFF", "-600k", "+600k", "-5M", "+5M" };

static const int32_t qm_shift_hz[] = { 0, -600000, 600000, -5000000, 5000000 };

int qm_get_power(void)
{
    return (int)state.channel.power;
}

void qm_set_power(int mw)
{
    if (mw < 1000)
        mw = 1000;
    if (mw > 5000)
        mw = 5000;

    state.channel.power = (uint32_t)mw;
    qm_requestSync();
    vp_announcePower((uint32_t)mw, qm_vpFlags());
}

void qm_fmt_power(char *buf, int len)
{
    int mw = (int)state.channel.power;
    if ((mw % 1000) == 0)
        snprintf(buf, len, "%dW", mw / 1000);
    else
        snprintf(buf, len, "%d.%dW", mw / 1000, (mw % 1000) / 100);
}

int qm_get_brightness(void)
{
    return state.settings.brightness;
}

void qm_set_brightness(int pct)
{
    /* Never store zero: on the C62 state_terminate() never runs (no graceful
       shutdown) and the radio would boot to a black screen. */
    if (pct < 5)
        pct = 5;
    if (pct > 100)
        pct = 100;

    state.settings.brightness = (uint8_t)pct;
    display_setBacklightLevel((uint8_t)pct);
    vp_announceSettingsInt(&currentLanguage->brightness, qm_vpFlags(), pct);
}

int qm_get_lock(void)
{
    return ui_getInputLock() ? 1 : 0;
}

void qm_set_lock(int on)
{
    ui_setInputLock(on != 0);
}

int qm_get_rx_fine(void)
{
    return state.settings.rxFineHz;
}

void qm_set_rx_fine(int hz)
{
    if (hz < -500)
        hz = -500;
    if (hz > 500)
        hz = 500;
    hz = (hz / 10) * 10; /* the BK4819 tunes in 10 Hz steps */

    state.settings.rxFineHz = (int16_t)hz;
#ifdef CONFIG_RX_FINE_OFFSET
    /*
     * The radio adds it to the channel frequency on every retune, next to the
     * TETRA correction (radio_C62.cpp), so it can never overwrite it: ask for
     * a resync and let the rtx thread retune.
     */
    qm_requestSync();
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
#endif
}

void qm_fmt_fine(char *buf, int len)
{
    snprintf(buf, len, "%+dHz", state.settings.rxFineHz);
}

#ifdef CONFIG_KEY_BACKLIGHT
int qm_get_keylight(void)
{
    return state.settings.keyBacklight ? 1 : 0;
}

void qm_set_keylight(int on)
{
    state.settings.keyBacklight = (on != 0);
    c62_setKeyBacklight(state.settings.keyBacklight);
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
}
#endif

int qm_get_mode(void)
{
    return state.channel.mode;
}

void qm_set_mode(int opmode)
{
    state.channel.mode = (uint8_t)opmode;
    qm_requestSync();
    vp_announceRadioMode(state.channel.mode, qm_vpFlags());
    /* The table depends on the mode: rebuild and keep the cursor in range */
    qm_state.qm = qm_findMenu(state.channel.mode);
    qm_state.selected = 0;
    qm_state.first = 0;
}

/* Unread messages badge, shown on the MSGS row of every table */
static void qm_fmt_unread(char *buf, int len)
{
    int n = messaging_unread();
    if (n <= 0)
        buf[0] = '\0';
    else
        snprintf(buf, len, "!%d", n);
}

void qm_fmt_frequency(char *buf, int len)
{
    freq_t freq = state.channel.rx_frequency;
    snprintf(buf, len, "%lu.%05lu", (unsigned long)(freq / 1000000UL),
             (unsigned long)((freq % 1000000UL) / 10UL));
}

/* ------------------------------------------------------------------------- */
/* FM                                                                         */
/* ------------------------------------------------------------------------- */

int qm_fm_get_sql(void)
{
    return state.settings.sqlLevel;
}

void qm_fm_set_sql(int level)
{
    if (level < 0)
        level = 0;
    if (level > 15)
        level = 15;
    state.settings.sqlLevel = (uint8_t)level;
    qm_requestSync();
    vp_announceSquelch(state.settings.sqlLevel, qm_vpFlags());
}

int qm_fm_get_ctcss(void)
{
    return (state.channel.fm.rxToneEn << 1) | state.channel.fm.txToneEn;
}

void qm_fm_set_ctcss(int flags)
{
    state.channel.fm.txToneEn = (flags & 1) ? 1 : 0;
    state.channel.fm.rxToneEn = (flags & 2) ? 1 : 0;
    qm_requestSync();
    vp_announceCTCSS(state.channel.fm.rxToneEn, state.channel.fm.rxTone,
                     state.channel.fm.txToneEn, state.channel.fm.txTone,
                     qm_vpFlags());
}

int qm_fm_get_tone(void)
{
    return state.channel.fm.txTone;
}

void qm_fm_set_tone(int index)
{
    if (index < 0)
        index = CTCSS_FREQ_NUM - 1;
    index %= CTCSS_FREQ_NUM;

    state.channel.fm.txTone = (uint8_t)index;
    state.channel.fm.rxTone = (uint8_t)index;
    qm_requestSync();
    vp_announceCTCSS(state.channel.fm.rxToneEn, state.channel.fm.rxTone,
                     state.channel.fm.txToneEn, state.channel.fm.txTone,
                     qm_vpFlags());
}

void qm_fm_fmt_tone(char *buf, int len)
{
    uint16_t tone = ctcss_tone[state.channel.fm.txTone];
    snprintf(buf, len, "%u.%u", (unsigned)tone / 10, (unsigned)tone % 10);
}

int qm_fm_get_bw(void)
{
    return state.channel.bandwidth;
}

void qm_fm_set_bw(int bw)
{
    if (bw < BW_12_5)
        bw = BW_6_25;
    if (bw > BW_6_25)
        bw = BW_12_5;
    state.channel.bandwidth = (uint8_t)bw;
    qm_requestSync();
    vp_announceBandwidth((uint8_t)bw, qm_vpFlags());
}

int qm_fm_get_shift(void)
{
    int32_t delta = (int32_t)state.channel.tx_frequency
                  - (int32_t)state.channel.rx_frequency;
    for (size_t i = 0; i < sizeof(qm_shift_hz) / sizeof(qm_shift_hz[0]); i++) {
        if (delta == qm_shift_hz[i])
            return (int)i;
    }
    return 0;
}

void qm_fm_set_shift(int index)
{
    if (index < 0 || index >= (int)(sizeof(qm_shift_hz) / sizeof(qm_shift_hz[0])))
        index = 0;
    state.channel.tx_frequency = (freq_t)((int32_t)state.channel.rx_frequency
                                + qm_shift_hz[index]);
    qm_requestSync();
}

int qm_fm_get_burst(void)
{
    return state.tone_enabled ? 1 : 0;
}

void qm_fm_set_burst(int on)
{
    state.tone_enabled = (on != 0);
    qm_requestSync();
}

/* ------------------------------------------------------------------------- */
/* M17                                                                        */
/* ------------------------------------------------------------------------- */

int qm_m17_get_can(void)
{
    return state.settings.m17_can;
}

void qm_m17_set_can(int can)
{
    if (can < 0)
        can = 15;
    if (can > 15)
        can = 0;
    state.settings.m17_can = (uint8_t)can;
    qm_requestSync();
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
}

int qm_m17_get_canrx(void)
{
    return state.settings.m17_can_rx ? 1 : 0;
}

void qm_m17_set_canrx(int on)
{
    state.settings.m17_can_rx = (on != 0);
    qm_requestSync();
}

void qm_m17_fmt_dest(char *buf, int len)
{
    if (state.settings.m17_dest[0] == '\0')
        snprintf(buf, len, "ALL");
    else
        snprintf(buf, len, "%s", state.settings.m17_dest);
}

void qm_m17_fmt_src(char *buf, int len)
{
    if (state.settings.callsign[0] == '\0')
        snprintf(buf, len, "-");
    else
        snprintf(buf, len, "%s", state.settings.callsign);
}

void qm_m17_fmt_meta(char *buf, int len)
{
    if (state.settings.M17_meta_text[0] == '\0')
        snprintf(buf, len, "-");
    else
        snprintf(buf, len, "%s", state.settings.M17_meta_text);
}

/* ------------------------------------------------------------------------- */
/* TETRA                                                                      */
/* ------------------------------------------------------------------------- */

#ifdef CONFIG_TETRA
int qm_tetra_get_afc(void)
{
    return state.settings.tetraAfcOff ? 0 : 1;
}

void qm_tetra_set_afc(int on)
{
    /* The mode reads the flag on every block: no resync needed */
    state.settings.tetraAfcOff = (on == 0) ? 1 : 0;
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
}

void qm_tetra_fmt_cell(char *buf, int len)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);
    if (ts.cell)
        snprintf(buf, len, "%03u-%02u", (unsigned)ts.mcc, (unsigned)ts.mnc);
    else
        snprintf(buf, len, "---");
}

void qm_tetra_fmt_lacc(char *buf, int len)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);
    if (ts.sysinfo)
        snprintf(buf, len, "LA%u CC%u", (unsigned)ts.la,
                 (unsigned)ts.colourCode);
    else
        snprintf(buf, len, "LA- CC%u", (unsigned)ts.colourCode);
}

void qm_tetra_fmt_call(char *buf, int len)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);
    if (!ts.call) {
        snprintf(buf, len, "IDLE");
        return;
    }
    snprintf(buf, len, "%lu>%lu%s%s", (unsigned long)ts.talker,
             (unsigned long)ts.callDst, ts.callGroup ? " G" : "",
             ts.callEncrypted ? " E" : "");
}
#endif /* CONFIG_TETRA */

/* ------------------------------------------------------------------------- */
/* DMR                                                                        */
/* ------------------------------------------------------------------------- */

#ifdef CONFIG_DMR
void qm_dmr_fmt_cc(char *buf, int len)
{
    struct dmrStatus st;
    dmr_getStatus(&st);
    if (st.colourCode < 0)
        snprintf(buf, len, "CC-");
    else
        snprintf(buf, len, "CC%u", (unsigned)st.colourCode);
}

void qm_dmr_fmt_ts(char *buf, int len)
{
    struct dmrStatus st;
    dmr_getStatus(&st);
    if (st.timeslot <= 0)
        snprintf(buf, len, "TS-");
    else
        snprintf(buf, len, "TS%u", (unsigned)st.timeslot);
}

void qm_dmr_fmt_sync(char *buf, int len)
{
    struct dmrStatus st;
    dmr_getStatus(&st);
    if (st.sync)
        snprintf(buf, len, "SYNC e%u%%", (unsigned)(st.evm / 10));
    else
        snprintf(buf, len, "SRCH");
}
#endif /* CONFIG_DMR */

/* ------------------------------------------------------------------------- */
/* Tables                                                                     */
/* ------------------------------------------------------------------------- */

#define QM_ITEM_INT(label, min, max, step, get, set) \
    { label, QM_INT, min, max, step, get, set, NULL, NULL, 0, 0 }
#define QM_ITEM_INT_F(label, min, max, step, get, set, fmt) \
    { label, QM_INT, min, max, step, get, set, NULL, fmt, 0, 0 }
#define QM_ITEM_ENUM(label, min, max, get, set, names, fmt) \
    { label, QM_ENUM, min, max, 1, get, set, names, fmt, 0, 0 }
#define QM_ITEM_TOGGLE(label, get, set) \
    { label, QM_TOGGLE, 0, 1, 1, get, set, qm_names_onoff, NULL, 0, 0 }
#define QM_ITEM_INFO(label, fmt) \
    { label, QM_INFO, 0, 0, 1, NULL, NULL, NULL, fmt, 0, 0 }
#define QM_ITEM_GOTO(label, screen, item) \
    { label, QM_GOTO, 0, 0, 1, NULL, NULL, NULL, NULL, screen, item }
#define QM_ITEM_GOTO_F(label, screen, fmt) \
    { label, QM_GOTO, 0, 0, 1, NULL, NULL, NULL, fmt, screen, 0 }
#define QM_ITEM_MODE() \
    { "MODE", QM_MODE, 0, 3, 1, qm_get_mode, qm_set_mode, qm_names_mode, \
      NULL, 0, 0 }

static const qm_item_t qm_items_fm[] =
{
    QM_ITEM_INT("SQL", 0, 15, 1, qm_fm_get_sql, qm_fm_set_sql),
    QM_ITEM_INT_F("PWR", 1000, 5000, 500, qm_get_power, qm_set_power,
                  qm_fmt_power),
    QM_ITEM_ENUM("CTCSS", 0, 3, qm_fm_get_ctcss, qm_fm_set_ctcss,
                 qm_names_ctcss, NULL),
    QM_ITEM_ENUM("TONE", 0, CTCSS_FREQ_NUM - 1, qm_fm_get_tone, qm_fm_set_tone,
                 NULL, qm_fm_fmt_tone),
    QM_ITEM_ENUM("BW", BW_12_5, BW_6_25, qm_fm_get_bw, qm_fm_set_bw,
                 qm_names_bw, NULL),
    QM_ITEM_ENUM("SHIFT", 0, 4, qm_fm_get_shift, qm_fm_set_shift,
                 qm_names_shift, NULL),
    QM_ITEM_TOGGLE("1750", qm_fm_get_burst, qm_fm_set_burst),
    QM_ITEM_INT("BKL", 5, 100, 5, qm_get_brightness, qm_set_brightness),
#ifdef CONFIG_KEY_BACKLIGHT
    QM_ITEM_TOGGLE("KEYLIT", qm_get_keylight, qm_set_keylight),
#endif
    QM_ITEM_TOGGLE("LOCK", qm_get_lock, qm_set_lock),
    QM_ITEM_GOTO_F("MSGS", MENU_MESSAGES, qm_fmt_unread),
    QM_ITEM_MODE(),
};

static const qm_item_t qm_items_m17[] =
{
    QM_ITEM_GOTO("DEST", SETTINGS_M17, M17_CALLSIGN),
    QM_ITEM_INT("CAN", 0, 15, 1, qm_m17_get_can, qm_m17_set_can),
    QM_ITEM_TOGGLE("CANRX", qm_m17_get_canrx, qm_m17_set_canrx),
    QM_ITEM_INFO("SRC", qm_m17_fmt_src),
    QM_ITEM_GOTO("META", SETTINGS_M17, M17_METATEXT),
    QM_ITEM_INT_F("PWR", 1000, 5000, 500, qm_get_power, qm_set_power,
                  qm_fmt_power),
    QM_ITEM_INT("BKL", 5, 100, 5, qm_get_brightness, qm_set_brightness),
#ifdef CONFIG_KEY_BACKLIGHT
    QM_ITEM_TOGGLE("KEYLIT", qm_get_keylight, qm_set_keylight),
#endif
    QM_ITEM_TOGGLE("LOCK", qm_get_lock, qm_set_lock),
    QM_ITEM_GOTO_F("MSGS", MENU_MESSAGES, qm_fmt_unread),
    QM_ITEM_MODE(),
};

#ifdef CONFIG_TETRA
static const qm_item_t qm_items_tetra[] =
{
    QM_ITEM_TOGGLE("AFC", qm_tetra_get_afc, qm_tetra_set_afc),
#ifdef CONFIG_RX_FINE_OFFSET
    QM_ITEM_INT_F("FINE", -500, 500, 10, qm_get_rx_fine, qm_set_rx_fine,
                  qm_fmt_fine),
#endif
    QM_ITEM_INFO("CELL", qm_tetra_fmt_cell),
    QM_ITEM_INFO("LA/CC", qm_tetra_fmt_lacc),
    QM_ITEM_INFO("CALL", qm_tetra_fmt_call),
    QM_ITEM_INT("BKL", 5, 100, 5, qm_get_brightness, qm_set_brightness),
#ifdef CONFIG_KEY_BACKLIGHT
    QM_ITEM_TOGGLE("KEYLIT", qm_get_keylight, qm_set_keylight),
#endif
    QM_ITEM_TOGGLE("LOCK", qm_get_lock, qm_set_lock),
    QM_ITEM_GOTO_F("MSGS", MENU_MESSAGES, qm_fmt_unread),
    QM_ITEM_MODE(),
};
#endif /* CONFIG_TETRA */

#ifdef CONFIG_DMR
static const qm_item_t qm_items_dmr[] =
{
    QM_ITEM_INFO("CC", qm_dmr_fmt_cc),
    QM_ITEM_INFO("TS", qm_dmr_fmt_ts),
    QM_ITEM_INFO("SYNC", qm_dmr_fmt_sync),
#ifdef CONFIG_RX_FINE_OFFSET
    QM_ITEM_INT_F("FINE", -500, 500, 10, qm_get_rx_fine, qm_set_rx_fine,
                  qm_fmt_fine),
#endif
    QM_ITEM_INT("BKL", 5, 100, 5, qm_get_brightness, qm_set_brightness),
#ifdef CONFIG_KEY_BACKLIGHT
    QM_ITEM_TOGGLE("KEYLIT", qm_get_keylight, qm_set_keylight),
#endif
    QM_ITEM_TOGGLE("LOCK", qm_get_lock, qm_set_lock),
    QM_ITEM_GOTO_F("MSGS", MENU_MESSAGES, qm_fmt_unread),
    QM_ITEM_MODE(),
};
#endif /* CONFIG_DMR */

#define QM_MENU_ENTRY(omode, title_, table) \
    { (omode), (title_), (table), (uint8_t)(sizeof(table) / sizeof((table)[0])) }

static const qm_menu_t qm_menus[] =
{
    QM_MENU_ENTRY(OPMODE_FM, "FM", qm_items_fm),
#ifdef CONFIG_M17
    QM_MENU_ENTRY(OPMODE_M17, "M17", qm_items_m17),
#endif
#ifdef CONFIG_TETRA
    QM_MENU_ENTRY(OPMODE_TETRA, "TETRA", qm_items_tetra),
#endif
#ifdef CONFIG_DMR
    QM_MENU_ENTRY(OPMODE_DMR, "DMR", qm_items_dmr),
#endif
};

#define QM_MENU_COUNT ((uint8_t)(sizeof(qm_menus) / sizeof(qm_menus[0])))

const qm_menu_t *qm_findMenu(uint8_t opmode)
{
    for (uint8_t i = 0; i < QM_MENU_COUNT; i++) {
        if (qm_menus[i].mode == opmode)
            return &qm_menus[i];
    }
    return &qm_menus[0]; /* FM: never leave the popup empty */
}

/* ------------------------------------------------------------------------- */
/* State machine                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Geometry. The screen is 160x128, layout.top_h is 16 and the hint bar takes
 * the last 11 px, so the popup has 16..116 for a 14 px header and 10 px rows:
 * eight rows fit and the ninth would land on the hints.
 */
uint8_t qm_rowHeight(void)
{
    return QM_ROW_H;
}

uint8_t qm_rowBaseline(void)
{
    return QM_ROW_BASELINE;
}

uint8_t qm_visibleRows(void)
{
    int16_t avail = (int16_t)(CONFIG_SCREEN_HEIGHT - UI_HINTS_H
                              - (layout.top_h + QM_HEADER_H));
    if (avail < QM_ROW_H)
        return 0;
    return (uint8_t)(avail / QM_ROW_H);
}

int16_t qm_rowTop(uint8_t row)
{
    return (int16_t)(QM_TOP_Y + QM_HEADER_H + row * QM_ROW_H);
}

static void qm_scroll(void)
{
    const qm_menu_t *qm = qm_active();
    uint8_t rows = qm_visibleRows();

    if ((qm == NULL) || (rows == 0))
        return;

    if (qm_state.selected < qm_state.first)
        qm_state.first = qm_state.selected;
    if (qm_state.selected >= qm_state.first + rows)
        qm_state.first = (uint8_t)(qm_state.selected - rows + 1);

    /* Never leave blank lines above the last items */
    if (qm->count > rows && qm_state.first > (uint8_t)(qm->count - rows))
        qm_state.first = (uint8_t)(qm->count - rows);
}

void qm_open(void)
{
    qm_state.qm = qm_findMenu(state.channel.mode);
    qm_state.open = true;
    qm_state.latched = false;   /* long P2 latches it, see qm_toggleLatch() */
    qm_state.selected = 0;
    qm_state.first = 0;
    qm_state.lastKeyTick = getTick();
    vp_playMenuBeepIfNeeded(true);
}

void qm_close(void)
{
    qm_state.open = false;
    qm_state.qm = NULL;
}

void qm_toggleLatch(void)
{
    qm_state.latched = !qm_state.latched;
    vp_beep(qm_state.latched ? BEEP_FUNCTION_LATCH_ON : BEEP_FUNCTION_LATCH_OFF,
            LONG_BEEP);
}

void qm_select(uint8_t index)
{
    if ((qm_state.qm == NULL) || (index >= qm_state.qm->count))
        return;
    qm_state.selected = index;
    qm_scroll();
}

void qm_move(int delta)
{
    if ((qm_state.qm == NULL) || (qm_state.qm->count == 0))
        return;

    if (delta < 0) {
        if (qm_state.selected > 0)
            qm_state.selected--;
        else
            qm_state.selected = qm_state.qm->count - 1;
    } else {
        if (qm_state.selected + 1 < qm_state.qm->count)
            qm_state.selected++;
        else
            qm_state.selected = 0;
    }
    vp_playMenuBeepIfNeeded(qm_state.selected == 0);
    qm_scroll();
}

static const qm_item_t *qm_selected(void)
{
    if ((qm_state.qm == NULL) || (qm_state.selected >= qm_state.qm->count))
        return NULL;
    return &qm_state.qm->items[qm_state.selected];
}

void qm_adjust(int delta)
{
    const qm_item_t *item = qm_selected();

    if ((item == NULL) || (delta == 0))
        return;

    /* Read only rows: no value to change, but never silently do nothing */
    if ((item->type == QM_INFO) || (item->type == QM_GOTO)
        || (item->get == NULL) || (item->set == NULL)) {
        vp_beep(400, SHORT_BEEP);
        return;
    }

    int value = qm_current(item) + delta * item->step;

    switch (item->type) {
        case QM_ENUM:
        case QM_TOGGLE:
        case QM_MODE: {
            /* Enumerations wrap: the last entry is next to the first one */
            int span = item->max - item->min + 1;
            if (span <= 0)
                return;
            while (value < item->min)
                value += span * abs(delta);
            while (value > item->max)
                value -= span * abs(delta);
            break;
        }

        case QM_INT:
        default:
            /* Numbers stop at their limits: a squelch at 15 stays at 15 */
            qm_clamp(&value, item);
            break;
    }

    item->set(value);
}

void qm_enter(void)
{
    const qm_item_t *item = qm_selected();

    if (item == NULL)
        return;

    switch (item->type) {
        case QM_INT:
        case QM_ENUM:
        case QM_TOGGLE:
        case QM_MODE:
            /* Green is the same as P2: cycle forward. It never closes. */
            qm_adjust(+1);
            break;

        case QM_GOTO:
            qm_close();
            if (item->goto_screen == MENU_MESSAGES)
                ui_msgs_open();
            else
                state.ui_screen = item->goto_screen;
            break;

        case QM_INFO:
        default:
            /* Read only: no action, no error beep */
            break;
    }
}

void qm_cancel(void)
{
    qm_close();
}

bool qm_tick(long long now)
{
    if (!qm_state.open)
        return false;

    /* A mode change from outside (key 5) must follow the table */
    if ((qm_state.qm != NULL) && (qm_state.qm->mode != state.channel.mode)) {
        qm_state.qm = qm_findMenu(state.channel.mode);
        qm_state.selected = 0;
        qm_state.first = 0;
        return true;
    }

    if (!qm_state.latched && (now - qm_state.lastKeyTick > QM_TIMEOUT_MS)) {
        qm_close();
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* ------------------------------------------------------------------------- */

bool qm_value_text(uint8_t index, char *buf, int len)
{
    if (buf == NULL || len <= 0)
        return false;
    buf[0] = '\0';

    if ((qm_state.qm == NULL) || (index >= qm_state.qm->count))
        return false;

    const qm_item_t *item = &qm_state.qm->items[index];

    if (item->fmt != NULL) {
        item->fmt(buf, len);
        return true;
    }
    if ((item->type == QM_ENUM || item->type == QM_TOGGLE
         || item->type == QM_MODE) && item->names != NULL && item->get != NULL) {
        int value = qm_current(item) - item->min;
        if (value < 0 || value > item->max - item->min)
            return false;
        snprintf(buf, len, "%s", item->names[value]);
        return true;
    }
    if (item->get != NULL) {
        snprintf(buf, len, "%d", qm_current(item));
        return true;
    }
    return false;
}

/*
 * Make a value fit in `avail` pixels of the 5 pt font. Long values are cut on
 * the left, where the least information is (a frequency keeps its tail), and
 * marked with '<' so that a shortened value never looks like a real one.
 */
static void qm_fitValue(char *buf, size_t len, uint16_t avail)
{
    if ((buf[0] == '\0') || (gfx_getTextWidth(FONT_SIZE_5PT, buf) <= avail))
        return;

    size_t n = strlen(buf);
    while (n > 0) {
        char tmp[QM_VALUE_BUF];
        memmove(tmp, buf + (strlen(buf) - n), n);
        tmp[n] = '\0';
        if (gfx_getTextWidth(FONT_SIZE_5PT, tmp) <= avail - 4)
            break;
        n--;
    }
    /* Prefix in place: no snprintf, whose overlapping source and destination
       is exactly the warning gcc reports here. */
    memmove(buf + 1, buf + (strlen(buf) - n), n);
    buf[0] = '<';
    buf[n + 1] = '\0';
    (void)len;
}

/* Header: mode on the left, frequency (or the mode's own state) on the right */
static void qm_drawHeader(void)
{
    char right[QM_VALUE_BUF] = "";
    point_t pos = { QM_TEXT_X, (int16_t)(QM_TOP_Y + QM_HEADER_H - 4) };

    gfx_print(pos, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, yellow_fab413, "%s",
              qm_state.qm->title);

    if (qm_state.qm->mode == OPMODE_TETRA) {
#ifdef CONFIG_TETRA
        struct tetraStatus ts;
        tetra_getStatus(&ts);
        if (ts.cell)
            snprintf(right, sizeof(right), "%03u-%02u", (unsigned)ts.mcc,
                     (unsigned)ts.mnc);
        else
            snprintf(right, sizeof(right), "SEARCHING");
#endif
    }

    if (right[0] == '\0')
        qm_fmt_frequency(right, sizeof(right));

    gfx_print((point_t){ QM_VALUE_X, pos.y }, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT,
              color_white, "%s", right);
}

void qm_draw(void)
{
    if (!qm_state.open || qm_state.qm == NULL)
        return;

    /* Cover what is under the popup, then draw the popup */
    gfx_drawRect((point_t){ 0, 0 }, CONFIG_SCREEN_WIDTH, CONFIG_SCREEN_HEIGHT,
                 color_black, true);

    qm_drawHeader();
    gfx_drawHLine(QM_TOP_Y + QM_HEADER_H - 1, 1, color_grey);

    uint8_t rows = qm_visibleRows();

    for (uint8_t row = 0; row < rows; row++) {
        uint8_t index = (uint8_t)(qm_state.first + row);
        if (index >= qm_state.qm->count)
            break;

        const qm_item_t *item = &qm_state.qm->items[index];
        bool selected = (index == qm_state.selected);
        int16_t top = qm_rowTop(row);
        /* Baseline inside the row: glyphs occupy top+3 .. top+7 (5 pt cap
           height is 5 px with a yOffset of -5), so the text can never spill
           into the row above, which was the visible bug. */
        int16_t baseline = (int16_t)(top + QM_ROW_BASELINE);
        char value[QM_VALUE_BUF] = "";

        if (selected)
            gfx_drawRect((point_t){ 0, top }, CONFIG_SCREEN_WIDTH, QM_ROW_H,
                         color_white, true);

        color_t fg = selected ? color_black : color_white;
        uint16_t label_w = gfx_getTextWidth(FONT_SIZE_5PT, item->label);
        uint16_t value_x = (uint16_t)(QM_TEXT_X + label_w + 4);
        uint16_t avail = (uint16_t)(CONFIG_SCREEN_WIDTH - QM_VALUE_X - value_x);

        gfx_print((point_t){ QM_TEXT_X, baseline }, FONT_SIZE_5PT,
                  TEXT_ALIGN_LEFT, fg, "%s", item->label);

        if (qm_value_text(index, value, sizeof(value))) {
            qm_fitValue(value, sizeof(value), avail);
            gfx_print((point_t){ QM_VALUE_X, baseline }, FONT_SIZE_5PT,
                      TEXT_ALIGN_RIGHT, fg, "%s", value);
        }
    }

    ui_hints_draw(&qm_hints);
}
