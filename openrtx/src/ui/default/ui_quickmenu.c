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
#include "core/airband.h"
#include "core/channel_store.h"
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
#include "ui/ui_layout.h"
#include "ui/ui_messages.h"
#include "ui/ui_modes.h"
#include "ui/ui_quickmenu.h"
#ifdef CONFIG_TETRA
#include "protocols/TETRA/TetraStatus.h"
#endif
#ifdef CONFIG_DMR
#include "protocols/DMR/DmrStatus.h"
#endif
#ifdef UI_MODE_NXDN
#include "protocols/NXDN/NxdnStatus.h"
#endif

#define QM_TOP_Y (layout.top_h)
#define QM_ROW_Y(i) (QM_TOP_Y + QM_HEADER_H + (i) * QM_ROW_H)

qm_state_t qm_state;

static const ui_hints_t qm_hints = { "OK", "-", "+", "Close" };

static void qm_scroll(void);
static void qm_rebuild(void);

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
const char *const qm_names_bw[] = { "12.5", "25", "6.25" };
const char *const qm_names_ctcss[] = { "OFF", "TX", "RX", "BOTH" };
const char *const qm_names_shift[] = { "OFF", "-600k", "+600k", "-5M", "+5M" };
#ifdef DMR_ID_MAX
const char *const qm_names_destType[] = { "GROUP", "USER" };
#endif

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

#if defined(CONFIG_DMR) && defined(DMR_ID_MAX)
/*
 * DMR call destination (docs/UI.md, "DMR"): the talkgroup or the subscriber ID
 * the next call is addressed to. The values live in settings (the model's
 * settings record) and reach the transmitter through the RTX resync, so the
 * quick menu only writes them and asks for a sync; the UI never touches
 * rtxStatus_t.
 */
int qm_dmr_getDestType(void)
{
    return (state.settings.dmr_dest_type == DMR_DEST_USER) ? DMR_DEST_USER
                                                           : DMR_DEST_GROUP;
}

void qm_dmr_setDestType(int type)
{
    state.settings.dmr_dest_type = (uint8_t)((type == DMR_DEST_USER)
                                             ? DMR_DEST_USER : DMR_DEST_GROUP);
    qm_requestSync();
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
}

int qm_dmr_getDest(void)
{
    int id = (int)state.settings.dmr_dest_id;

    if (id < (int)DMR_ID_MIN)
        id = (int)DMR_ID_MIN;
    if (id > (int)DMR_ID_MAX)
        id = (int)DMR_ID_MAX;
    return id;
}

void qm_dmr_setDest(int id)
{
    if (id < (int)DMR_ID_MIN)
        id = (int)DMR_ID_MIN;
    if (id > (int)DMR_ID_MAX)
        id = (int)DMR_ID_MAX;

    state.settings.dmr_dest_id = (uint32_t)id;
    qm_requestSync();
}

void qm_dmr_fmt_dest(char *buf, int len)
{
    snprintf(buf, len, "%s%d",
             (state.settings.dmr_dest_type == DMR_DEST_USER) ? "ID" : "TG",
             qm_dmr_getDest());
}
#endif /* CONFIG_DMR && DMR_ID_MAX */

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
/* NXDN rows                                                                  */
/* ------------------------------------------------------------------------- */

#ifdef UI_MODE_NXDN
/*
 * RAN is a receive filter, not a read-only number: the operator sets the RAN of
 * the site he wants to listen to and the mode drops voice and layer 3 from
 * every other one. The SACCH of a superframe is what carries it (the LICH does
 * not), so the filter can only close once a superframe has decoded: 320 ms
 * after the carrier appears, and until then everything passes. 64 is "ANY",
 * the default, which is the behaviour the mode had before the row existed.
 */
#define QM_NXDN_RAN_ANY 64

int qm_nxdn_get_ran(void)
{
    return (state.settings.nxdn_ran > 63) ? QM_NXDN_RAN_ANY
                                          : (int)state.settings.nxdn_ran;
}

void qm_nxdn_set_ran(int ran)
{
    if ((ran < 0) || (ran >= QM_NXDN_RAN_ANY))
        state.settings.nxdn_ran = 0xFF;
    else
        state.settings.nxdn_ran = (uint8_t)ran;
}

void qm_nxdn_fmt_ran(char *buf, int len)
{
    int ran = qm_nxdn_get_ran();

    if (ran >= QM_NXDN_RAN_ANY)
        snprintf(buf, len, "ANY");
    else
        snprintf(buf, len, "RAN %d", ran);
}

/*
 * Channel spacing. NXDN48 fits in a 6.25 kHz channel and is also used on 12.5
 * kHz ones; the driver keeps the BK4819's 12.5 kHz channel mode either way and
 * only the IF filter changes, because the demodulator's matched filter is what
 * selects the channel.
 */
