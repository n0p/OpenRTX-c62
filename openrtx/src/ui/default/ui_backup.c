/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file ui_backup.c
 * @brief The two screens of the internal backup: take one, restore one.
 *
 * docs/BACKUP.md has the format; this file has what the operator sees:
 *
 *  - MENU_BACKUP shows what a backup carries and the record already on flash,
 *    and takes a new one with green. Nothing is destroyed, so no confirmation.
 *  - MENU_RESTORE shows the record it found (size, date, CRC) and asks before
 *    it writes anything, with the same YES/NO pair the channel list uses to
 *    delete a channel. Red is "no" and goes back.
 *
 * Both screens stay up and report the outcome in place ("BACKUP OK 8634 B",
 * "CRC BAD", "NO BACKUP"), so an operator who is holding the radio does not
 * have to look anywhere else for the only thing he wants to know.
 */

#include <stdio.h>
#include <string.h>

#include "core/backup_image.h"
#include "core/beeps.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "ui/ui_backup.h"
#include "ui/ui_default.h"
#include "ui/ui_layout.h"
#include "ui/ui_strings.h"

/** What the screen is doing right now. */
enum backupStage
{
    BK_ASK = 0, /**< waiting for the operator (and showing the record) */
    BK_OK,      /**< the last action finished well                      */
    BK_FAIL     /**< it did not, and the reason is on the screen        */
};

/** Of which screen: one state each, they are never up at the same time. */
typedef struct
{
    uint8_t stage;
    int     rc;       /**< result of the last action    */
    bkpInfo_t info;   /**< what is on flash             */
    bool    haveInfo;
} backupScreen_t;

static backupScreen_t screenState[2];

#define BK_LINE_H 11
#define BK_FIRST  (UI_LIST_TOP + 9)

static void beepOk(void)
{
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
}

static void beepError(void)
{
    vp_beep(400, SHORT_BEEP * 2);
}

static backupScreen_t *stateOf(uint8_t screen)
{
    return &screenState[screen == MENU_RESTORE ? 1 : 0];
}

/** "26/11 20:31", or "no clock" when the radio has no real-time clock. */
static void formatWhen(const datetime_t *t, char *buf, size_t cap)
{
    if (t->year == 0 && t->month == 0 && t->date == 0) {
        snprintf(buf, cap, "no clock");
        return;
    }

    snprintf(buf, cap, "%02u/%02u %02u:%02u", (unsigned)t->date,
             (unsigned)t->month, (unsigned)t->hour, (unsigned)t->minute);
}

static void drawCentered(uint8_t line, fontSize_t font, color_t colour,
                         const char *text)
{
    point_t p = { CONFIG_SCREEN_WIDTH / 2,
                  (int16_t)(BK_FIRST + line * BK_LINE_H) };

    gfx_print(p, font, TEXT_ALIGN_CENTER, colour, "%s", text);
}

static void drawLeft(uint8_t line, fontSize_t font, const char *text)
{
    point_t p = { UI_TEXT_X, (int16_t)(BK_FIRST + line * BK_LINE_H) };

    gfx_print(p, font, TEXT_ALIGN_LEFT, color_white, "%s", text);
}

static void drawBackup(void)
{
    backupScreen_t *s = stateOf(MENU_BACKUP);
    char line[40], when[16];

    ui_list_header(currentLanguage->flashBackup, NULL);

    snprintf(line, sizeof(line), "channels %d, contacts %d",
             chstore_count(), cstore_count());
    drawLeft(0, FONT_SIZE_5PT, line);
    drawLeft(1, FONT_SIZE_5PT, "plus settings, VFO, messages, zones");

    if (s->haveInfo) {
        formatWhen(&s->info.when, when, sizeof(when));
        snprintf(line, sizeof(line), "last %s", when);
        drawLeft(2, FONT_SIZE_5PT, line);
        snprintf(line, sizeof(line), "%lu B, record %lu",
                 (unsigned long)s->info.size,
                 (unsigned long)s->info.sequence);
        drawLeft(3, FONT_SIZE_5PT, line);
    } else {
        drawLeft(2, FONT_SIZE_5PT, "no backup on flash yet");
    }

    switch (s->stage) {
        case BK_OK:
            drawCentered(5, FONT_SIZE_8PT, color_white, bkp_lastResult());
            drawCentered(6, FONT_SIZE_5PT, color_white, "green saves again");
            break;

        case BK_FAIL:
            drawCentered(5, FONT_SIZE_8PT, yellow_fab413, bkp_lastResult());
            drawCentered(6, FONT_SIZE_5PT, color_white, "nothing was lost");
            break;

        default:
            drawCentered(5, FONT_SIZE_8PT, color_white, "Save the stores?");
            drawCentered(6, FONT_SIZE_5PT, color_white,
                         "green writes them to flash");
            break;
    }
}

