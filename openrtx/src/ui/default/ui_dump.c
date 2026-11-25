/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file ui_dump.c
 * @brief The screen and the radio, as text, for the host simulator.
 *
 * See ui/ui_dump.h and docs/SIMULATOR.md. One snapshot, two renderings:
 *
 *  - the *lines* are what the firmware drew, captured at gfx_print() by
 *    core/gfx_capture.c. A line is marked `selected` when it was drawn in
 *    black, which is what every list screen of this UI does with the row under
 *    the cursor (the inverted rectangle): docs/UI.md, "Selection";
 *  - the *state* is read from `state`, from the quick menu's own tables
 *    (qm_state, qm_page_state) and from the module accessors, never from the
 *    screen.
 *
 * The dump is meant to be read while the UI thread is parked between frames:
 * the control channel takes state_mutex and waits for a new frame before it
 * asks (sim_control.c), so the values it prints belong to the screen it
 * describes.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/gfx_capture.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/ui.h"
#include "ui/ui_channels.h"
#include "ui/ui_contacts.h"
#include "ui/ui_default.h"
#include "ui/ui_dump.h"
#include "ui/ui_layout.h"
#include "ui/ui_messages.h"
#include "ui/ui_modes.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_zones.h"

/* --------------------------------------------------------------- helpers -- */

