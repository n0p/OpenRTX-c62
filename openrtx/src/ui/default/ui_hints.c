/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "ui/ui_default.h"
#include "ui/ui_hints.h"

/*
 * The C62's own key names, so the hints read like the stock firmware's manual:
 * the green key is MENU there (ENTER here), the red one is A/B (ESC here) and
 * P1/P2 are its two programmable keys. The side key is the secondary PTT, not
 * a hint: PTT is not a menu key.
 */
static const ui_hints_t hints_nav     = { "OK",  NULL, NULL, "Back" };
static const ui_hints_t hints_edit    = { "Edit", NULL, NULL, "Back" };
static const ui_hints_t hints_vfo_in  = { "Set", NULL, NULL, "Cancel" };
static const ui_hints_t hints_channel = { "Load", NULL, NULL, "Back" };
static const ui_hints_t hints_reset   = { "Reset", NULL, NULL, "Back" };
/* A mode's settings page behaves like the popup: green cycles, P1/P2 change */
static const ui_hints_t hints_modePage = { "Chg", "-", "+", "Back" };
/*
 * The analyser: green re-centres the span on the frequency the radio is tuned
 * to, P1 walks the span, P2 opens its quick menu and red leaves.
 */
static const ui_hints_t hints_spectrum = { "Centrar", "Span", "Menu", "Exit" };

const ui_hints_t *ui_hints_for_screen(uint8_t screen)
{
    switch (screen) {
        case MAIN_VFO_INPUT:
            return &hints_vfo_in;

        case MENU_TOP:
        case MENU_BANK:
        case MENU_SETTINGS:
        case MENU_BACKUP_RESTORE:
        case MENU_BACKUP:
        case MENU_RESTORE:
        case MENU_INFO:
        case MENU_ABOUT:
#ifdef CONFIG_GPS
        case MENU_GPS:
#endif
            return &hints_nav;

        /*
         * The CPS channel list has no per-entry action yet on the targets that
         * still use it (there is no codeplug behind cps_io_stub), so P1/P2
         * stay empty. MENU_CONTACTS is deliberately absent: the in-radio
         * contact book (ui_contacts.c) draws its own bar, and so do the
         * channel editor and the messaging screens.
         */
        case MENU_CHANNEL:
            return &hints_channel;

        case SPECTRUM:
            return &hints_spectrum;

        case SETTINGS_DISPLAY:
        case SETTINGS_RADIO:
        case SETTINGS_AUDIO:
        case SETTINGS_M17:
#ifdef CONFIG_TETRA_AFC
        case SETTINGS_TETRA:
#endif
        case SETTINGS_FM:
        case SETTINGS_ACCESSIBILITY:
#ifdef CONFIG_RTC
        case SETTINGS_TIMEDATE:
        case SETTINGS_TIMEDATE_SET:
#endif
            return &hints_edit;

        /*
         * A mode's own page (Settings → NXDN, Settings → AM): its rows are the
         * mode's quick menu ones, so the keys mean here what they mean there.
         */
        case SETTINGS_MODE:
            return &hints_modePage;

        case SETTINGS_RESET2DEFAULTS:
            return &hints_reset;

        default:
            /* Main screens: the bottom row is the S-meter */
            return NULL;
    }
}

static uint16_t hint_width(const char *key, const char *action)
{
    char buf[24];
    if(action == NULL)
        return 0;
    if(key == NULL)
        snprintf(buf, sizeof(buf), "%s", action);
    else
        snprintf(buf, sizeof(buf), "%s:%s", key, action);
    return gfx_getTextWidth(FONT_SIZE_5PT, buf) + 6;
}

void ui_hints_draw(const ui_hints_t *hints)
{
    if (hints == NULL)
        return;

    uint16_t y = CONFIG_SCREEN_HEIGHT - UI_HINTS_H;
    point_t bar = { 0, (int16_t)y };
    gfx_drawRect(bar, CONFIG_SCREEN_WIDTH, UI_HINTS_H, color_black, true);
    gfx_drawHLine((int16_t)y, 1, color_grey);

    uint16_t width[4];
    const char *keys[4] = { NULL, "P1", "P2", NULL };
    const char *acts[4] = { hints->green, hints->p1, hints->p2, hints->red };

    uint16_t total = 0;
    for (uint8_t i = 0; i < 4; i++) {
        width[i] = hint_width(keys[i], acts[i]);
        total += width[i];
    }

    /* Centre the group, dropping the separators if the four hints do not fit */
    int16_t x = (total < CONFIG_SCREEN_WIDTH) ?
                (CONFIG_SCREEN_WIDTH - total) / 2 : 2;
    for (uint8_t i = 0; i < 4; i++) {
        if (acts[i] == NULL)
            continue;

        point_t pos = { x, (int16_t)(y + 1) };
        if (keys[i] != NULL) {
            gfx_print(pos, FONT_SIZE_5PT, TEXT_ALIGN_LEFT, yellow_fab413,
                      "%s:", keys[i]);
            pos.x += gfx_getTextWidth(FONT_SIZE_5PT, keys[i]) + 2;
        }
        gfx_print(pos, FONT_SIZE_5PT, TEXT_ALIGN_LEFT, color_white, "%s",
                  acts[i]);
        x += width[i];
    }
}

void ui_hints_draw_for_screen(uint8_t screen)
{
    ui_hints_draw(ui_hints_for_screen(screen));
}
