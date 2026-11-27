/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <hwconfig.h>
#include "core/graphics.h"
#include "core/state.h"
#include "core/contact_store.h"
#include "core/channel_store.h"
#include "core/beeps.h"
#include "core/voicePrompts.h"
#include "interfaces/delays.h"
#include "ui/ui_default.h"
#include "ui/ui_contacts.h"
#include "ui/ui_hints.h"
#ifdef UI_CAPTURE
#include "core/gfx_capture.h"
#endif
#include "ui/ui_layout.h"

/*
 * Contact book screens, on top of MENU_CONTACTS:
 *
 *   list     UP/DOWN scroll, green edits, P1 adds, P2 edits, P2 long deletes
 *            after a confirmation, red goes back to the main menu.
 *   editor   UP/DOWN picks a field; on CALL and NAME UP/DOWN changes the
 *            character under the cursor and P1/P2 move the cursor; on MODE
 *            UP/DOWN changes the transport. Green saves, red discards.
 *
 * The same rule as the channel editor applies: no LEFT/RIGHT anywhere,
 * because the C62 keypad has no arrow keys (keyboard_c62.c: enter, red, up,
 * down, F1, F2, MONI and the 12 digits).
 */

#define CT_HEADER_H UI_HEADER_H
#define CT_ROW_H    12
#define CT_ROWS     ((CONFIG_SCREEN_HEIGHT - UI_LIST_TOP - UI_HINTS_H) / CT_ROW_H)
#define CT_MSG_MS   2000

enum ctScreen
{
    CTS_CLOSED = 0,
    CTS_LIST,
    CTS_EDIT,
    CTS_CONFIRM
};

enum ctField
{
    CTF_CALL = 0,
    CTF_NAME,
    CTF_MODE,
    CTF_NUM
};

static const char *const fieldLabels[CTF_NUM] = { "CALL", "NAME", "MODE" };

/* Characters offered for callsigns and names, in UP/DOWN order */
static const char editChars[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-./_@";

static const uint8_t editModes[] = { OPMODE_FM, OPMODE_M17, OPMODE_DMR,
                                     OPMODE_TETRA };

static struct
{
    enum ctScreen screen;
    uint16_t sel;
    uint16_t top;
    uint16_t editIdx;
    cstore_contact_t edit;
    uint8_t field;
    uint8_t cursor;
    char msg[22];
    long long msgUntil;
} ct;

/*
 * P2 on the list: edit on a tap, delete on a hold.
 *
 * The keypad delivers the press and, 700 ms later, the long press as two
 * events, so acting on the press edge opened the editor and the long press
 * then arrived in it, where P2 moves the cursor: the confirmation that erases
 * a contact could not be reached.  The press only remembers the key and the
 * release (keys == 0) decides the tap, as in the channel list.
 */
static bool p2Held = false;

static const char *modeName(uint8_t mode)
{
    switch (mode) {
        case OPMODE_FM:    return "FM";
        case OPMODE_M17:   return "M17";
        case OPMODE_DMR:   return "DMR";
        case OPMODE_TETRA: return "TETRA";
        default:           return "FM";
    }
}

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
    snprintf(ct.msg, sizeof(ct.msg), "%s", text);
    ct.msgUntil = getTick() + CT_MSG_MS;
}

static void clampList(void)
{
    uint16_t count = (uint16_t)cstore_count();

    ui_list_clamp(&ct.sel, &ct.top, count, (uint8_t)CT_ROWS);
}

/* ------------------------------------------------------------------------ */
/* Editing                                                                  */
/* ------------------------------------------------------------------------ */

static char *fieldText(uint8_t field)
{
    return (field == CTF_CALL) ? ct.edit.call : ct.edit.name;
}

static uint8_t fieldLen(uint8_t field)
{
    return (field == CTF_CALL) ? CSTORE_CALL_LEN : CSTORE_NAME_LEN;
}

static void textStep(int dir)
{
    char *s = fieldText(ct.field);
    uint8_t cap = fieldLen(ct.field);
    size_t len = strnlen(s, cap);

    if (len == 0) {
        if (dir < 0)
            return;
        s[0] = editChars[1];
        s[1] = '\0';
        ct.cursor = 0;
        return;
    }

    if (ct.cursor >= len)
        ct.cursor = (uint8_t)(len - 1);
    if (ct.cursor >= cap - 1)
        ct.cursor = (uint8_t)(cap - 2);

    /* Past the end: append a space and stay there, like the channel editor */
    if ((size_t)(ct.cursor + 1) == len && dir > 0 && len + 1 < cap) {
        s[len] = ' ';
        s[len + 1] = '\0';
        ct.cursor = (uint8_t)len;
        return;
    }

    const char *p = strchr(editChars, s[ct.cursor]);
    int n = (int)strlen(editChars);
    int i = (p == NULL) ? 0 : (int)(p - editChars);
    i = (i + dir + n) % n;
    s[ct.cursor] = editChars[i];
}