static const char *const qm_names_nxdn_bw[] = { "12.5", "6.25" };

int qm_nxdn_get_bw(void)
{
    return (state.channel.bandwidth == BW_6_25) ? 1 : 0;
}

void qm_nxdn_set_bw(int narrow)
{
    state.channel.bandwidth = narrow ? BW_6_25 : BW_12_5;
    qm_requestSync();
}

void qm_nxdn_fmt_sync(char *buf, int len)
{
    struct nxdnStatus st;

    nxdn_getStatus(&st);

    if (st.sync)
        snprintf(buf, len, "SYNC e%u%%", (unsigned)(st.evm / 10));
    else
        snprintf(buf, len, "SRCH");
}

/*
 * The last layer 3 message the SACCH of a superframe carried: who is calling
 * whom. Empty until four CRC-clean SACCH blocks have arrived, and never a zero
 * that would read as a real identifier.
 */
void qm_nxdn_fmt_call(char *buf, int len)
{
    struct nxdnStatus st;

    nxdn_getStatus(&st);

    if (!st.message.valid) {
        snprintf(buf, len, "---");
        return;
    }

    snprintf(buf, len, "%u>%u", (unsigned)st.message.source,
             (unsigned)st.message.dest);
}

void qm_nxdn_fmt_type(char *buf, int len)
{
    struct nxdnStatus st;

    nxdn_getStatus(&st);

    if (!st.message.valid) {
        snprintf(buf, len, "-");
        return;
    }

    switch (st.message.type) {
        case 0x01: snprintf(buf, len, "VCALL"); break;
        case 0x08: snprintf(buf, len, "TXREL"); break;
        case 0x11: snprintf(buf, len, "DISC");  break;
        default:   snprintf(buf, len, "0x%02X",
                            (unsigned)st.message.type); break;
    }
}
#endif /* UI_MODE_NXDN */

/* ------------------------------------------------------------------------- */
/* Airband AM rows                                                            */
/* ------------------------------------------------------------------------- */

#ifdef UI_MODE_AIRBAND
/*
 * Squelch. In AM the audio follows the level of the carrier, so the noise
 * squelch FM uses would open on the noise the AGC has just brought up and close
 * when an aircraft appears. OpMode_Airband therefore thresholds the RSSI, the
 * carrier itself, against this same level: the row is the one an operator
 * expects (SQL, 0..15) and what it measures is the carrier.
 */
int qm_am_get_sql(void)
{
    return (int)state.settings.sqlLevel;
}

void qm_am_set_sql(int level)
{
    if (level < 0)
        level = 0;
    if (level > 15)
        level = 15;

    state.settings.sqlLevel = (uint8_t)level;
    qm_requestSync();
}

/*
 * Audio AGC of the AM path (core/agc.h, run by radio_C62.cpp in the audio
 * worker). On by default: without it the volume follows the field strength of
 * the aircraft. Off is for a weak, steady signal where the operator would
 * rather hear the level move than the AGC pump.
 */
int qm_am_get_agc(void)
{
    return state.settings.am_agc ? 1 : 0;
}

void qm_am_set_agc(int on)
{
    /* Read by the receive path on every block, nothing to resynchronise */
    state.settings.am_agc = (on != 0);
}

/*
 * Channel raster. 25 kHz is the classic plan, 8.33 kHz the one mandated above
 * FL195 in Europe: three channels per 25 kHz slot, named with a designator
 * whose last two digits are 00, 05 or 10. The step is the global tuning step
 * (state.step_index), the same one Settings → Radio → Step edits, so the VFO
 * and the menu cannot disagree.
 */
int qm_am_get_step(void)
{
    return state.settings.am_step ? 1 : 0;
}

void qm_am_set_step(int raster833)
{
    uint32_t hz = raster833 ? 8333U : 25000U;

    state.settings.am_step = (raster833 != 0);

    for (size_t i = 0; i < n_freq_steps; i++) {
        if (freq_steps[i] == hz) {
            state.step_index = (uint8_t)i;
            break;
        }
    }
}

/*
 * Snap to the raster. The 8.33 kHz step is a whole number of hertz (8333), so
 * tuning by steps drifts off the real carrier (25/3 kHz) by a fraction of a
 * hertz per step; with this on, every step and every frequency entry lands on
 * the nearest channel of the raster instead.
 */
int qm_am_get_snap(void)
{
    return state.settings.am_snap ? 1 : 0;
}

void qm_am_set_snap(int on)
{
    state.settings.am_snap = (on != 0);
}

static const char *const qm_names_am_if[] = { "12.5", "25" };
static const char *const qm_names_am_25_833[] = { "25k", "8.33k" };

int qm_am_get_if(void)
{
    return (state.channel.bandwidth == BW_25) ? 1 : 0;
}

