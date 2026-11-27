/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/ui_aprs.h"
#include "ui/ui_layout.h"
#include "ui/ui_hints.h"
#include "ui/ui_default.h"
#include "core/aprs_service.h"
#include "core/state.h"
#include "core/beeps.h"
#include "core/voicePrompts.h"
#include "core/graphics.h"
#include "interfaces/delays.h"
#include "interfaces/nvmem.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(CONFIG_SCREEN_WIDTH)
#define SCR_W CONFIG_SCREEN_WIDTH
#else
#define SCR_W 160
#endif
#if defined(CONFIG_SCREEN_HEIGHT)
#define SCR_H CONFIG_SCREEN_HEIGHT
#else
#define SCR_H 128
#endif

#define ROW_H     12
#define TOP_Y      2
/* Rows of every APRS list: the shared 12 px row, 8 of them (docs/UI.md) */
#define LIST_ROWS  (ui_rows_fit(ui_row_height()))

/* Screen-local state: which station or message is selected, and the editor */
static struct
{
    uint8_t station;      /**< index into the station list              */
    uint8_t message;      /**< index into the inbox                     */
    uint8_t field;        /**< configuration page: selected field       */
    uint8_t cfgReturn;    /**< screen the configuration page returns to */

    char    editTo[APRS_CALL_MAX];
    char    editText[APRS_MSG_TEXT_MAX + 1];
    uint8_t editPos;
    uint8_t editChar;

    /*
     * One line of feedback for an action whose result is not a screen: asking
     * for a beacon transmits nothing for a moment, so without this the key that
     * the bar documents as BEACON does nothing the operator can see. Drawn for
     * a couple of seconds over the top of the list, like the channel list does
     * with its own message (ui_channels.c).
     */
    char    note[24];
    uint32_t noteUntil;
} uia;

uint8_t ui_aprs_selectedStation(void)
{
    return uia.station;
}

uint8_t ui_aprs_selectedMessage(void)
{
    return uia.message;
}

uint8_t ui_aprs_editLength(void)
{
    return (uint8_t)strlen(uia.editText);
}

bool ui_aprs_isScreen(uint8_t screen)
{
    return (screen == MENU_APRS) || (screen == APRS_DETAIL)
        || (screen == APRS_MSGS) || (screen == APRS_MSG_VIEW)
        || (screen == APRS_MSG_EDIT) || (screen == APRS_CFG);
}

void ui_aprs_enter(void)
{
    uia.station = 0;
    uia.message = 0;
    uia.field   = 0;
    uia.cfgReturn = MENU_APRS;
    state.ui_screen = MENU_APRS;
}

/*
 * The configuration page as a quick menu entry: it opens over the main screen
 * and red returns there, not to the station list the operator never saw.
 */
void ui_aprs_config(void)
{
    uia.field = 0;
    uia.cfgReturn = (state.ui_screen == MENU_APRS) ? MENU_APRS : MAIN_VFO;
    state.ui_screen = APRS_CFG;
}

/* ------------------------------------------------------------- drawing --- */

static void header(const char *title, const char *right)
{
    ui_list_header(title, right);
}

static void hints(const char *green, const char *p1, const char *p2,
                  const char *red)
{
    /* The same bar as every other screen: ui_hints_draw() centres the group
       and draws the key names itself. */
    ui_hints_draw(&(ui_hints_t){ green, p1, p2, red });
}

/* Age of a packet in the "12s / 5m / 3h" form the list uses. */
static void ageText(char *out, size_t cap, uint32_t lastMs)
{
    if (lastMs == 0)
    {
        snprintf(out, cap, "  -");
        return;
    }

    uint32_t age = ((uint32_t)getTick() - lastMs) / 1000u;
    if (age < 60u)
        snprintf(out, cap, "%2us", (unsigned)age);
    else if (age < 3600u)
        snprintf(out, cap, "%2um", (unsigned)(age / 60u));
    else
        snprintf(out, cap, "%2uh", (unsigned)(age / 3600u));
}