static void fieldChange(int dir)
{
    if (ct.field == CTF_MODE) {
        int cur = 0;
        for (size_t i = 0; i < sizeof(editModes); i++) {
            if (editModes[i] == ct.edit.mode)
                cur = (int)i;
        }
        cur = (cur + dir + (int)sizeof(editModes)) % (int)sizeof(editModes);
        ct.edit.mode = editModes[cur];
        return;
    }

    textStep(dir);
}

static void cursorStep(int dir)
{
    if (ct.field == CTF_MODE)
        return;

    uint8_t cap = fieldLen(ct.field);
    size_t len = strnlen(fieldText(ct.field), cap);

    if (len == 0)
        return;

    if (dir > 0)
        ct.cursor = ((size_t)(ct.cursor + 1) < len) ? (uint8_t)(ct.cursor + 1) : 0;
    else
        ct.cursor = (ct.cursor > 0) ? (uint8_t)(ct.cursor - 1)
                                    : (uint8_t)(len - 1);
}

static void editorEnter(uint16_t idx, const cstore_contact_t *from)
{
    if (from != NULL) {
        ct.edit = *from;
        ct.editIdx = idx;
    } else {
        memset(&ct.edit, 0, sizeof(ct.edit));
        ct.editIdx = CSTORE_NONE;
        ct.edit.mode = state.channel.mode;
    }

    ct.edit.call[CSTORE_CALL_LEN - 1] = '\0';
    ct.edit.name[CSTORE_NAME_LEN - 1] = '\0';
    ct.field = CTF_CALL;
    size_t len = strnlen(ct.edit.call, CSTORE_CALL_LEN);
    ct.cursor = (uint8_t)((len > 0) ? len - 1 : 0);
    ct.screen = CTS_EDIT;
}

static void editorSave(void)
{
    if (!cstore_valid(&ct.edit)) {
        setMessage("CALL EMPTY");
        beepError();
        return;
    }

    int ret;
    if (ct.editIdx == CSTORE_NONE) {
        ret = cstore_add(&ct.edit);
        if (ret >= 0) {
            ct.sel = (uint16_t)ret;
            ct.editIdx = (uint16_t)ret;
        }
    } else {
        ret = cstore_set(ct.editIdx, &ct.edit);
    }

    if (ret < 0) {
        setMessage("WRITE FAILED");
        beepError();
        return;
    }

    clampList();
    beepOk();
    ct.screen = CTS_LIST;
}

static void deleteSel(void)
{
    if (cstore_delete(ct.sel) != 0) {
        setMessage("DELETE FAILED");
        beepError();
    } else {
        beepOk();
    }
    clampList();
    ct.screen = CTS_LIST;
}

/* ------------------------------------------------------------------------ */
/* Key handling                                                             */
/* ------------------------------------------------------------------------ */

static bool listKey(kbd_msg_t *msg)
{
    uint16_t count = (uint16_t)cstore_count();

    /* A key that joins P2 while it is down makes the press something else */
    if (p2Held && (msg->keys != 0) && ((msg->keys & KEY_F2) == 0))
        p2Held = false;

    /* UP is the previous contact, as in every other list */
    if (msg->keys & KEY_UP) {
        if (count > 0)
            ct.sel = (ct.sel > 0) ? (uint16_t)(ct.sel - 1)
                                  : (uint16_t)(count - 1);
        clampList();
    } else if (msg->keys & KEY_DOWN) {
        if (count > 0)
            ct.sel = (ct.sel + 1 < count) ? (uint16_t)(ct.sel + 1) : 0;
        clampList();
    } else if (msg->keys & KEY_ENTER) {
        if (count > 0) {
            cstore_contact_t c;
            if (cstore_get(ct.sel, &c) == 0)
                editorEnter(ct.sel, &c);
        } else {
            editorEnter(CSTORE_NONE, NULL);
        }
    } else if (msg->keys & KEY_F1) {
        editorEnter(CSTORE_NONE, NULL);
    } else if (msg->keys & KEY_F2) {
        if (count == 0)
            return true;

        if (msg->long_press) {
            /* Only a press this screen saw can become a delete: the tail of a
             * keystroke the editor took is not one (see the note above) */
            if (p2Held) {
                p2Held = false;
                ct.screen = CTS_CONFIRM;
            }
        } else {
            p2Held = true;
        }
    } else if (msg->keys == 0) {
        /* The key came up: a tap, which edits the selected contact */
        if (p2Held) {
            p2Held = false;
            cstore_contact_t c;
            if (cstore_get(ct.sel, &c) == 0)
                editorEnter(ct.sel, &c);
        }
    } else if (msg->keys & KEY_ESC) {
        p2Held = false;
        ct.screen = CTS_CLOSED;
        state.ui_screen = MENU_TOP;
    }

    return true;
}

