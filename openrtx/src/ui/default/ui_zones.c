/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Zone browser. Two screens on top of whatever the UI was showing:
 *
 *   zones    the zone titles R, U, PMR, with how many entries each holds
 *   entries  the repeaters or PMR channels of one zone: label on the left,
 *            frequency on the right, transmit power under 1 W marked
 *
 * The tables come from the data model through ui_zones_source_t (see
 * ui/ui_zones.h); this file never defines a frequency. It draws its own header
 * with the shared band of ui_layout.c and its rows with the shared 12 px row of
 * every other list, so the zone list lines up with the channel list and the
 * menus to the pixel.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "hwconfig.h"
#include "core/graphics.h"
#include "core/state.h"
#include "core/beeps.h"
#include "core/voicePrompts.h"
#include "interfaces/delays.h"
#include "ui/ui_default.h"
#include "ui/ui_hints.h"
#include "ui/ui_layout.h"
#include "ui/ui_zones.h"

enum zScreen
{
    ZS_CLOSED = 0,
    ZS_ZONES,
    ZS_ENTRIES
};

static const ui_zones_source_t *source = NULL;

static struct
{
    enum zScreen screen;
    uint8_t  zone;
    uint16_t zsel;
    uint16_t esel;
    uint16_t etop;
    char     msg[20];
    long long msgUntil;
} zs;

/* Zone titles the model does not name itself */
static const char *const defaultTitles[] = { "R", "U", "PMR" };

/******************************************************************************/
/* Data access                                                                */
/******************************************************************************/

void ui_zones_setSource(const ui_zones_source_t *src)
{
    source = src;
}

const ui_zones_source_t *ui_zones_source(void)
{
    return source;
}

static uint8_t zoneCount(void)
{
    return (source != NULL) ? source->zones : 0;
}

static const char *zoneTitle(uint8_t zone)
{
    const char *t = NULL;

    if ((source != NULL) && (source->name != NULL))
        t = source->name(zone);

    if (t != NULL && t[0] != '\0')
        return t;
    if (zone < sizeof(defaultTitles) / sizeof(defaultTitles[0]))
        return defaultTitles[zone];
    return "ZONE";
}

static uint16_t zoneSize(uint8_t zone)
{
    if ((source == NULL) || (source->size == NULL))
        return 0;
    return source->size(zone);
}

static const ui_zone_entry_t *zoneAt(uint8_t zone, uint16_t index)
{
    if ((source == NULL) || (source->at == NULL))
        return NULL;
    return source->at(zone, index);
}

/******************************************************************************/
/* Text helpers                                                               */
/******************************************************************************/

void ui_zones_label(const char *zone_title, uint16_t index, char *buf,
                    size_t len)
{
    if (buf == NULL || len == 0)
        return;

    const char *t = (zone_title != NULL) ? zone_title : "Z";

    /* The PMR channels are numbered from one, the repeaters from zero: that is
       how the stock channel list and every PMR radio name them. */
    if (strcmp(t, "PMR") == 0)
        snprintf(buf, len, "PMR%u", (unsigned)(index + 1));
    else if (strcmp(t, "R") == 0 || strcmp(t, "U") == 0)
        snprintf(buf, len, "%s%u", t, (unsigned)index);
    else
        snprintf(buf, len, "%.4s%u", t, (unsigned)(index + 1));
}

const char *ui_zones_powerText(uint16_t mw, char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return "";

    if (mw == 0)
        snprintf(buf, len, "-");
    else if (mw < 1000)
        snprintf(buf, len, "%umW", (unsigned)mw);
    else if ((mw % 1000) == 0)
        snprintf(buf, len, "%uW", (unsigned)(mw / 1000));
    else
        snprintf(buf, len, "%u.%uW", (unsigned)(mw / 1000),
                 (unsigned)((mw % 1000) / 100));

    return buf;
}

bool ui_zones_isQrp(uint16_t mw)
{
    return (mw > 0) && (mw < 1000);
}

void ui_zones_fmtFreq(uint32_t hz, char *buf, size_t len)
{
    snprintf(buf, len, "%lu.%05lu", (unsigned long)(hz / 1000000UL),
             (unsigned long)((hz % 1000000UL) / 10UL));
}

uint8_t ui_zones_visibleRows(void)
{
    return ui_rows_fit(ui_row_height());
}

/******************************************************************************/
/* Actions                                                                    */
/******************************************************************************/

static void beepOk(void)
{
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
}

static void beepError(void)
{
    vp_beep(400, SHORT_BEEP * 2);
}