void qm_am_set_if(int wide)
{
    state.channel.bandwidth = wide ? BW_25 : BW_12_5;
    qm_requestSync();
}

/* Designator of the 8.33 kHz channel the receiver is on, e.g. "118.005" */
void qm_am_fmt_ch(char *buf, int len)
{
    char name[AIRBAND_NAME_LEN];

    if (!airband_inBand(state.channel.rx_frequency)) {
        snprintf(buf, len, "-");
        return;
    }

    airband_channelName(state.channel.rx_frequency, name, sizeof(name));
    snprintf(buf, len, "%s", name);
}
#endif /* UI_MODE_AIRBAND */

void qm_am_applyRaster(void)
{
#ifdef UI_MODE_AIRBAND
    if (state.channel.mode == OPMODE_AIRBAND)
        qm_am_set_step(state.settings.am_step ? 1 : 0);
#endif
}

/* ------------------------------------------------------------------------- */
/* Tables                                                                     */
/* ------------------------------------------------------------------------- */

#define QM_ITEM_INT(label, min, max, step, get, set) \
    { label, QM_INT, min, max, step, 0, get, set, NULL, NULL, 0, 0 }
#define QM_ITEM_INT_F(label, min, max, step, get, set, fmt) \
    { label, QM_INT, min, max, step, 0, get, set, NULL, fmt, 0, 0 }
#define QM_ITEM_INT_FF(label, min, max, step, fast, get, set, fmt) \
    { label, QM_INT, min, max, step, fast, get, set, NULL, fmt, 0, 0 }
#define QM_ITEM_ENUM(label, min, max, get, set, names, fmt) \
    { label, QM_ENUM, min, max, 1, 0, get, set, names, fmt, 0, 0 }
#define QM_ITEM_ENUM_FF(label, min, max, fast, get, set, names, fmt) \
    { label, QM_ENUM, min, max, 1, fast, get, set, names, fmt, 0, 0 }
#define QM_ITEM_TOGGLE(label, get, set) \
    { label, QM_TOGGLE, 0, 1, 1, 0, get, set, qm_names_onoff, NULL, 0, 0 }
#define QM_ITEM_INFO(label, fmt) \
    { label, QM_INFO, 0, 0, 1, 0, NULL, NULL, NULL, fmt, 0, 0 }
#define QM_ITEM_GOTO(label, screen, item) \
    { label, QM_GOTO, 0, 0, 1, 0, NULL, NULL, NULL, NULL, screen, item }
#define QM_ITEM_GOTO_F(label, screen, fmt) \
    { label, QM_GOTO, 0, 0, 1, 0, NULL, NULL, NULL, fmt, screen, 0 }

/*
 * The two rows every mode shares. MSG opens the composer with the transport of
 * the current mode (msg_service decides), so a text can be written without
 * walking the menu, and it is the same screen from FM, DMR, M17 and TETRA.
 * The mode itself is not here: the side key changes it, and a second way to do
 * it from a popup that rebuilds itself under the finger was the worst part of
 * the old table (docs/UI.md).
 */
#define QM_ITEM_MSG() \
    { "MSG", QM_GOTO, 0, 0, 1, 0, NULL, NULL, NULL, NULL, MSG_COMPOSE, 0 }

#ifdef CONFIG_APRS
#define QM_ITEM_APRS() \
    { "APRS", QM_GOTO, 0, 0, 1, 0, NULL, NULL, NULL, NULL, APRS_CFG, 0 }
#else
#define QM_ITEM_APRS()
#endif

#if defined(CONFIG_DMR) && defined(DMR_ID_MAX)
#define QM_ITEM_DMR_DEST() \
    QM_ITEM_ENUM("CALL", DMR_DEST_GROUP, DMR_DEST_USER, qm_dmr_getDestType, \
                 qm_dmr_setDestType, qm_names_destType, NULL), \
    QM_ITEM_INT_FF("DEST", (int)DMR_ID_MIN, (int)DMR_ID_MAX, 1, 100, \
                   qm_dmr_getDest, qm_dmr_setDest, qm_dmr_fmt_dest),
#else
#define QM_ITEM_DMR_DEST()
#endif

static const qm_item_t qm_items_fm[] =
{
    QM_ITEM_INT("SQL", 0, 15, 1, qm_fm_get_sql, qm_fm_set_sql),
    QM_ITEM_INT_F("PWR", 1000, 5000, 500, qm_get_power, qm_set_power,
                  qm_fmt_power),
    QM_ITEM_ENUM("CTCSS", 0, 3, qm_fm_get_ctcss, qm_fm_set_ctcss,
                 qm_names_ctcss, NULL),
    /* 50 tones: a long press walks them ten at a time */
    QM_ITEM_ENUM_FF("TONE", 0, CTCSS_FREQ_NUM - 1, 10, qm_fm_get_tone,
                    qm_fm_set_tone, NULL, qm_fm_fmt_tone),
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
    QM_ITEM_MSG(),
    QM_ITEM_APRS()
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
    QM_ITEM_MSG()
};