static bool editKey(kbd_msg_t *msg)
{
    if (msg->keys & KEY_UP) {
        fieldChange(+1);
    } else if (msg->keys & KEY_DOWN) {
        fieldChange(-1);
    } else if (msg->keys & KEY_F1) {
        /* On CALL/NAME: move the cursor. On MODE: same as DOWN. */
        if (ct.field == CTF_MODE)
            fieldChange(-1);
        else
            cursorStep(-1);
    } else if (msg->keys & KEY_F2) {
        if (ct.field == CTF_MODE)
            fieldChange(+1);
        else
            cursorStep(+1);
    } else if ((msg->keys & KEY_ENTER) && msg->long_press) {
        /* Long green is "next field", so that a whole record can be typed
           with one hand without leaving the editor */
        ct.field = (uint8_t)((ct.field + 1) % CTF_NUM);
        size_t len = strnlen(fieldText(ct.field), fieldLen(ct.field));
        ct.cursor = (uint8_t)((len > 0) ? len - 1 : 0);
    } else if (msg->keys & KEY_ENTER) {
        editorSave();
    } else if (msg->keys & KEY_ESC) {
        beepError();
        ct.screen = CTS_LIST;
    }

    return true;
}

bool ui_contacts_handleEvent(kbd_msg_t *msg, bool *sync_rtx)
{
    (void)sync_rtx;

    switch (ct.screen) {
        case CTS_LIST:
            return listKey(msg);

        case CTS_EDIT:
            return editKey(msg);

        case CTS_CONFIRM:
            if (msg->keys & KEY_ENTER)
                deleteSel();
            else if (msg->keys & KEY_ESC)
                ct.screen = CTS_LIST;
            return true;

        case CTS_CLOSED:
        default:
            return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                  */
/* ------------------------------------------------------------------------ */

static void drawHeader(const char *title, const char *right)
{
    ui_list_header(title, right);
}

static void drawList(void)
{
    char right[16];
    uint16_t count = (uint16_t)cstore_count();

    snprintf(right, sizeof(right), "%u/%u", (unsigned)count,
             (unsigned)CSTORE_MAX_CONTACTS);
    drawHeader("CONTACTS", right);

    if (count == 0) {
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2,
                             (int16_t)(UI_LIST_TOP + 24) },
                  FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white,
                  "No contacts");
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2,
                             (int16_t)(UI_LIST_TOP + 40) },
                  FONT_SIZE_5PT, TEXT_ALIGN_CENTER, color_white,
                  "P1 / green adds one");
        ui_hints_draw(&(ui_hints_t){ "ADD", "ADD", NULL, "BACK" });
        return;
    }

    for (uint8_t row = 0; row < CT_ROWS; row++) {
        uint16_t idx = (uint16_t)(ct.top + row);
        if (idx >= count)
            break;

        cstore_contact_t c;
        if (cstore_get(idx, &c) != 0)
            break;

        int16_t y = (int16_t)(UI_LIST_TOP + row * CT_ROW_H);
        int16_t baseline = ui_row_baseline_at(y, CT_ROW_H);
        bool selected = (idx == ct.sel);

        if (selected)
            ui_row_selection(y, CT_ROW_H, true);

        color_t fg = selected ? color_black : color_white;
        gfx_print((point_t){ UI_TEXT_X, baseline }, FONT_SIZE_6PT,
                  TEXT_ALIGN_LEFT, fg, "%s", c.call);
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH - UI_TEXT_X, baseline },
                  FONT_SIZE_5PT, TEXT_ALIGN_RIGHT, fg, "%s",
                  modeName(c.mode));
    }

    if (ct.msg[0] != '\0' && getTick() < ct.msgUntil)
        ui_hints_draw(&(ui_hints_t){ ct.msg, NULL, NULL, NULL });
    else
        ui_hints_draw(&(ui_hints_t){ "EDIT", "NEW", "EDIT", "BACK" });
}