static void drawStationList(void)
{
    const aprs_station_list_t *list = aprs_stations();

    char right[16];
    snprintf(right, sizeof(right), "RX %u", (unsigned)aprs_rxCount());
    header("APRS", right);

    if (list->count == 0)
    {
        gfx_print((point_t){SCR_W / 2, 46}, FONT_SIZE_8PT, TEXT_ALIGN_CENTER,
                  color_white, "Waiting for");
        gfx_print((point_t){SCR_W / 2, 60}, FONT_SIZE_8PT, TEXT_ALIGN_CENTER,
                  color_white, "packets");
        hints("MENU", "MSG", "SETUP", "EXIT");
        return;
    }

    if (uia.station >= list->count)
        uia.station = (uint8_t)(list->count - 1u);

    /* Scroll so that the selection is always visible */
    uint16_t sel = uia.station;
    uint16_t first = 0;

    ui_list_clamp(&sel, &first, list->count, (uint8_t)LIST_ROWS);

    for (uint8_t i = 0; i < LIST_ROWS; i++)
    {
        uint8_t idx = (uint8_t)(first + i);
        if (idx >= list->count)
            break;

        const aprs_station_t *s = &list->st[idx];
        const bool sel = (idx == uia.station);
        const color_t col = sel ? color_black : color_white;
        int16_t ry = ui_row_top(i);
        int16_t baseline = (int16_t)(ry + ui_row_baseline());

        if (sel)
            ui_row_selection(ry, ui_row_height(), true);

        char age[8];
        ageText(age, sizeof(age), s->lastMs);

        char dist[10] = "    -";
        if (s->hasPos && (s->distM > 0))
        {
            if (s->distM >= 10000)
                snprintf(dist, sizeof(dist), "%3uk", (unsigned)(s->distM / 1000));
            else if (s->distM >= 1000)
                snprintf(dist, sizeof(dist), "%u.%uk", (unsigned)(s->distM / 1000),
                         (unsigned)((s->distM % 1000) / 100));
            else
                snprintf(dist, sizeof(dist), "%4um", (unsigned)s->distM);
        }

        gfx_print((point_t){ UI_TEXT_X, baseline }, FONT_SIZE_6PT,
                  TEXT_ALIGN_LEFT, col, "%-9s %s %s", s->call, age, dist);
    }

    hints("MENU", "MSG", "SETUP", "EXIT");
}

static void drawStationDetail(void)
{
    const aprs_station_list_t *list = aprs_stations();
    if (uia.station >= list->count)
    {
        state.ui_screen = MENU_APRS;
        return;
    }

    const aprs_station_t *s = &list->st[uia.station];
    header(s->call, NULL);

    int16_t y = UI_LIST_TOP + 2;

    if (s->hasPos)
    {
        aprs_pos_t p;
        memset(&p, 0, sizeof(p));
        p.lat = s->lat;
        p.lon = s->lon;
        char pos[32];
        aprs_format_position(pos, sizeof(pos), &p);
        gfx_print((point_t){2, y}, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white,
                  "%s", pos);
        y += ROW_H;
        gfx_print((point_t){2, y}, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white,
                  "%u.%ukm  %03u deg", (unsigned)(s->distM / 1000),
                  (unsigned)((s->distM % 1000) / 100), (unsigned)s->bearing);
        y += ROW_H;
        gfx_print((point_t){2, y}, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white,
                  "sym %c%c", s->table, s->symbol);
        y += ROW_H;
    }
    else
    {
        gfx_print((point_t){2, y}, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white,
                  "no position");
        y += ROW_H;
    }

    char age[8];
    ageText(age, sizeof(age), s->lastMs);
    gfx_print((point_t){2, y}, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white,
              "%u pkts, last %s ago", (unsigned)s->packets, age);
    y += ROW_H;

    gfx_print((point_t){2, y}, FONT_SIZE_5PT, TEXT_ALIGN_LEFT, color_white,
              "%.20s", s->comment);
    gfx_print((point_t){2, (int16_t)(y + 10)}, FONT_SIZE_5PT, TEXT_ALIGN_LEFT,
              color_white, "%.20s", s->comment[20] ? &s->comment[20] : "");

    hints("MSG", NULL, "NEXT", "BACK");
}