#ifdef CONFIG_TETRA
static const qm_item_t qm_items_tetra[] =
{
    QM_ITEM_TOGGLE("AFC", qm_tetra_get_afc, qm_tetra_set_afc),
#ifdef CONFIG_RX_FINE_OFFSET
    QM_ITEM_INT_FF("FINE", -500, 500, 10, 100, qm_get_rx_fine,
                   qm_set_rx_fine, qm_fmt_fine),
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
    QM_ITEM_MSG()
};
#endif /* CONFIG_TETRA */

#ifdef CONFIG_DMR
static const qm_item_t qm_items_dmr[] =
{
    QM_ITEM_INFO("CC", qm_dmr_fmt_cc),
    QM_ITEM_INFO("TS", qm_dmr_fmt_ts),
    QM_ITEM_INFO("SYNC", qm_dmr_fmt_sync),
    QM_ITEM_DMR_DEST()
#ifdef CONFIG_RX_FINE_OFFSET
    QM_ITEM_INT_FF("FINE", -500, 500, 10, 100, qm_get_rx_fine,
                   qm_set_rx_fine, qm_fmt_fine),
#endif
    QM_ITEM_INT("BKL", 5, 100, 5, qm_get_brightness, qm_set_brightness),
#ifdef CONFIG_KEY_BACKLIGHT
    QM_ITEM_TOGGLE("KEYLIT", qm_get_keylight, qm_set_keylight),
#endif
    QM_ITEM_TOGGLE("LOCK", qm_get_lock, qm_set_lock),
    QM_ITEM_GOTO_F("MSGS", MENU_MESSAGES, qm_fmt_unread),
    QM_ITEM_MSG()
};
#endif /* CONFIG_DMR */

#ifdef UI_MODE_NXDN
/*
 * NXDN, receive only. The first rows are what the mode can be told to do (RAN
 * filter, channel spacing of the demodulator), the next three are what the
 * demodulator reports (sync and error, the call the SACCH carried, its type),
 * and the rest is the tail every table shares.
 */
static const qm_item_t qm_items_nxdn[] =
{
    QM_ITEM_INT_F("RAN", 0, QM_NXDN_RAN_ANY, 1, qm_nxdn_get_ran,
                  qm_nxdn_set_ran, qm_nxdn_fmt_ran),
    QM_ITEM_ENUM("BW", 0, 1, qm_nxdn_get_bw, qm_nxdn_set_bw,
                 qm_names_nxdn_bw, NULL),
    QM_ITEM_INFO("SYNC", qm_nxdn_fmt_sync),
    QM_ITEM_INFO("CALL", qm_nxdn_fmt_call),
    QM_ITEM_INFO("TYPE", qm_nxdn_fmt_type),
#ifdef CONFIG_RX_FINE_OFFSET
    QM_ITEM_INT_FF("FINE", -500, 500, 10, 100, qm_get_rx_fine,
                   qm_set_rx_fine, qm_fmt_fine),
#endif
    QM_ITEM_INT("BKL", 5, 100, 5, qm_get_brightness, qm_set_brightness),
#ifdef CONFIG_KEY_BACKLIGHT
    QM_ITEM_TOGGLE("KEYLIT", qm_get_keylight, qm_set_keylight),
#endif
    QM_ITEM_TOGGLE("LOCK", qm_get_lock, qm_set_lock),
    QM_ITEM_GOTO_F("MSGS", MENU_MESSAGES, qm_fmt_unread),
    QM_ITEM_MSG()
};
#endif /* UI_MODE_NXDN */

#ifdef UI_MODE_AIRBAND
/*
 * Airband AM, receive only. Squelch, audio AGC, raster and the snap to it, the
 * designator of the channel the receiver is on, the IF filter, and the shared
 * tail. No PWR row: there is no transmitter in this mode.
 */
static const qm_item_t qm_items_airband[] =
{
    QM_ITEM_INT("SQL", 0, 15, 1, qm_am_get_sql, qm_am_set_sql),
    QM_ITEM_TOGGLE("AGC", qm_am_get_agc, qm_am_set_agc),
    QM_ITEM_ENUM("STEP", 0, 1, qm_am_get_step, qm_am_set_step,
                 qm_names_am_25_833, NULL),
    QM_ITEM_TOGGLE("SNAP", qm_am_get_snap, qm_am_set_snap),
    QM_ITEM_INFO("CH", qm_am_fmt_ch),
    QM_ITEM_ENUM("IF", 0, 1, qm_am_get_if, qm_am_set_if,
                 qm_names_am_if, NULL),
#ifdef CONFIG_RX_FINE_OFFSET
    QM_ITEM_INT_FF("FINE", -500, 500, 10, 100, qm_get_rx_fine,
                   qm_set_rx_fine, qm_fmt_fine),
#endif
    QM_ITEM_INT("BKL", 5, 100, 5, qm_get_brightness, qm_set_brightness),
#ifdef CONFIG_KEY_BACKLIGHT
    QM_ITEM_TOGGLE("KEYLIT", qm_get_keylight, qm_set_keylight),
#endif
    QM_ITEM_TOGGLE("LOCK", qm_get_lock, qm_set_lock),
    QM_ITEM_GOTO_F("MSGS", MENU_MESSAGES, qm_fmt_unread),
    QM_ITEM_MSG()
};
#endif /* UI_MODE_AIRBAND */

