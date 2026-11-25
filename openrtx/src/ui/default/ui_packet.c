/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/ui_packet.h"
#include "ui/ui_default.h"
#include "ui/ui_layout.h"
#include "ui/ui_hints.h"
#include "core/graphics.h"
#include "core/state.h"
#include "core/packet_service.h"
#include "core/settings.h"
#include "interfaces/delays.h"
#include "protocols/APRS/ax25.h"
#include "protocols/KISS/kiss.h"
#include <stdio.h>
#include <string.h>

#define PKT_ROWS 8

static struct
{
    uint8_t field;      /**< selected row of the configuration page */
    uint8_t back;       /**< screen red returns to                 */
} uip;

static const char *modemLabel(pkt_modem_t m)
{
    return pkt_modem_info(m)->name;
}

/* ------------------------------------------------------------- drawing --- */

static void hints(const char *green, const char *p1, const char *p2,
                  const char *red)
{
    ui_hints_draw(&(ui_hints_t){ green, p1, p2, red });
}

/* Text of one configuration row, value right-aligned on the right margin. */
static void rowValue(char *out, size_t cap, uint8_t row)
{
    const settings_t *s = &state.settings;

    switch (row)
    {
        case 0:
            snprintf(out, cap, "%s", s->packet_enable ? "on" : "off");
            break;

        case 1:
            snprintf(out, cap, "%s", modemLabel((pkt_modem_t)s->packet_modem));
            break;

        case 2:
            snprintf(out, cap, "%ums", (unsigned)s->packet_txdelay_ms);
            break;

        case 3:
            snprintf(out, cap, "%u", (unsigned)s->packet_persist);
            break;

        case 4:
            snprintf(out, cap, "%ums", (unsigned)s->packet_slottime_ms);
            break;

        case 5:
            snprintf(out, cap, "%ums", (unsigned)s->packet_txtail_ms);
            break;

        case 6:
            snprintf(out, cap, "%s", s->packet_fulldup ? "on" : "off");
            break;

        default:
            snprintf(out, cap, ">");
            break;
    }
}

static const char *rowName(uint8_t row)
{
    static const char *names[PKT_ROWS] =
    {
        "Enable", "Modem", "TXDELAY", "Persist", "SlotTime", "TXtail",
        "Fulldup", "Status"
    };
    return (row < PKT_ROWS) ? names[row] : "";
}

static void drawConfig(void)
{
    packet_status_t st;
    packet_service_status(&st);

    char right[12];
    snprintf(right, sizeof(right), "%s%s", st.enabled ? "" : "off",
             st.enabled ? modemLabel(st.modemId) : "");
    ui_list_header("Packet", right);

    for (uint8_t i = 0; i < PKT_ROWS; i++)
    {
        const int16_t top      = ui_row_top(i);
        const int16_t baseline = ui_row_baseline_at(top, ui_row_height());
        const bool    sel      = (i == uip.field);

        if (sel)
            ui_row_selection(top, ui_row_height(), true);

        char value[12];
        rowValue(value, sizeof(value), i);

        ui_row_text(baseline, UI_ROW_FONT, TEXT_ALIGN_LEFT, sel,
                    "%s", rowName(i));
        ui_row_text(baseline, UI_ROW_FONT, TEXT_ALIGN_RIGHT, sel,
                    "%s", value);
    }

    hints("EDIT", "-", "+", "BACK");
}

static void drawStatus(void)
{
    packet_status_t st;
    packet_service_status(&st);

    char right[12];
    snprintf(right, sizeof(right), "%u", (unsigned)st.queued);
    ui_list_header("Modem status", right);

    static const char *labels[] =
    {
        "Frames TX", "Frames RX", "FCS errors", "Dropped", "TX errors",
        "Carrier", "Queue", "Port"
    };

    char values[8][12];
    snprintf(values[0], sizeof(values[0]), "%lu", (unsigned long)st.txFrames);
    snprintf(values[1], sizeof(values[1]), "%lu", (unsigned long)st.rxFrames);
    snprintf(values[2], sizeof(values[2]), "%lu", (unsigned long)st.rxFcs);
    snprintf(values[3], sizeof(values[3]), "%lu", (unsigned long)st.txDropped);
    snprintf(values[4], sizeof(values[4]), "%lu", (unsigned long)st.txErrors);
    snprintf(values[5], sizeof(values[5]), "%s",
             st.txActive ? "tx" : (st.carrier ? "rx" : "-"));
    snprintf(values[6], sizeof(values[6]), "%u/%u", (unsigned)st.queued, 3u);
    snprintf(values[7], sizeof(values[7]), "%s",
             st.portOpen ? "open" : "none");

    for (uint8_t i = 0; i < 8; i++)
    {
        const int16_t top      = ui_row_top(i);
        const int16_t baseline = ui_row_baseline_at(top, ui_row_height());

        ui_row_text(baseline, UI_ROW_FONT, TEXT_ALIGN_LEFT, false,
                    "%s", labels[i]);
        ui_row_text(baseline, UI_ROW_FONT, TEXT_ALIGN_RIGHT, false,
                    "%s", values[i]);
    }

    hints("CLEAR", NULL, NULL, "BACK");
}