static void setMessage(const char *text)
{
    snprintf(zs.msg, sizeof(zs.msg), "%s", text);
    zs.msgUntil = getTick() + 2000;
}

/* Copy a zone entry into the radio state, as the channel list does. */
static bool tuneEntry(uint16_t index)
{
    const ui_zone_entry_t *e = zoneAt(zs.zone, index);
    char label[8];

    if (e == NULL)
        return false;

    ui_zones_label(zoneTitle(zs.zone), index, label, sizeof(label));

    memset(&state.channel, 0, sizeof(state.channel));
    state.channel.mode = (e->mode != OPMODE_NONE) ? e->mode : OPMODE_FM;
    state.channel.bandwidth = BW_12_5;
    state.channel.power = (e->power_mw > 0) ? e->power_mw : 1000;
    state.channel.rx_frequency = e->rx_hz;
    state.channel.tx_frequency = (e->tx_hz > 0) ? e->tx_hz : e->rx_hz;
    snprintf(state.channel.name, sizeof(state.channel.name), "%s", label);
    state.channel_index = 0;
    return true;
}

/******************************************************************************/
/* Key handling                                                               */
/******************************************************************************/

bool ui_zones_handleEvent(kbd_msg_t *msg, bool *sync_rtx)
{
    uint8_t rows = ui_zones_visibleRows();
    uint16_t count;

    if (zs.screen == ZS_CLOSED)
        return false;

    if (msg->keys & KEY_ESC) {
        if (zs.screen == ZS_ENTRIES) {
            zs.screen = ZS_ZONES;
            zs.msg[0] = '\0';
        } else {
            zs.screen = ZS_CLOSED;
            /* The browser was opened from the main menu: go back there, so
               that the menu is drawn again and the transmitter is re-armed */
            state.ui_screen = MENU_TOP;
        }
        return true;
    }

    if (zs.screen == ZS_ZONES) {
        count = zoneCount();
        if (count == 0) {
            return true;
        }
        if (msg->keys & (KEY_UP | KNOB_LEFT)) {
            zs.zsel = (zs.zsel > 0) ? (uint16_t)(zs.zsel - 1)
                                    : (uint16_t)(count - 1);
        } else if (msg->keys & (KEY_DOWN | KNOB_RIGHT)) {
            zs.zsel = (zs.zsel + 1 < count) ? (uint16_t)(zs.zsel + 1) : 0;
        } else if (msg->keys & KEY_ENTER) {
            zs.zone = (uint8_t)zs.zsel;
            zs.esel = 0;
            zs.etop = 0;
            zs.msg[0] = '\0';
            zs.screen = ZS_ENTRIES;
            beepOk();
        }
        (void)rows;
        return true;
    }

    /* Entries */
    count = zoneSize(zs.zone);
    if (count == 0) {
        if (msg->keys & KEY_ENTER)
            beepError();
        return true;
    }

    if (msg->keys & (KEY_UP | KNOB_LEFT)) {
        zs.esel = (zs.esel > 0) ? (uint16_t)(zs.esel - 1)
                                : (uint16_t)(count - 1);
    } else if (msg->keys & (KEY_DOWN | KNOB_RIGHT)) {
        zs.esel = (zs.esel + 1 < count) ? (uint16_t)(zs.esel + 1) : 0;
    } else if (msg->keys & KEY_ENTER) {
        if (tuneEntry(zs.esel)) {
            beepOk();
            zs.screen = ZS_CLOSED;
            /* The tuned entry lives in the VFO, so show it there */
            state.ui_screen = MAIN_VFO;
            *sync_rtx = true;
        } else {
            setMessage("NO ENTRY");
            beepError();
        }
    } else if (msg->keys & KEY_F1) {
        /* P1: back to the zone list, same as red */
        zs.screen = ZS_ZONES;
    }

    ui_list_clamp(&zs.esel, &zs.etop, count, rows);
    return true;
}

/******************************************************************************/
/* Drawing                                                                    */
/******************************************************************************/