/** snprintf into a buffer that is being appended to. */
static void append(char *buf, size_t len, size_t *pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void append(char *buf, size_t len, size_t *pos, const char *fmt, ...)
{
    if (*pos >= len)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsniprintf(buf + *pos, len - *pos, fmt, ap);
    va_end(ap);

    if (n < 0)
        return;

    *pos += (size_t)n;
    if (*pos >= len)
        *pos = len - 1;
}

/** A string as a JSON literal, quotes and escapes included. */
static void append_json_string(char *buf, size_t len, size_t *pos,
                               const char *s)
{
    append(buf, len, pos, "\"");
    for (size_t i = 0; (s != NULL) && (s[i] != '\0'); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            append(buf, len, pos, "\\%c", c);
        else if (c < 0x20)
            append(buf, len, pos, " ");
        else
            append(buf, len, pos, "%c", c);
    }
    append(buf, len, pos, "\"");
}

/** "145.500000" for 145500000 Hz. */
static void fmt_mhz(char *out, size_t len, freq_t hz)
{
    snprintf(out, len, "%lu.%06lu", (unsigned long)(hz / 1000000UL),
             (unsigned long)(hz % 1000000UL));
}

/** Step of state.step_index, Hz. */
static freq_t step_hz(void)
{
    if (state.step_index < n_freq_steps)
        return freq_steps[state.step_index];

    return 0;
}

/** True when the line was drawn in black: the row under the cursor. */
static bool line_selected(const gfx_capture_line_t *l)
{
    return l->rgb == 0x000000;
}

/* ----------------------------------------------------------------- names -- */

const char *ui_dump_screenName(uint8_t screen)
{
    switch (screen) {
        case MAIN_VFO:            return "MAIN_VFO";
        case MAIN_VFO_INPUT:      return "MAIN_VFO_INPUT";
        case MAIN_MEM:            return "MAIN_MEM";
        case MODE_VFO:            return "MODE_VFO";
        case MODE_MEM:            return "MODE_MEM";
        case MENU_TOP:            return "MENU_TOP";
        case MENU_BANK:           return "MENU_BANK";
        case MENU_CHANNEL:        return "MENU_CHANNEL";
        case MENU_ZONES:          return "MENU_ZONES";
        case MENU_CONTACTS:       return "MENU_CONTACTS";
        case MENU_GPS:            return "MENU_GPS";
        case MENU_SETTINGS:       return "MENU_SETTINGS";
        case MENU_BACKUP_RESTORE: return "MENU_BACKUP_RESTORE";
        case MENU_BACKUP:         return "MENU_BACKUP";
        case MENU_RESTORE:        return "MENU_RESTORE";
        case MENU_INFO:           return "MENU_INFO";
        case MENU_ABOUT:          return "MENU_ABOUT";
        case SETTINGS_TIMEDATE:   return "SETTINGS_TIMEDATE";
        case SETTINGS_TIMEDATE_SET: return "SETTINGS_TIMEDATE_SET";
        case SETTINGS_DISPLAY:    return "SETTINGS_DISPLAY";
        case SETTINGS_GPS:        return "SETTINGS_GPS";
        case SETTINGS_RADIO:      return "SETTINGS_RADIO";
        case SETTINGS_AUDIO:      return "SETTINGS_AUDIO";
        case SETTINGS_M17:        return "SETTINGS_M17";
#ifdef CONFIG_TETRA_AFC
        case SETTINGS_TETRA:      return "SETTINGS_TETRA";
#endif
        case SETTINGS_FM:         return "SETTINGS_FM";
        case SETTINGS_MODE:       return "SETTINGS_MODE";
        case SETTINGS_ACCESSIBILITY: return "SETTINGS_ACCESSIBILITY";
        case SETTINGS_RESET2DEFAULTS: return "SETTINGS_RESET2DEFAULTS";
#ifdef CONFIG_APRS
        case MENU_APRS:           return "MENU_APRS";
        case APRS_DETAIL:         return "APRS_DETAIL";
        case APRS_MSGS:           return "APRS_MSGS";
        case APRS_MSG_VIEW:       return "APRS_MSG_VIEW";
        case APRS_MSG_EDIT:       return "APRS_MSG_EDIT";
        case APRS_CFG:            return "APRS_CFG";
#endif
#ifdef CONFIG_KISS
        case PKT_CFG:             return "PKT_CFG";
        case PKT_STATUS:          return "PKT_STATUS";
#endif
        case MENU_MESSAGES:       return "MENU_MESSAGES";
        case MSG_VIEW:            return "MSG_VIEW";
        case MSG_SENT:            return "MSG_SENT";
        case MSG_COMPOSE:         return "MSG_COMPOSE";
        case MSG_STATUS:          return "MSG_STATUS";
        case LOW_BAT:             return "LOW_BAT";
        case SPECTRUM:            return "SPECTRUM";
        default:                  return "UNKNOWN";
    }
}

/* ------------------------------------------------------------- quick menu -- */

/** Rows of the table a dump should describe: the popup first, then the page. */
static const qm_menu_t *activeTable(uint8_t *selected)
{
    if (qm_state.open && (qm_state.qm != NULL)) {
        *selected = qm_state.selected;
        return qm_state.qm;
    }

    if (qm_page_isOpen() && (qm_page_state.qm != NULL)) {
        *selected = qm_page_state.selected;
        return qm_page_state.qm;
    }

    return NULL;
}

/* ------------------------------------------------------------ text format -- */

size_t ui_dump_text(char *buf, size_t len)
{
    gfx_capture_frame_t f;
    char hz[24];
    size_t pos = 0;

    if (len == 0)
        return 0;

    buf[0] = '\0';

    bool have = gfx_capture_last(&f);

    append(buf, len, &pos, "# screen %s (%u)  claim %u x%u  edit %s  lock %s\n",
           ui_dump_screenName(state.ui_screen), state.ui_screen,
           ui_lastClaim(), ui_lastClaimCount(), ui_edit_mode() ? "yes" : "no",
           ui_getInputLock() ? "yes" : "no");

    if (have && f.header)
        append(buf, len, &pos, "header: \"%s\"  right: \"%s\"\n", f.title,
               f.right);

    if (have && f.selection)
        append(buf, len, &pos, "selection: top %d h %u  %s\n", f.sel_top,
               f.sel_h, f.sel_filled ? "filled" : "hollow (editing)");

    append(buf, len, &pos, "lines:\n");
    if (have) {
        for (uint16_t i = 0; i < f.lines; i++) {
            const gfx_capture_line_t *l = &f.line[i];
            append(buf, len, &pos, "  y%3d x%3d f%u %c %s%s\n", l->y, l->x,
                   l->font, (l->align == TEXT_ALIGN_LEFT)   ? 'L'
                            : (l->align == TEXT_ALIGN_CENTER) ? 'C'
                                                              : 'R',
                   line_selected(l) ? "* " : "  ", l->text);
        }

        if (f.dropped > 0)
            append(buf, len, &pos, "  (%lu lines not recorded)\n",
                   (unsigned long)f.dropped);
    }

    uint8_t sel = 0;
    const qm_menu_t *qm = activeTable(&sel);

    if (qm != NULL) {
        append(buf, len, &pos, "%s \"%s\" (%u rows), cursor on %u:\n",
               qm_state.open ? "quickmenu" : "page", qm->title, qm->count, sel);

        for (uint8_t i = 0; i < qm->count; i++) {
            char value[QM_VALUE_BUF] = "";
            qm_value_text_of(qm, i, value, sizeof(value));
            append(buf, len, &pos, "  %s %-10s %s\n", (i == sel) ? ">" : " ",
                   qm->items[i].label, value);
        }
    }

    fmt_mhz(hz, sizeof(hz), state.channel.rx_frequency);
    append(buf, len, &pos, "radio: %s rx %s MHz", ui_mode_label(state.channel.mode),
           hz);
    fmt_mhz(hz, sizeof(hz), state_txFrequency());
    append(buf, len, &pos, "  tx %s MHz  ch#%u \"%s\"  bw %u  pwr %lumW\n", hz,
           state.channel_index, state.channel.name, state.channel.bandwidth,
           (unsigned long)state_txPowerMw());

    fmt_mhz(hz, sizeof(hz), step_hz());
    append(buf, len, &pos,
           "volume %u  rssi %.1f  vbat %.1fV  ptt %s  txDisable %s  step %s "
           "MHz\n",
           state.volume, (double)state.rssi, (double)state.v_bat / 1000.0,
           (state.rtxStatus == RTX_TX) ? "down" : "up",
           state.txDisable ? "yes" : "no", hz);

    append(buf, len, &pos,
           "settings: bright %u  sql %u  tx_power %umW  rx_fine %dHz  "
           "keylight %s  brightness_timer %u\n",
           state.settings.brightness, state.settings.sqlLevel,
           (unsigned)state.settings.tx_power, (int)state.settings.rxFineHz,
#ifdef CONFIG_KEY_BACKLIGHT
           state.settings.keyBacklight ? "on" : "off",
#else
           "n/a",
#endif
           state.settings.display_timer);

    append(buf, len, &pos, "modules: channels %s  contacts %s (%d)  zones %s  "
                           "msgs %s\n",
           ui_channels_isOpen() ? "open" : "closed",
           ui_contacts_isOpen() ? "open" : "closed", cstore_count(),
           ui_zones_isOpen() ? "open" : "closed",
           ui_msgs_isScreen(state.ui_screen) ? "open" : "closed");

    append(buf, len, &pos, "bank: %d channels, last used %d\n", chstore_count(),
           chstore_lastUsed());

    return pos;
}

/* ------------------------------------------------------------ JSON format -- */

size_t ui_dump_json(char *buf, size_t len)
{
    gfx_capture_frame_t f;
    char hz[24];
    size_t pos = 0;

    if (len == 0)
        return 0;

    buf[0] = '\0';

    bool have = gfx_capture_last(&f);

    append(buf, len, &pos, "{");
    append(buf, len, &pos, "\"screen\":{\"id\":%u,\"name\":", state.ui_screen);
    append_json_string(buf, len, &pos, ui_dump_screenName(state.ui_screen));
    append(buf, len, &pos,
           "},\"input\":{\"claim\":%u,\"claims\":%u,\"edit\":%s,\"locked\":%s,"
           "\"frames\":%lu,\"menu\":%u,\"last_key\":{\"keys\":%lu,"
           "\"long\":%s}},\"header\":{\"present\":%s,\"title\":",
           ui_lastClaim(), ui_lastClaimCount(),
           ui_edit_mode() ? "true" : "false",
           ui_getInputLock() ? "true" : "false", (unsigned long)ui_frames(),
           ui_menu_selected(), (unsigned long)(ui_lastKeyEvent() >> 1),
           (ui_lastKeyEvent() & 1) ? "true" : "false",
           (have && f.header) ? "true" : "false");
    append_json_string(buf, len, &pos, (have && f.header) ? f.title : "");
    append(buf, len, &pos, ",\"right\":");
    append_json_string(buf, len, &pos, (have && f.header) ? f.right : "");

    append(buf, len, &pos,
           "},\"selection\":{\"present\":%s,\"top\":%d,\"height\":%u,"
           "\"filled\":%s},\"lines\":[",
           (have && f.selection) ? "true" : "false",
           (have && f.selection) ? f.sel_top : -1,
           (have && f.selection) ? f.sel_h : 0,
           (have && f.selection && f.sel_filled) ? "true" : "false");

    if (have) {
        for (uint16_t i = 0; i < f.lines; i++) {
            const gfx_capture_line_t *l = &f.line[i];
            append(buf, len, &pos,
                   "%s{\"y\":%d,\"x\":%d,\"font\":%u,\"align\":%u,"
                   "\"rgb\":\"%06lx\",\"selected\":%s,\"text\":",
                   (i == 0) ? "" : ",", l->y, l->x, l->font, l->align,
                   (unsigned long)l->rgb,
                   line_selected(l) ? "true" : "false");
            append_json_string(buf, len, &pos, l->text);
            append(buf, len, &pos, "}");
        }
    }

    append(buf, len, &pos, "]");

    uint8_t sel = 0;
    const qm_menu_t *qm = activeTable(&sel);

    append(buf, len, &pos, ",\"quickmenu\":");
    if (qm != NULL) {
        append(buf, len, &pos, "{\"kind\":\"%s\",\"title\":",
               qm_state.open ? "popup" : "page");
        append_json_string(buf, len, &pos, qm->title);
        append(buf, len, &pos, ",\"selected\":%u,\"rows\":[", sel);

        for (uint8_t i = 0; i < qm->count; i++) {
            char value[QM_VALUE_BUF] = "";
            qm_value_text_of(qm, i, value, sizeof(value));

            append(buf, len, &pos, "%s{\"label\":", (i == 0) ? "" : ",");
            append_json_string(buf, len, &pos, qm->items[i].label);
            append(buf, len, &pos, ",\"value\":");
            append_json_string(buf, len, &pos, value);
            append(buf, len, &pos, ",\"selected\":%s}",
                   (i == sel) ? "true" : "false");
        }

        append(buf, len, &pos, "]}");
    } else {
        append(buf, len, &pos, "null");
    }

    fmt_mhz(hz, sizeof(hz), state.channel.rx_frequency);
    append(buf, len, &pos,
           ",\"radio\":{\"mode\":\"%s\",\"mode_id\":%u,\"rx\":%lu,"
           "\"rx_text\":\"%s MHz\",\"tx\":%lu,\"channel_index\":%u,"
           "\"name\":",
           ui_mode_label(state.channel.mode), state.channel.mode,
           (unsigned long)state.channel.rx_frequency, hz,
           (unsigned long)state_txFrequency(), state.channel_index);
    append_json_string(buf, len, &pos, state.channel.name);
    append(buf, len, &pos,
           ",\"bandwidth\":%u,\"power\":%lu,\"squelch\":%u,\"volume\":%u,"
           "\"rssi\":%.1f,\"vbat\":%.2f,\"step_hz\":%lu,\"bank_count\":%d,"
           "\"bank_last\":%d,\"contact_count\":%d,\"tx_disable\":%s,"
           "\"ptt\":%s}",
           state.channel.bandwidth, (unsigned long)state_txPowerMw(),
           state.settings.sqlLevel, state.volume, (double)state.rssi,
           (double)state.v_bat / 1000.0, (unsigned long)step_hz(),
           chstore_count(), chstore_lastUsed(), cstore_count(),
           state.txDisable ? "true" : "false",
           (state.rtxStatus == RTX_TX) ? "true" : "false");

    append(buf, len, &pos,
           ",\"settings\":{\"brightness\":%u,\"sql\":%u,\"tx_power\":%u,"
           "\"rx_fine\":%d,\"key_backlight\":%s,\"m17_can\":%u,"
           "\"display_timer\":%u,\"vol_prompt\":%u}",
           state.settings.brightness, state.settings.sqlLevel,
           (unsigned)state.settings.tx_power, (int)state.settings.rxFineHz,
#ifdef CONFIG_KEY_BACKLIGHT
           state.settings.keyBacklight ? "true" : "false",
#else
           "null",
#endif
           state.settings.m17_can, state.settings.display_timer,
           state.settings.vpLevel);

#ifdef CONFIG_APRS
    append(buf, len, &pos,
           ",\"aprs\":{\"decode\":%s,\"beacon\":%s,\"ssid\":%u}",
           state.settings.aprs_decode ? "true" : "false",
           state.settings.aprs_beaconEn ? "true" : "false",
           state.settings.aprs_ssid);
#endif

#ifdef CONFIG_KISS
    append(buf, len, &pos,
           ",\"packet\":{\"enabled\":%s,\"modem\":%u}",
           state.settings.packet_enable ? "true" : "false",
           state.settings.packet_modem);
#endif

    append(buf, len, &pos,
           ",\"modules\":{\"channels\":%s,\"contacts\":%s,\"zones\":%s,"
           "\"messages\":%s}}",
           ui_channels_isOpen() ? "true" : "false",
           ui_contacts_isOpen() ? "true" : "false",
           ui_zones_isOpen() ? "true" : "false",
           ui_msgs_isScreen(state.ui_screen) ? "true" : "false");

    return pos;
}