bool ui_packet_isScreen(uint8_t screen)
{
    return (screen == PKT_CFG) || (screen == PKT_STATUS);
}

void ui_packet_draw(uint8_t screen)
{
    if (screen == PKT_STATUS)
        drawStatus();
    else
        drawConfig();
}

/* ---------------------------------------------------------------- fsm --- */

void ui_packet_config(void)
{
    uip.field = 0;
    uip.back  = (state.ui_screen == PKT_CFG) ? MAIN_VFO : state.ui_screen;
    state.ui_screen = PKT_CFG;

    /* The service has to exist before the page can read it */
    packet_service_init();
}

static void adjust(int8_t dir)
{
    settings_t *s = &state.settings;

    switch (uip.field)
    {
        case 0:
            packet_service_setEnabled(!s->packet_enable);
            break;

        case 1:
        {
            int m = (int)s->packet_modem + dir;
            if (m < 0)
                m = PKT_MODEM_COUNT - 1;
            if (m >= PKT_MODEM_COUNT)
                m = 0;
            packet_service_setModem((uint8_t)m);
            break;
        }

        case 2:
        {
            int v = (int)s->packet_txdelay_ms + dir * 50;
            if (v < 0) v = 0;
            if (v > 2000) v = 2000;
            packet_service_setParam(KISS_CMD_TXDELAY, (uint8_t)(v / 10));
            break;
        }

        case 3:
        {
            int v = (int)s->packet_persist + dir * 8;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            packet_service_setParam(KISS_CMD_PERSIST, (uint8_t)v);
            break;
        }

        case 4:
        {
            int v = (int)s->packet_slottime_ms + dir * 10;
            if (v < 10) v = 10;
            if (v > 1000) v = 1000;
            packet_service_setParam(KISS_CMD_SLOTTIME, (uint8_t)(v / 10));
            break;
        }

        case 5:
        {
            int v = (int)s->packet_txtail_ms + dir * 10;
            if (v < 0) v = 0;
            if (v > 500) v = 500;
            packet_service_setParam(KISS_CMD_TXTAIL, (uint8_t)(v / 10));
            break;
        }

        case 6:
            packet_service_setParam(KISS_CMD_FULLDUP, s->packet_fulldup ? 0 : 1);
            break;

        default:
            break;
    }
}

/** One short frame to the local channel, for a bench check of the chain. */
static void sendTest(void)
{
    char src[12];
    snprintf(src, sizeof(src), "%.6s-0", state.settings.callsign);

    ax25_frame_t f;
    memset(&f, 0, sizeof(f));
    ax25_addr_set(&f.dst, "BEACON");
    ax25_addr_set(&f.src, src);
    f.nDigis  = 0;
    f.control = 0x03;
    f.pid     = 0xF0;

    static const char text[] = "OpenRTX packet modem test";
    f.info    = (const uint8_t *)text;
    f.infoLen = sizeof(text) - 1u;

    uint8_t frame[HDLC_MAX_LEN];
    size_t  len = ax25_build_ui(frame, sizeof(frame), &f);
    if (len == 0)
        return;

    packet_service_sendNow(frame, len - 2u);
}

bool ui_packet_fsm(kbd_msg_t msg, bool *sync_rtx)
{
    (void)sync_rtx;

    switch (state.ui_screen)
    {
        case PKT_CFG:
        {
            if (msg.keys & KEY_DOWN)
            {
                uip.field = (uint8_t)((uip.field + 1u) % PKT_ROWS);
                return true;
            }
            if (msg.keys & KEY_UP)
            {
                uip.field = (uint8_t)((uip.field + PKT_ROWS - 1u) % PKT_ROWS);
                return true;
            }
            /* P1/P2 change the value: the C62 has no LEFT/RIGHT */
            if (msg.keys & KEY_F2) { adjust(+1); return true; }
            if (msg.keys & KEY_F1) { adjust(-1); return true; }
            if (msg.keys & KEY_ENTER)
            {
                if (uip.field == (PKT_ROWS - 1u))
                    state.ui_screen = PKT_STATUS;
                else
                    sendTest();
                return true;
            }
            if (msg.keys & KEY_ESC)
            {
                state.ui_screen = uip.back;
                return true;
            }
            return true;
        }

        case PKT_STATUS:
        {
            if (msg.keys & KEY_ENTER)
            {
                packet_service_clearCounters();
                return true;
            }
            if (msg.keys & KEY_ESC)
            {
                state.ui_screen = PKT_CFG;
                return true;
            }
            return true;
        }

        default:
            return false;
    }
}

void ui_packet_statusLine(char *buf, size_t cap)
{
    packet_status_t st;
    packet_service_status(&st);

    if (!st.enabled)
    {
        buf[0] = '\0';
        return;
    }

    if (st.txActive)
        snprintf(buf, cap, "PKT TX");
    else
        snprintf(buf, cap, "PKT %lu", (unsigned long)st.rxFrames);
}