static void drawMessageList(void)
{
    aprs_mbox_t *m = aprs_mailbox();

    char right[16];
    snprintf(right, sizeof(right), "%u new", (unsigned)aprs_mbox_unread(m));
    header("Messages", right);

    uint8_t shown = 0;
    uint8_t idx = 0;
    for (uint8_t i = 0; (i < APRS_INBOX_MAX) && (shown < LIST_ROWS); i++)
    {
        if (!m->in[i].inUse)
            continue;

        idx = i;
        const aprs_in_msg_t *e = &m->in[i];
        const bool sel = (uia.message == i);
        int16_t ry = ui_row_top(shown);
        int16_t baseline = (int16_t)(ry + ui_row_baseline());

        if (sel)
            ui_row_selection(ry, ui_row_height(), true);

        gfx_print((point_t){ UI_TEXT_X, baseline }, FONT_SIZE_6PT,
                  TEXT_ALIGN_LEFT, sel ? color_black : color_white,
                  "%-9s %.11s", e->from, e->text);
        shown++;
    }

    if (shown == 0)
        gfx_print((point_t){SCR_W / 2, 46}, FONT_SIZE_8PT, TEXT_ALIGN_CENTER,
                  color_white, "No messages");

    /* The beacon request is queued for the transmitter: say so */
    if ((uia.note[0] != '\0') && ((int32_t)(uia.noteUntil - (uint32_t)getTick()) > 0))
        gfx_print((point_t){SCR_W / 2, (int16_t)(SCR_H - 24)}, FONT_SIZE_5PT,
                  TEXT_ALIGN_CENTER, yellow_fab413, "%s", uia.note);

    (void)idx;
    hints("OPEN", "REPLY", "BEACON", "EXIT");
}

static void drawMessageView(void)
{
    aprs_mbox_t *m = aprs_mailbox();
    if (!m->in[uia.message].inUse)
    {
        state.ui_screen = APRS_MSGS;
        return;
    }

    const aprs_in_msg_t *e = &m->in[uia.message];
    header(e->from, e->msgNo[0] ? e->msgNo : NULL);

    /* Six rows of 11 characters: the 67 character limit of APRS fits */
    for (uint8_t r = 0; r < 6; r++)
    {
        char line[12];
        size_t off = (size_t)r * 11u;
        if (off >= strlen(e->text))
            break;
        snprintf(line, sizeof(line), "%.11s", &e->text[off]);
        gfx_print((point_t){2, (int16_t)(UI_LIST_TOP + 2 + r * 11)},
                  FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white, "%s", line);
    }

    gfx_print((point_t){2, SCR_H - 24}, FONT_SIZE_5PT, TEXT_ALIGN_LEFT,
              color_white, "%s", e->acked ? "acked" : "no ack sent");

    /* P1 is the next message, bound to KEY_F1 in the dispatch below */
    hints("REPLY", "NEXT", NULL, "BACK");
}