/*
 * The mode registry: one entry per operating mode this build has, in the order
 * the side key and every list walk them. This is the only list of modes in the
 * UI; see ui/ui_modes.h and docs/UI.md ("Añadir un modo").
 */
#define QM_MODE(omode, title_, table, flags_) \
    { (omode), (title_), (table), (uint8_t)(sizeof(table) / sizeof((table)[0])), \
      (flags_) }

static const qm_menu_t uiModes[] =
{
    QM_MODE(OPMODE_FM, "FM", qm_items_fm,
            UI_MODE_TX | UI_MODE_CHANNEL | UI_MODE_SELECT),
#ifdef UI_MODE_M17
    QM_MODE(OPMODE_M17, "M17", qm_items_m17,
            UI_MODE_TX | UI_MODE_CHANNEL | UI_MODE_SELECT),
#endif
#ifdef UI_MODE_TETRA
    QM_MODE(OPMODE_TETRA, "TETRA", qm_items_tetra,
            UI_MODE_CHANNEL | UI_MODE_SELECT),
#endif
#ifdef UI_MODE_DMR
    QM_MODE(OPMODE_DMR, "DMR", qm_items_dmr,
            UI_MODE_CHANNEL | UI_MODE_SELECT),
#endif
#ifdef UI_MODE_NXDN
    QM_MODE(OPMODE_NXDN, "NXDN", qm_items_nxdn,
            UI_MODE_CHANNEL | UI_MODE_SELECT | UI_MODE_PAGE),
#endif
#ifdef UI_MODE_AIRBAND
    QM_MODE(OPMODE_AIRBAND, "AM", qm_items_airband,
            UI_MODE_CHANNEL | UI_MODE_SELECT | UI_MODE_PAGE),
#endif
};

#define UI_MODE_COUNT ((uint8_t)(sizeof(uiModes) / sizeof(uiModes[0])))

/* The FM entry: the fallback of every accessor, so it must always be there */
static const ui_mode_t *ui_mode_fm(void)
{
    for (uint8_t i = 0; i < UI_MODE_COUNT; i++) {
        if (uiModes[i].mode == OPMODE_FM)
            return &uiModes[i];
    }
    return &uiModes[0];
}

uint8_t ui_mode_count(void)
{
    return UI_MODE_COUNT;
}

const ui_mode_t *ui_mode_at(uint8_t index)
{
    return (index < UI_MODE_COUNT) ? &uiModes[index] : NULL;
}

const ui_mode_t *ui_mode_find(uint8_t id)
{
    for (uint8_t i = 0; i < UI_MODE_COUNT; i++) {
        if (uiModes[i].mode == id)
            return &uiModes[i];
    }
    return NULL;
}

const ui_mode_t *ui_mode_current(void)
{
    return ui_mode_find(state.channel.mode);
}

const char *ui_mode_label(uint8_t id)
{
    /* The one place a mode is spelled is the channel store */
    return chstore_modeName(id);
}

const char *ui_mode_title(uint8_t id)
{
    const ui_mode_t *m = ui_mode_find(id);

    return (m != NULL) ? m->title : chstore_modeName(id);
}

bool ui_mode_canTx(uint8_t id)
{
    const ui_mode_t *m = ui_mode_find(id);

    return (m != NULL) && ((m->flags & UI_MODE_TX) != 0);
}

bool ui_mode_hasPage(uint8_t id)
{
    const ui_mode_t *m = ui_mode_find(id);

    return (m != NULL) && ((m->flags & UI_MODE_PAGE) != 0);
}

bool ui_mode_inChannelList(uint8_t id)
{
    const ui_mode_t *m = ui_mode_find(id);

    return (m != NULL) && ((m->flags & UI_MODE_CHANNEL) != 0);
}

const qm_item_t *ui_mode_rows(uint8_t id, uint8_t *count)
{
    const ui_mode_t *m = ui_mode_find(id);

    if (m == NULL)
        return NULL;

    if (count != NULL)
        *count = m->count;

    return m->items;
}