static void drawEditor(void)
{
    char title[24];
    char value[CSTORE_NAME_LEN + 2];

    if (ct.editIdx == CSTORE_NONE)
        snprintf(title, sizeof(title), "NEW CONTACT");
    else
        snprintf(title, sizeof(title), "EDIT %u/%u",
                 (unsigned)(ct.editIdx + 1), (unsigned)cstore_count());

    drawHeader(title, NULL);

    uint16_t y = UI_LIST_TOP + 4;
    for (uint8_t f = 0; f < CTF_NUM; f++) {
        bool selected = (f == ct.field);
        int16_t rowY = (int16_t)(y + f * 16);
        int16_t baseline = ui_row_baseline_at(rowY, 14);

        if (selected) {
            gfx_print((point_t){ UI_TEXT_X, baseline }, FONT_SIZE_5PT,
                      TEXT_ALIGN_LEFT, yellow_fab413, ">");
        }

        gfx_print((point_t){ 12, baseline }, FONT_SIZE_5PT,
                  TEXT_ALIGN_LEFT, selected ? yellow_fab413 : color_white,
                  "%s", fieldLabels[f]);

        if (f == CTF_MODE)
            snprintf(value, sizeof(value), "%s", modeName(ct.edit.mode));
        else
            snprintf(value, sizeof(value), "%s", fieldText(f));

        gfx_print((point_t){ 48, baseline }, FONT_SIZE_6PT,
                  TEXT_ALIGN_LEFT, color_white, "%s", value);

        /* Caret under the character being changed */
        if (selected && f != CTF_MODE && value[0] != '\0') {
            char prefix[CSTORE_NAME_LEN + 1];
            uint8_t n = ct.cursor;
            if (n > strlen(value))
                n = (uint8_t)strlen(value);
            memcpy(prefix, value, n);
            prefix[n] = '\0';
            uint16_t x0 = (uint16_t)(48 + gfx_getTextWidth(FONT_SIZE_6PT,
                                                          prefix));
            uint16_t x1 = (uint16_t)(48 + gfx_getTextWidth(FONT_SIZE_6PT,
                                                           value));
            uint16_t caretW = (uint16_t)((x1 > x0 + 6) ? 6 : (x1 - x0));
            gfx_drawRect((point_t){ (int16_t)x0, (int16_t)(baseline + 3) },
                         caretW, 1, yellow_fab413, true);
#ifdef UI_CAPTURE
            gfx_capture_caret((int16_t)x0, (int16_t)(baseline + 3), caretW);
#endif
        }
    }

    ui_hints_draw(&(ui_hints_t){ "SAVE", "PREV", "NEXT", "CANCEL" });
}

static void drawConfirm(void)
{
    cstore_contact_t c;

    drawHeader("DELETE", NULL);
    gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2, CT_HEADER_H + 26 },
              FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white, "Delete contact?");
    if (cstore_get(ct.sel, &c) == 0) {
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2, CT_HEADER_H + 44 },
                  FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white, "%s", c.call);
    }
    ui_hints_draw(&(ui_hints_t){ "YES", NULL, NULL, "NO" });
}

void ui_contacts_draw(void)
{
    switch (ct.screen) {
        case CTS_LIST:    drawList();    break;
        case CTS_EDIT:    drawEditor();  break;
        case CTS_CONFIRM: drawConfirm(); break;
        default:          break;
    }
}

void ui_contacts_open(void)
{
    cstore_init();
    ct.screen = CTS_LIST;
    ct.msg[0] = '\0';
    p2Held = false;
    clampList();
}

bool ui_contacts_isOpen(void)
{
    return ct.screen != CTS_CLOSED;
}

uint16_t ui_contacts_count(void)
{
    return (uint16_t)cstore_count();
}

bool ui_contacts_callAt(uint16_t idx, char *buf, unsigned len)
{
    cstore_contact_t c;
    if (buf == NULL || cstore_get(idx, &c) != 0)
        return false;
    snprintf(buf, len, "%s", c.call);
    return true;
}

uint16_t ui_contacts_find(const char *call)
{
    return cstore_findCall(call);
}