static void drawZoneList(void)
{
    char right[12];
    uint8_t count = zoneCount();
    uint8_t rows = ui_zones_visibleRows();

    snprintf(right, sizeof(right), "%u", (unsigned)count);
    ui_list_header("ZONES", right);

    if (count == 0) {
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2,
                             (int16_t)(ui_list_top() + 24) }, FONT_SIZE_6PT,
                  TEXT_ALIGN_CENTER, color_white, "NO ZONES");
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2,
                             (int16_t)(ui_list_top() + 40) }, FONT_SIZE_5PT,
                  TEXT_ALIGN_CENTER, color_white, "zone tables not loaded");
        ui_hints_draw(&(ui_hints_t){ NULL, NULL, NULL, "BACK" });
        return;
    }

    uint16_t top = 0;
    ui_list_clamp(&zs.zsel, &top, count, rows);

    for (uint8_t row = 0; row < rows; row++) {
        uint16_t idx = (uint16_t)(top + row);
        if (idx >= count)
            break;

        int16_t y = ui_row_top(row);
        bool sel = (idx == zs.zsel);

        if (sel)
            ui_row_selection(y, ui_row_height(), true);

        char name[12];
        snprintf(name, sizeof(name), "%s", zoneTitle((uint8_t)idx));
        ui_row_text(ui_row_baseline_at(y, ui_row_height()), FONT_SIZE_6PT,
                    TEXT_ALIGN_LEFT, sel, "%s", name);

        char countbuf[12];
        snprintf(countbuf, sizeof(countbuf), "%u", (unsigned)zoneSize((uint8_t)idx));
        ui_row_text(ui_row_baseline_at(y, ui_row_height()), FONT_SIZE_5PT,
                    TEXT_ALIGN_RIGHT, sel, "%s", countbuf);
    }

    ui_hints_draw(&(ui_hints_t){ "OPEN", NULL, NULL, "BACK" });
}

static void drawEntryList(void)
{
    char right[16];
    uint16_t count = zoneSize(zs.zone);
    uint8_t rows = ui_zones_visibleRows();
    const char *title = zoneTitle(zs.zone);

    snprintf(right, sizeof(right), "%u/%u", (unsigned)(count ? zs.esel + 1 : 0),
             (unsigned)count);
    ui_list_header(title, right);

    if (count == 0) {
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2,
                             (int16_t)(ui_list_top() + 24) }, FONT_SIZE_6PT,
                  TEXT_ALIGN_CENTER, color_white, "EMPTY");
        ui_hints_draw(&(ui_hints_t){ NULL, NULL, NULL, "BACK" });
        return;
    }

    for (uint8_t row = 0; row < rows; row++) {
        uint16_t idx = (uint16_t)(zs.etop + row);
        if (idx >= count)
            break;

        const ui_zone_entry_t *e = zoneAt(zs.zone, idx);
        if (e == NULL)
            break;

        int16_t y = ui_row_top(row);
        bool sel = (idx == zs.esel);
        int16_t baseline = ui_row_baseline_at(y, ui_row_height());

        if (sel)
            ui_row_selection(y, ui_row_height(), true);

        char label[10];
        if (e->label[0] != '\0')
            snprintf(label, sizeof(label), "%.9s", e->label);
        else
            ui_zones_label(title, idx, label, sizeof(label));

        ui_row_text(baseline, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, sel, "%s", label);

        char freq[16];
        ui_zones_fmtFreq(e->rx_hz, freq, sizeof(freq));

        char power[10];
        ui_zones_powerText(e->power_mw, power, sizeof(power));

        /*
         * Fixed columns, so a QRP tag never shifts the frequency: label at the
         * left margin (6 pt), power tag at 44 px (5 pt), frequency right
         * aligned at the right margin (5 pt). "PMR16 500mW 446.00625" is the
         * widest row and it takes 112 of the 156 usable pixels.
         */
        if (ui_zones_isQrp(e->power_mw))
        {
            gfx_print((point_t){ 44, baseline }, FONT_SIZE_5PT,
                      TEXT_ALIGN_LEFT, sel ? color_black : color_white,
                      "%s", power);
        }

        ui_row_text(baseline, FONT_SIZE_5PT, TEXT_ALIGN_RIGHT, sel, "%s", freq);
    }

    if (zs.msg[0] != '\0' && getTick() < zs.msgUntil)
        ui_hints_draw(&(ui_hints_t){ zs.msg, NULL, NULL, NULL });
    else
        ui_hints_draw(&(ui_hints_t){ "TUNE", "<", NULL, "BACK" });
}

void ui_zones_draw(void)
{
    switch (zs.screen) {
        case ZS_ZONES:   drawZoneList();  break;
        case ZS_ENTRIES: drawEntryList(); break;
        default: break;
    }
}

void ui_zones_open(void)
{
    zs.screen = ZS_ZONES;
    zs.zsel = 0;
    zs.zone = 0;
    zs.esel = 0;
    zs.etop = 0;
    zs.msg[0] = '\0';
}

bool ui_zones_isOpen(void)
{
    return zs.screen != ZS_CLOSED;
}