static void drawRestore(void)
{
    backupScreen_t *s = stateOf(MENU_RESTORE);
    char line[40], when[16];

    ui_list_header(currentLanguage->flashRestore, NULL);

    if (!s->haveInfo) {
        drawCentered(3, FONT_SIZE_8PT, yellow_fab413, "NO BACKUP");
        drawCentered(5, FONT_SIZE_5PT, color_white,
                     "menu: Backup & Restore");
        drawCentered(6, FONT_SIZE_5PT, color_white, "take one first");
        return;
    }

    formatWhen(&s->info.when, when, sizeof(when));
    snprintf(line, sizeof(line), "record %lu, %lu B",
             (unsigned long)s->info.sequence, (unsigned long)s->info.size);
    drawLeft(0, FONT_SIZE_5PT, line);
    snprintf(line, sizeof(line), "taken %s", when);
    drawLeft(1, FONT_SIZE_5PT, line);
    drawLeft(2, FONT_SIZE_5PT, "settings, VFO, channels, contacts,");
    drawLeft(3, FONT_SIZE_5PT, "messages and zones");

    switch (s->stage) {
        case BK_OK:
            drawCentered(5, FONT_SIZE_8PT, color_white, bkp_lastResult());
            drawCentered(6, FONT_SIZE_5PT, color_white,
                         "radio reloaded, settings applied");
            break;

        case BK_FAIL:
            drawCentered(5, FONT_SIZE_8PT, yellow_fab413, bkp_lastResult());
            drawCentered(6, FONT_SIZE_5PT, color_white,
                         "nothing else was touched");
            break;

        default:
            /*
             * The confirmation. The record has already been read and its CRC
             * checked (bkp_inspect), so what is on this line is a record the
             * restore will accept: "CRC ok" is a fact, not a hope.
             */
            drawCentered(5, FONT_SIZE_8PT, color_white, "Restore it?");
            drawCentered(6, FONT_SIZE_5PT, yellow_fab413,
                         "YES overwrites the stores");
            break;
    }
}

void ui_backup_draw(uint8_t screen)
{
    if (screen == MENU_RESTORE)
        drawRestore();
    else
        drawBackup();
}

void ui_backup_open(uint8_t screen)
{
    backupScreen_t *s = stateOf(screen);

    memset(s, 0, sizeof(*s));
    s->stage = BK_ASK;

    /* What is on flash decide whether the screen offers anything at all */
    s->rc = bkp_inspect(&s->info);
    s->haveInfo = (s->rc == BKP_OK);
}

bool ui_backup_handleEvent(uint8_t screen, kbd_msg_t *msg, bool *sync_rtx)
{
    backupScreen_t *s = stateOf(screen);

    if (msg->keys & KEY_ESC) {
        /*
         * Red is "no" and goes back to the two-row menu, with the cursor on the
         * row this screen belongs to. Leaving it on row 0 (Backup) made the
         * next green open the *backup* screen instead of the restore the
         * operator had just declined, and the green after that wrote a new
         * record: a "no" ended in a write to flash.
         */
        state.ui_screen = MENU_BACKUP_RESTORE;
        ui_menu_select(screen == MENU_RESTORE ? BR_RESTORE : BR_BACKUP);
        return true;
    }

    if ((msg->keys & KEY_ENTER) == 0)
        return true;

    if (screen == MENU_BACKUP) {
        int rc = bkp_backup();

        s->rc = rc;
        s->stage = (rc == BKP_OK) ? BK_OK : BK_FAIL;
        s->haveInfo = (bkp_inspect(&s->info) == BKP_OK);

        if (rc == BKP_OK)
            beepOk();
        else
            beepError();

        return true;
    }

    /* Restore: green is "yes" only when there is something to restore */
    if (!s->haveInfo) {
        beepError();
        s->stage = BK_FAIL;
        return true;
    }

    int rc = bkp_restore();

    s->rc = rc;
    s->stage = (rc == BKP_OK) ? BK_OK : BK_FAIL;

    if (rc == BKP_OK) {
        /*
         * The engine has already put the restored settings and channel in
         * `state` and written every store through its own path: what is left
         * is telling the radio to take the new configuration.
         */
        *sync_rtx = true;
        ui_rtxSyncRequest = true;
        beepOk();
    } else {
        beepError();
    }

    return true;
}