uint8_t ui_mode_next(uint8_t id, int dir)
{
    int count = 0;
    int first = -1;
    int cur = -1;

    for (uint8_t i = 0; i < UI_MODE_COUNT; i++) {
        if ((uiModes[i].flags & UI_MODE_SELECT) == 0)
            continue;

        if (first < 0)
            first = i;
        if (uiModes[i].mode == id)
            cur = i;
        count++;
    }

    if (count == 0)
        return id;   /* a build with no selectable mode: leave it alone */

    if (cur < 0)
        return uiModes[first].mode;   /* unknown mode: restart the cycle */

    /* Walk to the next selectable entry, wrapping at both ends */
    int i = cur;
    for (int n = 0; n < (int)UI_MODE_COUNT; n++) {
        int next = (dir < 0) ? (i - 1 + UI_MODE_COUNT) % UI_MODE_COUNT
                             : (i + 1) % UI_MODE_COUNT;

        if (uiModes[next].flags & UI_MODE_SELECT)
            return uiModes[next].mode;

        i = next;
    }

    return uiModes[first].mode;
}

const qm_menu_t *qm_findMenu(uint8_t opmode)
{
    const ui_mode_t *m = ui_mode_find(opmode);

    /* FM for a mode without a table: never leave the popup empty */
    return (m != NULL) ? m : ui_mode_fm();
}

/* ------------------------------------------------------------------------- */
/* State machine                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Geometry. The screen is 160x128, layout.top_h is 16 and the hint bar takes
 * the last 11 px, so the popup has 16..116 for a 14 px header and 12 px rows
 * of the 6 pt font: seven rows fit (16 + 14 + 7*12 = 114) and an eighth would
 * land on the hints.
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

/* Pick the table of the current mode and remember which mode it is for */
static void qm_rebuild(void)
{
    qm_state.mode = state.channel.mode;
    qm_state.qm = qm_findMenu(qm_state.mode);
    qm_state.selected = 0;
    qm_state.first = 0;
}

static void qm_scroll(void)
{
    const qm_menu_t *qm = qm_active();
    uint8_t rows = qm_visibleRows();
    uint16_t sel;
    uint16_t first;

    if (qm == NULL)
        return;

    sel = qm_state.selected;
    first = qm_state.first;
    ui_list_clamp(&sel, &first, qm->count, rows);
    qm_state.selected = (uint8_t)sel;
    qm_state.first = (uint8_t)first;
}