static const char EDIT_CHARS[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,-/:?@+=!";

/*
 * Comment and Path are strings, and this page has no text editor for them: the
 * two rows were listed next to the numeric ones and pressing P1/P2 on them did
 * nothing at all, which is a dead row. They are read only until a field editor
 * for strings lands, and the row says so ("(fixed)"): the values are the ones
 * the beacon uses, but this screen cannot change them.
 */
static bool configReadOnly(uint8_t field)
{
    return (field == 5u) || (field == 6u);
}

static void drawMessageEdit(void)
{
    char right[16];
    snprintf(right, sizeof(right), "to %s", uia.editTo);
    header("New message", right);

    /* Two lines of 22 characters */
    for (uint8_t r = 0; r < 2; r++)
    {
        char line[24];
        size_t off = (size_t)r * 22u;
        snprintf(line, sizeof(line), "%.22s", &uia.editText[off]);
        gfx_print((point_t){ UI_TEXT_X, (int16_t)(UI_LIST_TOP + 8 + r * 14)},
                  FONT_SIZE_8PT, TEXT_ALIGN_LEFT, color_white, "%s", line);
    }

    /* Caret: the character being chosen, under the text */
    const size_t n = strlen(EDIT_CHARS);
    gfx_print((point_t){ UI_TEXT_X, 70}, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, yellow_fab413,
              "char '%c'  pos %u/%u",
              EDIT_CHARS[uia.editChar % n], (unsigned)uia.editPos,
              (unsigned)strlen(uia.editText));

    gfx_print((point_t){ UI_TEXT_X, 84}, FONT_SIZE_5PT, TEXT_ALIGN_LEFT, color_white,
              "up/down char, P1 adds, P2 deletes");

    hints("SEND", "ADD", "DEL", "EXIT");
}

static void drawConfig(void)
{
    header("APRS setup", state.settings.aprs_decode ? "dec on" : "dec off");

    static const char *names[] = {
        "Decode", "Beacon", "Interval", "Latitude", "Longitude", "Comment",
        "Path", "SSID", "Ack msgs"
    };

    char values[9][24];
    snprintf(values[0], 24, "%s", state.settings.aprs_decode ? "on" : "off");
    snprintf(values[1], 24, "%s", state.settings.aprs_beaconEn ? "on" : "off");
    snprintf(values[2], 24, "%u s", (unsigned)state.settings.aprs_interval_s);
    snprintf(values[3], 24, "%ld.%04ld", (long)(state.settings.aprs_lat_e4 / 10000),
             labs((long)state.settings.aprs_lat_e4 % 10000));
    snprintf(values[4], 24, "%ld.%04ld", (long)(state.settings.aprs_lon_e4 / 10000),
             labs((long)state.settings.aprs_lon_e4 % 10000));
    snprintf(values[5], 24, "%.12s", state.settings.aprs_comment);
    snprintf(values[6], 24, "%.14s", state.settings.aprs_path);
    snprintf(values[7], 24, "%u", (unsigned)state.settings.aprs_ssid);
    snprintf(values[8], 24, "%s", state.settings.aprs_msgAck ? "on" : "off");

    for (uint8_t i = 0; i < 9; i++)
    {
        const bool sel = (i == uia.field);
        int16_t ry = (int16_t)(UI_LIST_TOP + i * ui_row_height());
        int16_t baseline = (int16_t)(ry + ui_row_baseline());

        if (sel)
            ui_row_selection(ry, ui_row_height(), false);

        gfx_print((point_t){ UI_TEXT_X, baseline }, FONT_SIZE_5PT,
                  TEXT_ALIGN_LEFT, sel ? yellow_fab413 : color_white,
                  "%-9s %s%s", names[i], values[i],
                  configReadOnly(i) ? " (fixed)" : "");
    }

    /*
     * The bar says what the dispatch does (ui_aprs_fsm, APRS_CFG): green writes
     * the settings to the flash, P1/P2 change the value of the selected row and
     * UP/DOWN walk the rows. It used to read "NEXT / +/- / SAVE" and bind
     * nothing that matched.
     */
    if ((uia.note[0] != '\0')
        && ((int32_t)(uia.noteUntil - (uint32_t)getTick()) > 0))
        gfx_print((point_t){ SCR_W / 2, (int16_t)(SCR_H - 24) }, FONT_SIZE_5PT,
                  TEXT_ALIGN_CENTER, yellow_fab413, "%s", uia.note);

    hints("SAVE", "-", "+", "EXIT");
}

void ui_aprs_draw(uint8_t screen)
{
    switch (screen)
    {
        case MENU_APRS:     drawStationList();  break;
        case APRS_DETAIL:   drawStationDetail(); break;
        case APRS_MSGS:     drawMessageList();  break;
        case APRS_MSG_VIEW: drawMessageView();  break;
        case APRS_MSG_EDIT: drawMessageEdit();  break;
        case APRS_CFG:      drawConfig();       break;
        default: break;
    }
}

/* ----------------------------------------------------------------- FSM --- */

static void startReply(const char *to)
{
    memset(uia.editText, 0, sizeof(uia.editText));
    uia.editPos  = 0;
    uia.editChar = 0;
    snprintf(uia.editTo, sizeof(uia.editTo), "%s", to);
    state.ui_screen = APRS_MSG_EDIT;
}

static void editCycle(int dir)
{
    const int n = (int)(sizeof(EDIT_CHARS) - 1);
    int c = (int)uia.editChar + dir;
    if (c < 0) c = n - 1;
    if (c >= n) c = 0;
    uia.editChar = (uint8_t)c;
}

static void editCommit(void)
{
    const size_t len = strlen(uia.editText);
    if (len >= APRS_MSG_TEXT_MAX)
        return;

    uia.editText[len]     = EDIT_CHARS[uia.editChar % (sizeof(EDIT_CHARS) - 1)];
    uia.editText[len + 1] = '\0';
    uia.editPos           = (uint8_t)(len + 1);
}

static bool configAdjust(int dir)
{
    if (configReadOnly(uia.field))
    {
        /* Never pretend: the two string rows are not adjustable here */
        vp_beep(400, SHORT_BEEP);
        return false;
    }

    switch (uia.field)
    {
        case 0:
            state.settings.aprs_decode = !state.settings.aprs_decode;
            if (!state.settings.aprs_decode)
                aprs_rxEnd();
            break;
        case 1: state.settings.aprs_beaconEn = !state.settings.aprs_beaconEn; break;
        case 2:
        {
            int32_t v = (int32_t)state.settings.aprs_interval_s + dir * 30;
            if (v < 30) v = 30;
            if (v > 3600) v = 3600;
            state.settings.aprs_interval_s = (uint16_t)v;
            break;
        }
        case 3:
            state.settings.aprs_lat_e4 += dir * 100;    /* 0.01 degree */
            if (state.settings.aprs_lat_e4 > 900000) state.settings.aprs_lat_e4 = 900000;
            if (state.settings.aprs_lat_e4 < -900000) state.settings.aprs_lat_e4 = -900000;
            break;
        case 4:
            state.settings.aprs_lon_e4 += dir * 100;
            if (state.settings.aprs_lon_e4 > 1800000) state.settings.aprs_lon_e4 = 1800000;
            if (state.settings.aprs_lon_e4 < -1800000) state.settings.aprs_lon_e4 = -1800000;
            break;
        case 7:
        {
            int32_t v = (int32_t)state.settings.aprs_ssid + dir;
            if (v < 0) v = 15;
            if (v > 15) v = 0;
            state.settings.aprs_ssid = (uint8_t)v;
            break;
        }
        case 8: state.settings.aprs_msgAck = !state.settings.aprs_msgAck; break;
        default: break;
    }
    return true;
}

bool ui_aprs_fsm(kbd_msg_t msg, bool *sync_rtx)
{
    (void)sync_rtx;
    const uint8_t screen = state.ui_screen;

    switch (screen)
    {
        case MENU_APRS:
        {
            const aprs_station_list_t *list = aprs_stations();

            if (msg.keys & KEY_UP)
            {
                if (uia.station > 0) uia.station--;
                return true;
            }
            if (msg.keys & KEY_DOWN)
            {
                if ((uint8_t)(uia.station + 1u) < list->count) uia.station++;
                return true;
            }
            if (msg.keys & KEY_ENTER)
            {
                if (list->count > 0)
                    state.ui_screen = APRS_DETAIL;
                return true;
            }
            if (msg.keys & KEY_F1)             /* P1: the inbox */
            {
                uia.message = 0;
                state.ui_screen = APRS_MSGS;
                return true;
            }
            if (msg.keys & KEY_F2)             /* P2: the setup screen */
            {
                uia.field = 0;
                uia.cfgReturn = MENU_APRS;
                state.ui_screen = APRS_CFG;
                return true;
            }
            if (msg.keys & KEY_HASH)           /* #: clear the station list */
            {
                aprs_clearStations();
                uia.station = 0;
                return true;
            }
            if (msg.keys & KEY_ESC)
            {
                state.ui_screen = MAIN_VFO;
                return true;
            }
            return true;
        }

        case APRS_DETAIL:
        {
            const aprs_station_list_t *list = aprs_stations();
            if ((msg.keys & KEY_ENTER) && (uia.station < list->count))
            {
                startReply(list->st[uia.station].call);
                return true;
            }
            if (msg.keys & KEY_F2)
            {
                uia.station = (uint8_t)((uia.station + 1u) % (list->count ? list->count : 1));
                return true;
            }
            if (msg.keys & KEY_ESC)
            {
                state.ui_screen = MENU_APRS;
                return true;
            }
            return true;
        }

        case APRS_MSGS:
        {
            aprs_mbox_t *m = aprs_mailbox();

            if (msg.keys & KEY_UP)
            {
                for (int i = (int)uia.message - 1; i >= 0; i--)
                {
                    if (m->in[i].inUse) { uia.message = (uint8_t)i; break; }
                }
                return true;
            }
            if (msg.keys & KEY_DOWN)
            {
                for (int i = uia.message + 1; i < APRS_INBOX_MAX; i++)
                {
                    if (m->in[i].inUse) { uia.message = (uint8_t)i; break; }
                }
                return true;
            }
            if (msg.keys & KEY_ENTER)
            {
                aprs_mbox_markRead(m, uia.message);
                state.ui_screen = APRS_MSG_VIEW;
                return true;
            }
            if (msg.keys & KEY_F1)
            {
                if (m->in[uia.message].inUse)
                    startReply(m->in[uia.message].from);
                return true;
            }
            if (msg.keys & KEY_F2)             /* send a beacon now */
            {
                aprs_requestBeacon((uint32_t)getTick());
                snprintf(uia.note, sizeof(uia.note), "%s",
                         "BEACON QUEUED");
                uia.noteUntil = (uint32_t)getTick() + 2000u;
                return true;
            }
            if (msg.keys & KEY_ESC)
            {
                state.ui_screen = MENU_APRS;
                return true;
            }
            return true;
        }

        case APRS_MSG_VIEW:
        {
            aprs_mbox_t *m = aprs_mailbox();
            if (msg.keys & KEY_ENTER)
            {
                if (m->in[uia.message].inUse)
                    startReply(m->in[uia.message].from);
                return true;
            }
            /*
             * P1 is "NEXT" on this screen's bar (drawMessageView): the next
             * message of the inbox, wrapping, so a whole inbox can be read
             * without going back to the list between two messages. It used to
             * be on the bar and bound to nothing.
             */
            if (msg.keys & KEY_F1)
            {
                for (uint8_t step = 1; step < APRS_INBOX_MAX; step++)
                {
                    uint8_t i = (uint8_t)((uia.message + step) % APRS_INBOX_MAX);
                    if (m->in[i].inUse)
                    {
                        uia.message = i;
                        break;
                    }
                }
                return true;
            }
            if (msg.keys & KEY_ESC)
            {
                state.ui_screen = APRS_MSGS;
                return true;
            }
            return true;
        }

        case APRS_MSG_EDIT:
        {
            /*
             * The C62 has no LEFT/RIGHT keys: the editor used to commit a
             * character on RIGHT and backspace on LEFT, so on this radio a
             * reply could be started, deleted from and sent, but never typed.
             * P1 is now "add the chosen character" (what RIGHT did) and P2 is
             * "delete the last one"; long P1 is the old backspace. UP/DOWN keep
             * cycling the character, and the hints row says so.
             */
            if (msg.keys & KEY_UP)    { editCycle(+1); return true; }
            if (msg.keys & KEY_DOWN)  { editCycle(-1); return true; }
            if ((msg.keys & KEY_F1) && msg.long_press)
            {
                const size_t len = strlen(uia.editText);
                if (len > 0)
                {
                    uia.editText[len - 1] = '\0';
                    uia.editPos = (uint8_t)(len - 1);
                }
                return true;
            }
            if ((msg.keys & KEY_F1) || (msg.keys & KEY_RIGHT))
            {
                editCommit();
                return true;
            }
            if (msg.keys & KEY_LEFT)
            {
                const size_t len = strlen(uia.editText);
                if (len > 0)
                {
                    uia.editText[len - 1] = '\0';
                    uia.editPos = (uint8_t)(len - 1);
                }
                return true;
            }
            if (msg.keys & KEY_ENTER)
            {
                if (strlen(uia.editText) > 0)
                {
                    /*
                     * The mailbox can refuse the message (a full outbox, an
                     * address the AX.25 layer will not take).  Leaving the
                     * editor as if it had been sent threw the text away and
                     * said nothing.
                     */
                    if (aprs_sendMessage(uia.editTo, uia.editText,
                                         (uint32_t)getTick()))
                    {
                        state.ui_screen = APRS_MSGS;
                    }
                    else
                    {
                        snprintf(uia.note, sizeof(uia.note), "%s",
                                 "SEND FAILED");
                        uia.noteUntil = (uint32_t)getTick() + 2000u;
                    }
                }
                return true;
            }
            if (msg.keys & KEY_F2)             /* delete last character */
            {
                const size_t len = strlen(uia.editText);
                if (len > 0) uia.editText[len - 1] = '\0';
                return true;
            }
            if (msg.keys & KEY_ESC)
            {
                state.ui_screen = APRS_MSGS;
                return true;
            }
            return true;
        }

        case APRS_CFG:
        {
            if (msg.keys & KEY_DOWN)
            {
                do
                    uia.field = (uint8_t)((uia.field + 1u) % 9u);
                while (configReadOnly(uia.field));
                return true;
            }
            if (msg.keys & KEY_UP)
            {
                do
                    uia.field = (uint8_t)((uia.field + 8u) % 9u);
                while (configReadOnly(uia.field));
                return true;
            }
            /* P1/P2 change the value: the C62 keypad has no LEFT/RIGHT */
            if (msg.keys & KEY_F2) { configAdjust(+1); return true; }
            if (msg.keys & KEY_F1) { configAdjust(-1); return true; }
            if (msg.keys & KEY_ENTER)          /* save to NVM */
            {
                const int rc = nvm_writeSettings(&state.settings);
                snprintf(uia.note, sizeof(uia.note), "%s",
                         (rc == 0) ? "SAVED" : "WRITE FAILED");
                uia.noteUntil = (uint32_t)getTick() + 2000u;
                return true;
            }
            if (msg.keys & KEY_ESC)
            {
                /* Back where the page was opened from: the APRS list or the
                   quick menu over the main screen. */
                state.ui_screen = (uia.cfgReturn != APRS_CFG)
                                ? uia.cfgReturn : MENU_APRS;
                return true;
            }
            return true;
        }

        default:
            return false;
    }
}

void ui_aprs_statusLine(char *buf, size_t cap)
{
    if (!state.settings.aprs_decode)
    {
        buf[0] = '\0';
        return;
    }

    aprs_mbox_t *m = aprs_mailbox();
    if (aprs_mbox_unread(m) > 0)
        snprintf(buf, cap, "APRS !%u", (unsigned)aprs_mbox_unread(m));
    else
        snprintf(buf, cap, "APRS %u", (unsigned)aprs_rxCount());
}