void qm_open(void)
{
    qm_state.open = true;
    qm_state.latched = false;   /* long P2 latches it, see qm_toggleLatch() */
    qm_state.lastKeyTick = getTick();
    qm_rebuild();
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

/*
 * The row engine works on a table and a cursor instead of on the popup's
 * globals, so a Settings page (qm_page_*) walks the same rows with the same
 * limits and the same setters. One implementation of "what does P1 do to this
 * row", used by both screens.
 */
static const qm_item_t *qm_rowOf(const qm_menu_t *qm, uint8_t index)
{
    if ((qm == NULL) || (index >= qm->count))
        return NULL;
    return &qm->items[index];
}

/* Enumerations wrap: the last entry is next to the first one */
static int qm_wrap(const qm_item_t *item, int value, int dir)
{
    int span = item->max - item->min + 1;

    if (span <= 0)
        return value;

    while (value < item->min)
        value += span;
    while (value > item->max)
        value -= span;

    (void)dir;
    return value;
}

/* Change the value of a row: P1/P2 on the popup, and on a settings page */
static void qm_row_adjust(const qm_item_t *item, int delta, bool coarse)
{
    if ((item == NULL) || (delta == 0))
        return;

    /* Read only rows: no value to change, but never silently do nothing */
    if ((item->type == QM_INFO) || (item->type == QM_GOTO)
        || (item->get == NULL) || (item->set == NULL)) {
        vp_beep(400, SHORT_BEEP);
        return;
    }

    int step = item->step;

    /* A long press walks the value faster on the rows that say so */
    if (coarse && item->fast_step > 0) {
        step = item->fast_step;
        /* Do not jump over the limit and stop there: land on it */
        int value = qm_current(item) + delta * step;
        if ((delta > 0) && (value > item->max))
            value = item->max;
        if ((delta < 0) && (value < item->min))
            value = item->min;
        if ((item->type != QM_ENUM) && (item->type != QM_TOGGLE)) {
            item->set(value);
            vp_playMenuBeepIfNeeded(false);
            return;
        }
    }

    int value = qm_current(item) + delta * step;

    switch (item->type) {
        case QM_ENUM:
        case QM_TOGGLE:
            value = qm_wrap(item, value, delta);
            break;

        case QM_INT:
        default:
            /* Numbers stop at their limits: a squelch at 15 stays at 15 */
            qm_clamp(&value, item);
            break;
    }

    item->set(value);
}

/*
 * ENTER on a row. `leave` is what closes the screen the row belongs to before a
 * QM_GOTO row opens another one: qm_close for the popup, qm_page_close for a
 * settings page, so both screens behave the same way without either knowing
 * about the other.
 */
static void qm_row_enter(const qm_item_t *item, void (*leave)(void))
{
    if (item == NULL)
        return;

    switch (item->type) {
        case QM_INT:
        case QM_ENUM:
        case QM_TOGGLE:
            /* Green is the same as P2: cycle forward. It never closes. */
            qm_row_adjust(item, +1, false);
            break;

        case QM_GOTO:
            /*
             * One entry point for every screen an overlay can open: it picks
             * the row (DEST/META used to land on the first field of the page),
             * clears any stale edit_mode and arms or disarms the transmitter
             * the way that screen needs. See ui_open_screen().
             */
            if (leave != NULL)
                leave();
            ui_open_screen(item->goto_screen, item->goto_item);
            break;

        case QM_INFO:
        default:
            /* Read only: no action, no error beep */
            break;
    }
}

void qm_adjust_step(int delta, bool coarse)
{
    qm_row_adjust(qm_selected(), delta, coarse);
}

void qm_adjust(int delta)
{
    qm_adjust_step(delta, false);
}

void qm_enter(void)
{
    qm_row_enter(qm_selected(), qm_close);
}

void qm_cancel(void)
{
    qm_close();
}

bool qm_tick(long long now)
{
    if (!qm_state.open)
        return false;

    /*
     * A mode change from outside (the side key) must follow the table.
     *
     * Compare against the mode the table was chosen for, never against the
     * table's own mode field: a mode with no table of its own falls back to the
     * FM one, and comparing pointers' contents there rebuilt the popup on every
     * pass - the cursor jumped back to the first row each time the user moved
     * it, which is the "the quick menu stops working" report. With the mode
     * recorded in the state, the table is rebuilt exactly once per change.
     */
    if (qm_state.mode != state.channel.mode) {
        qm_rebuild();
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
    return qm_value_text_of(qm_state.qm, index, buf, len);
}

bool qm_value_text_of(const qm_menu_t *qm, uint8_t index, char *buf, int len)
{
    if (buf == NULL || len <= 0)
        return false;
    buf[0] = '\0';

    if ((qm == NULL) || (index >= qm->count))
        return false;

    const qm_item_t *item = &qm->items[index];

    if (item->fmt != NULL) {
        item->fmt(buf, len);
        return true;
    }
    if ((item->type == QM_ENUM || item->type == QM_TOGGLE)
        && item->names != NULL && item->get != NULL) {
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
 * Make a value fit in `avail` pixels of the row font. Long values are cut on
 * the left, where the least information is (a frequency keeps its tail), and
 * marked with '<' so that a shortened value never looks like a real one.
 */
static void qm_fitValue(char *buf, size_t len, uint16_t avail)
{
    if ((buf[0] == '\0') || (avail < 8))
        return;   /* nothing to trim into: leave the row as it is */

    if (gfx_getTextWidth(QM_FONT, buf) <= avail)
        return;

    size_t n = strlen(buf);
    while (n > 0) {
        char tmp[QM_VALUE_BUF];
        memmove(tmp, buf + (strlen(buf) - n), n);
        tmp[n] = '\0';
        if (gfx_getTextWidth(QM_FONT, tmp) <= avail - 6)
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
    int16_t baseline = (int16_t)(QM_TOP_Y + QM_HEADER_BASELINE);

    gfx_print((point_t){ QM_TEXT_X, baseline }, FONT_SIZE_8PT, TEXT_ALIGN_LEFT,
              yellow_fab413, "%s", qm_state.qm->title);

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

    gfx_print((point_t){ CONFIG_SCREEN_WIDTH - QM_VALUE_X, baseline },
              FONT_SIZE_8PT, TEXT_ALIGN_RIGHT, color_white, "%s", right);
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
        /*
         * The rectangle and the baseline come from the same `top`: QM_ROW_H
         * high, text at top + 9. The 6 pt glyph box is top+2..top+8 and its
         * descenders reach top+10, so the black-on-white text sits inside the
         * white rectangle instead of floating five pixels above it (the bug
         * that made the 5 pt popup unreadable).
         */
        int16_t baseline = (int16_t)(top + QM_ROW_BASELINE);
        char value[QM_VALUE_BUF] = "";

        if (selected)
            gfx_drawRect((point_t){ 0, top }, CONFIG_SCREEN_WIDTH, QM_ROW_H,
                         color_white, true);

        color_t fg = selected ? color_black : color_white;
        uint16_t label_w = gfx_getTextWidth(QM_FONT, item->label);
        uint16_t value_x = (uint16_t)(QM_TEXT_X + label_w + 6);
        uint16_t avail = (uint16_t)(CONFIG_SCREEN_WIDTH - QM_VALUE_X - value_x);

        gfx_print((point_t){ QM_TEXT_X, baseline }, QM_FONT,
                  TEXT_ALIGN_LEFT, fg, "%s", item->label);

        if (qm_value_text(index, value, sizeof(value))) {
            qm_fitValue(value, sizeof(value), avail);
            gfx_print((point_t){ CONFIG_SCREEN_WIDTH - QM_VALUE_X, baseline },
                      QM_FONT, TEXT_ALIGN_RIGHT, fg, "%s", value);
        }
    }

    ui_hints_draw(&qm_hints);
}

/* ------------------------------------------------------------------------- */
/* Settings pages                                                             */
/* ------------------------------------------------------------------------- */

/*
 * A mode's page in Settings is its quick menu table drawn as a list. The rows,
 * their limits, their formatters and their setters are therefore the same
 * objects the popup uses: a value cannot disagree between the two screens, and
 * the walk every row of every table that tests/unit/ui_quickmenu.cpp does
 * covers the page as well.
 *
 * The header is the shared list header (14 px, 8 pt title, grey rule), not the
 * popup's band, so the page looks like the other settings pages.
 */
qm_page_t qm_page_state;

static void qm_page_scroll(void)
{
    uint16_t sel;
    uint16_t first;

    if (qm_page_state.qm == NULL)
        return;

    sel = qm_page_state.selected;
    first = qm_page_state.first;
    ui_list_clamp(&sel, &first, qm_page_state.qm->count, qm_visibleRows());
    qm_page_state.selected = (uint8_t)sel;
    qm_page_state.first = (uint8_t)first;
}

void qm_page_open(uint8_t mode)
{
    qm_page_state.qm = qm_findMenu(mode);
    qm_page_state.selected = 0;
    qm_page_state.first = 0;
}

void qm_page_close(void)
{
    qm_page_state.qm = NULL;
    qm_page_state.selected = 0;
    qm_page_state.first = 0;
}

bool qm_page_isOpen(void)
{
    return qm_page_state.qm != NULL;
}

void qm_page_move(int delta)
{
    if ((qm_page_state.qm == NULL) || (qm_page_state.qm->count == 0))
        return;

    if (delta < 0) {
        if (qm_page_state.selected > 0)
            qm_page_state.selected--;
        else
            qm_page_state.selected = (uint8_t)(qm_page_state.qm->count - 1);
    } else {
        if ((uint8_t)(qm_page_state.selected + 1) < qm_page_state.qm->count)
            qm_page_state.selected++;
        else
            qm_page_state.selected = 0;
    }
    vp_playMenuBeepIfNeeded(qm_page_state.selected == 0);
    qm_page_scroll();
}

void qm_page_adjust(int delta, bool coarse)
{
    qm_row_adjust(qm_rowOf(qm_page_state.qm, qm_page_state.selected), delta,
                  coarse);
}

void qm_page_enter(void)
{
    qm_row_enter(qm_rowOf(qm_page_state.qm, qm_page_state.selected),
                 qm_page_close);
}

void qm_page_draw(void)
{
    char right[QM_VALUE_BUF] = "";

    if (qm_page_state.qm == NULL)
        return;

    qm_fmt_frequency(right, sizeof(right));

    gfx_clearScreen();
    ui_list_header(qm_page_state.qm->title, right);

    uint8_t rowH = ui_row_height();
    uint8_t rows = ui_rows_fit(rowH);

    for (uint8_t row = 0; row < rows; row++) {
        uint8_t index = (uint8_t)(qm_page_state.first + row);

        if (index >= qm_page_state.qm->count)
            break;

        const qm_item_t *item = &qm_page_state.qm->items[index];
        bool selected = (index == qm_page_state.selected);
        int16_t top = ui_row_top(row);
        int16_t baseline = ui_row_baseline_at(top, rowH);
        char value[QM_VALUE_BUF] = "";
        uint16_t label_w = gfx_getTextWidth(UI_ROW_FONT, item->label);
        uint16_t value_x = (uint16_t)(UI_TEXT_X + label_w + 6);
        uint16_t avail = (uint16_t)(CONFIG_SCREEN_WIDTH - UI_TEXT_X - value_x);

        if (selected)
            ui_row_selection(top, rowH, true);

        ui_row_text(baseline, UI_ROW_FONT, TEXT_ALIGN_LEFT, selected,
                    "%s", item->label);

        if (qm_value_text_of(qm_page_state.qm, index, value, sizeof(value))) {
            qm_fitValue(value, sizeof(value), avail);
            ui_row_text(baseline, UI_ROW_FONT, TEXT_ALIGN_RIGHT, selected,
                        "%s", value);
        }
    }

    ui_hints_draw_for_screen(SETTINGS_MODE);
}
