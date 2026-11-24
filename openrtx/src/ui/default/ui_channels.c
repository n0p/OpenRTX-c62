/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "hwconfig.h"
#include "core/graphics.h"
#include "interfaces/delays.h"
#include "core/state.h"
#include "core/channel_store.h"
#include "core/voicePrompts.h"
#include "core/beeps.h"
#include "ui/ui_default.h"
#include "ui/ui_channels.h"

/*
 * In-radio channel editor.
 *
 * Two screens on top of whatever the UI was showing:
 *
 *   list    UP/DOWN scroll, green tunes, P1 adds (duplicating the selected
 *           channel, or the VFO when the bank is empty), P2 edits, P2 long
 *           press deletes after a confirmation, red goes back.
 *   editor  UP/DOWN changes field, green enters/leaves the field, LEFT/RIGHT
 *           moves inside a name or a frequency, P2 saves, red cancels.
 *
 * Everything is drawn with the 5 and 6 point fonts so that the whole bank is
 * readable on the 160x128 C62 display. Nothing here talks to the radio: a
 * channel reaches the RTX by being copied into state.channel and asking the
 * main loop for a synchronisation.
 */

#define CH_HEADER_H 12
#define CH_HINTS_H  12
#define CH_ROW_H    20
#define CH_FIELD_H  12
#define CH_LIST_ROWS  ((CONFIG_SCREEN_HEIGHT - CH_HEADER_H - CH_HINTS_H) / CH_ROW_H)
#define CH_FIELD_ROWS ((CONFIG_SCREEN_HEIGHT - CH_HEADER_H - CH_HINTS_H) / CH_FIELD_H)

#define CH_MSG_MS 2000

enum chScreen
{
    CHS_CLOSED = 0,
    CHS_LIST,
    CHS_EDIT,
    CHS_CONFIRM
};

enum chField
{
    CF_NAME = 0,
    CF_RX,
    CF_TX,
    CF_SIMPLEX,
    CF_MODE,
    CF_BW,
    CF_POWER,
    CF_TONE_TX,
    CF_TONE_RX,
    CF_DMR,
    CF_NUM
};

static const char *const fieldLabels[CF_NUM] = {
    "NAME", "RX", "TX", "SIMPLEX", "MODE", "BW", "POWER", "TONE TX",
    "TONE RX", "DMR CC/TS"
};

/* Characters offered for the name, in the order UP/DOWN walks through them */
static const char nameChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-./+";

static enum chScreen screen = CHS_CLOSED;
static uint16_t listSel = 0;
static uint16_t listTop = 0;
static uint16_t editIdx = CHSTORE_NONE;
static chstore_channel_t edit;
static uint8_t field = CF_NAME;
static uint8_t cursor = 0;
static bool fieldEdit = false;
static char msgBuf[22] = "";
static long long msgUntil = 0;

/******************************************************************************
 *                              Small helpers                                 *
 ******************************************************************************/

static void formatFreq(char *buf, size_t len, freq_t f)
{
    snprintf(buf, len, "%lu.%04lu", (unsigned long)(f / 1000000U),
             (unsigned long)((f % 1000000U) / 100U));
}

static void formatTone(char *buf, size_t len, bool en, uint8_t idx)
{
    if (!en || idx >= CTCSS_FREQ_NUM) {
        snprintf(buf, len, "OFF");
        return;
    }

    uint16_t t = ctcss_tone[idx]; /* tenths of Hz */
    snprintf(buf, len, "%u.%u", (unsigned)(t / 10U), (unsigned)(t % 10U));
}

static void formatPower(char *buf, size_t len, uint16_t mw)
{
    snprintf(buf, len, "%u.%uW", (unsigned)(mw / 1000U),
             (unsigned)((mw % 1000U) / 100U));
}

/* Width of the first n characters of s in the given font. */
static uint16_t prefixWidth(fontSize_t font, const char *s, uint8_t n)
{
    char buf[CHSTORE_NAME_LEN + 1];
    size_t len = strlen(s);
    if (n > len)
        n = (uint8_t)len;
    if (n > CHSTORE_NAME_LEN)
        n = CHSTORE_NAME_LEN;

    memcpy(buf, s, n);
    buf[n] = '\0';
    return gfx_getTextWidth(font, buf);
}

static void setMessage(const char *text)
{
    snprintf(msgBuf, sizeof(msgBuf), "%s", text);
    msgUntil = getTick() + CH_MSG_MS;
}

static void beepOk(void)
{
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
}

static void beepError(void)
{
    vp_beep(400, SHORT_BEEP * 2);
}

static void clampList(void)
{
    uint16_t count = (uint16_t)chstore_count();

    if (count == 0) {
        listSel = 0;
        listTop = 0;
        return;
    }

    if (listSel >= count)
        listSel = count - 1;

    if (listSel < listTop)
        listTop = listSel;

    if ((uint16_t)(listSel - listTop) >= CH_LIST_ROWS)
        listTop = (uint16_t)(listSel - CH_LIST_ROWS + 1);
}

/******************************************************************************
 *                            Channel -> RTX                                  *
 ******************************************************************************/

static bool tuneChannel(uint16_t idx)
{
    channel_t ch;

    if (chstore_toChannel(idx, &ch) != 0)
        return false;

    state.channel = ch;
    state.channel_index = idx;
    chstore_setLastUsed(idx);

    return true;
}

/******************************************************************************
 *                                Editing                                     *
 ******************************************************************************/

static bool fieldVisible(uint8_t f)
{
    if (f == CF_DMR)
        return edit.mode == OPMODE_DMR;

    return true;
}

static uint8_t fieldStep(uint8_t f, int dir)
{
    for (uint8_t i = 0; i < CF_NUM; i++) {
        f = (uint8_t)((f + CF_NUM + dir) % CF_NUM);
        if (fieldVisible(f))
            return f;
    }

    return f;
}

/* Number of visible fields before f, i.e. the row f is drawn on. */
static uint8_t fieldRow(uint8_t f)
{
    uint8_t row = 0;

    for (uint8_t i = 0; i < f; i++) {
        if (fieldVisible(i))
            row++;
    }

    return row;
}

/* Move one digit of a frequency, snapping to the digit and checking the band. */
static bool freqStep(uint32_t *freq, uint8_t pos, int dir, bool coarse)
{
    static const uint32_t pow10[10] = {
        1000000000U, 100000000U, 10000000U, 1000000U, 100000U,
        10000U, 1000U, 100U, 10U, 1U
    };

    if (pos > 9)
        pos = 9;

    /* A long press moves the digit above the selected one */
    if (coarse && pos > 0)
        pos--;

    uint32_t step = pow10[pos];
    uint64_t v = *freq;

    if (dir > 0) {
        v = ((v / step) + 1) * step;
        if (v > 0xFFFFFFFFULL)
            return false;
        if (!chstore_freqValid((freq_t)v))
            return false;
        *freq = (freq_t)v;
    } else {
        if (v < step)
            return false;
        v = ((v / step) - 1) * step + (step - 1);
        if (!chstore_freqValid((freq_t)v))
            return false;
        *freq = (freq_t)v;
    }

    return true;
}

static void nameStep(int dir)
{
    size_t len = strlen(edit.name);

    if (len == 0) {
        edit.name[0] = nameChars[1];
        edit.name[1] = '\0';
        cursor = 0;
        return;
    }

    if (cursor >= CHSTORE_NAME_LEN - 1)
        cursor = CHSTORE_NAME_LEN - 2;

    if (cursor >= len) {
        /* Walk past the end: append a space and keep the cursor there */
        if (len < CHSTORE_NAME_LEN - 2 && dir > 0) {
            edit.name[len] = ' ';
            edit.name[len + 1] = '\0';
            cursor = (uint8_t)len;
        } else {
            cursor = (uint8_t)(len - 1);
        }
    }

    const char *p = (edit.name[cursor] == '\0') ? nameChars
                                                : strchr(nameChars, edit.name[cursor]);
    if (p == NULL)
        p = nameChars;

    int n = (int)strlen(nameChars);
    int i = (int)(p - nameChars);
    i = (i + dir + n) % n;
    edit.name[cursor] = nameChars[i];
}

static void toneStep(bool tx, int dir)
{
    /* The tone fields are bitfields: go through locals, no addresses */
    uint8_t en = tx ? edit.tx_tone_en : edit.rx_tone_en;
    uint8_t idx = tx ? edit.tx_tone : edit.rx_tone;

    if (dir > 0) {
        if (!en) {
            en = 1;
            idx = 0;
        } else if (idx + 1 < CTCSS_FREQ_NUM) {
            idx++;
        } else {
            en = 0;
            idx = 0;
        }
    } else {
        if (!en) {
            en = 1;
            idx = CTCSS_FREQ_NUM - 1;
        } else if (idx > 0) {
            idx--;
        } else {
            en = 0;
            idx = 0;
        }
    }

    if (tx) {
        edit.tx_tone_en = en;
        edit.tx_tone = idx;
    } else {
        edit.rx_tone_en = en;
        edit.rx_tone = idx;
    }
}

static void modeStep(int dir)
{
    static const uint8_t modes[] = { OPMODE_FM, OPMODE_M17, OPMODE_DMR,
                                     OPMODE_TETRA };
    int cur = 0;

    for (size_t i = 0; i < sizeof(modes); i++) {
        if (modes[i] == edit.mode)
            cur = (int)i;
    }

    cur = (cur + dir + (int)sizeof(modes)) % (int)sizeof(modes);
    edit.mode = modes[cur];

    if (edit.mode == OPMODE_DMR && edit.dmr_cc == 0)
        edit.dmr_cc = 1;

    /* Keep the selection on a field that is still drawn */
    if (!fieldVisible(field))
        field = CF_MODE;
}

static void powerStep(int dir)
{
    static const uint16_t powers[] = { 1000, 2500, 5000 };
    int cur = 0;

    for (size_t i = 0; i < sizeof(powers) / sizeof(powers[0]); i++) {
        if (powers[i] == edit.power)
            cur = (int)i;
    }

    cur = (cur + dir + 3) % 3;
    edit.power = powers[cur];
}

static void fieldChange(int dir, bool coarse)
{
    switch (field)
    {
        case CF_NAME:
            nameStep(dir);
            break;

        case CF_RX:
        {
            uint32_t f = edit.rx_freq;
            if (freqStep(&f, cursor, dir, coarse)) {
                edit.rx_freq = f;
                if (edit.flags & CHSTORE_FLAG_SIMPLEX)
                    edit.tx_freq = f;
            }
            break;
        }

        case CF_TX:
        {
            uint32_t f = edit.tx_freq;
            if (freqStep(&f, cursor, dir, coarse)) {
                /* Touching TX means the channel is no longer simplex */
                edit.tx_freq = f;
                edit.flags &= (uint8_t)~CHSTORE_FLAG_SIMPLEX;
            }
            break;
        }

        case CF_SIMPLEX:
            if (edit.flags & CHSTORE_FLAG_SIMPLEX) {
                edit.flags &= (uint8_t)~CHSTORE_FLAG_SIMPLEX;
            } else {
                edit.flags |= CHSTORE_FLAG_SIMPLEX;
                edit.tx_freq = edit.rx_freq;
            }
            break;

        case CF_MODE:
            modeStep(dir);
            break;

        case CF_BW:
            edit.bandwidth = (edit.bandwidth == BW_25) ? BW_12_5 : BW_25;
            break;

        case CF_POWER:
            powerStep(dir);
            break;

        case CF_TONE_TX:
            toneStep(true, dir);
            break;

        case CF_TONE_RX:
            toneStep(false, dir);
            break;

        case CF_DMR:
        default:
            /* Colour code and timeslot are read only here */
            break;
    }
}

static void fieldCursorStep(int dir)
{
    if (field == CF_NAME) {
        uint8_t len = (uint8_t)strlen(edit.name);
        if (len == 0)
            return;
        if (dir > 0)
            cursor = (cursor + 1 < len) ? (uint8_t)(cursor + 1) : 0;
        else
            cursor = (cursor > 0) ? (uint8_t)(cursor - 1) : (uint8_t)(len - 1);
    } else if (field == CF_RX || field == CF_TX) {
        if (dir > 0)
            cursor = (cursor + 1) % 10;
        else
            cursor = (cursor == 0) ? 9 : (uint8_t)(cursor - 1);
    } else {
        /* Enumerations: LEFT/RIGHT also changes the value */
        fieldChange(dir, false);
    }
}

/******************************************************************************
 *                              Actions                                       *
 ******************************************************************************/

static const char *invalidReason(void)
{
    if (edit.name[0] == '\0')
        return "NAME EMPTY";

    if (!chstore_freqValid(edit.rx_freq) || !chstore_freqValid(edit.tx_freq))
        return "FREQ OUT OF RANGE";

    return "INVALID CHANNEL";
}

/* Fields common to a new and an existing channel, then show the editor. */
static void editorEnter(void)
{
    /* Never hand an impossible record to the editor */
    if (edit.bandwidth != BW_25 && edit.bandwidth != BW_12_5)
        edit.bandwidth = BW_12_5;
    if (edit.power == 0 || edit.power > 5000)
        edit.power = 1000;
    if (edit.mode == OPMODE_DMR && edit.dmr_cc == 0)
        edit.dmr_cc = 1;
    edit.name[CHSTORE_NAME_LEN - 1] = '\0';

    size_t len = strnlen(edit.name, CHSTORE_NAME_LEN);
    field = CF_NAME;
    fieldEdit = false;
    cursor = (uint8_t)((len > 0) ? len - 1 : 0);
    msgBuf[0] = '\0';
    screen = CHS_EDIT;
}

/* P2 on an existing channel: change it in place. */
static void editorEdit(uint16_t idx)
{
    if (chstore_get(idx, &edit) != 0)
        return;

    editIdx = idx;
    editorEnter();
}

/* P1: a new channel, copied from the selected one or, if the bank is empty,
 * from the frequency the radio is tuned on. */
static void editorNew(void)
{
    editIdx = CHSTORE_NONE;

    if (chstore_count() > 0 && chstore_get(listSel, &edit) == 0) {
        /* Duplicate: same parameters, name gets a hint of its own */
        beepOk();
    } else {
        chstore_fromChannel(&state.channel, &edit);
        edit.name[0] = '\0';
        edit.flags |= CHSTORE_FLAG_SIMPLEX;
        edit.dmr_cc = 1;
        edit.dmr_ts = 0;
        if (edit.mode == OPMODE_NONE)
            edit.mode = OPMODE_FM;
        if (edit.rx_freq == 0)
            edit.rx_freq = 430000000;
        edit.tx_freq = edit.rx_freq;
        if (edit.power == 0)
            edit.power = 1000;
    }

    if (edit.name[0] == '\0') {
        snprintf(edit.name, sizeof(edit.name), "CH %u",
                 (unsigned)(chstore_count() + 1));
    }

    editorEnter();
}

static void editorSave(void)
{
    if (!chstore_valid(&edit)) {
        setMessage(invalidReason());
        beepError();
        return;
    }

    int ret;
    if (editIdx == CHSTORE_NONE) {
        ret = chstore_add(&edit);
        if (ret >= 0) {
            listSel = (uint16_t)ret;
            editIdx = (uint16_t)ret;
        }
    } else {
        ret = chstore_set(editIdx, &edit);
    }

    if (ret < 0) {
        setMessage("WRITE FAILED");
        beepError();
        return;
    }

    clampList();
    beepOk();
    screen = CHS_LIST;
}

static void listDelete(void)
{
    if (chstore_delete(listSel) != 0) {
        setMessage("DELETE FAILED");
        beepError();
    } else {
        beepOk();
    }

    clampList();
    screen = CHS_LIST;
}

/******************************************************************************
 *                             Key handling                                   *
 ******************************************************************************/

static bool listKey(kbd_msg_t *msg, bool *sync_rtx)
{
    uint16_t count = (uint16_t)chstore_count();

    if (msg->keys & (KEY_UP | KNOB_RIGHT)) {
        if (count > 0) {
            listSel = (listSel + 1 < count) ? (uint16_t)(listSel + 1) : 0;
            clampList();
        }
    } else if (msg->keys & (KEY_DOWN | KNOB_LEFT)) {
        if (count > 0) {
            listSel = (listSel > 0) ? (uint16_t)(listSel - 1)
                                    : (uint16_t)(count - 1);
            clampList();
        }
    } else if (msg->keys & KEY_ENTER) {
        if (count > 0 && tuneChannel(listSel)) {
            beepOk();
            *sync_rtx = true;
            screen = CHS_CLOSED;
        }
    } else if (msg->keys & KEY_F1) {
        /* New: duplicate the selected channel, or start from the VFO */
        editorNew();
    } else if (msg->keys & KEY_F2) {
        if (count == 0)
            return true;

        if (msg->long_press)
            screen = CHS_CONFIRM;
        else
            editorEdit(listSel);
    } else if (msg->keys & KEY_ESC) {
        screen = CHS_CLOSED;
    }

    return true;
}

static bool editKey(kbd_msg_t *msg, bool *sync_rtx)
{
    (void)sync_rtx;

    if (msg->keys & (KEY_UP | KNOB_RIGHT)) {
        if (fieldEdit)
            fieldChange(1, msg->long_press);
        else
            field = fieldStep(field, 1);
    } else if (msg->keys & (KEY_DOWN | KNOB_LEFT)) {
        if (fieldEdit)
            fieldChange(-1, msg->long_press);
        else
            field = fieldStep(field, -1);
    } else if (msg->keys & (KEY_LEFT | KEY_RIGHT)) {
        fieldCursorStep((msg->keys & KEY_LEFT) ? -1 : 1);
    } else if (msg->keys & KEY_ENTER) {
        if (field == CF_SIMPLEX) {
            fieldChange(1, false);
        } else if (field == CF_DMR) {
            beepError();
        } else {
            fieldEdit = !fieldEdit;
            if (fieldEdit && (field == CF_RX || field == CF_TX)) {
                /* Start on the kHz digit, the one people change most */
                cursor = 6;
            } else if (fieldEdit && field == CF_NAME) {
                size_t len = strnlen(edit.name, CHSTORE_NAME_LEN);
                cursor = (uint8_t)((len > 0) ? len - 1 : 0);
            }
        }
    } else if (msg->keys & KEY_F2) {
        editorSave();
    } else if (msg->keys & KEY_ESC) {
        if (fieldEdit) {
            fieldEdit = false;
        } else {
            beepError();
            screen = CHS_LIST;
        }
    }

    return true;
}

bool ui_channels_handleEvent(kbd_msg_t *msg, bool *sync_rtx)
{
    switch (screen)
    {
        case CHS_LIST:
            return listKey(msg, sync_rtx);

        case CHS_EDIT:
            return editKey(msg, sync_rtx);

        case CHS_CONFIRM:
            if (msg->keys & KEY_ENTER) {
                listDelete();
            } else if (msg->keys & KEY_ESC) {
                screen = CHS_LIST;
            }
            return true;

        case CHS_CLOSED:
        default:
            return false;
    }
}

/******************************************************************************
 *                                Drawing                                     *
 ******************************************************************************/

static void drawHLine(uint16_t y)
{
    point_t p = { 0, (int16_t)y };
    gfx_drawRect(p, CONFIG_SCREEN_WIDTH, 1, color_grey, true);
}

void ui_channels_drawHints(const char *green, const char *p1, const char *p2,
                           const char *red)
{
    /*
     * HOOK: the shared hint bar of the UI branch replaces this body. It gets
     * the same four labels and has to draw them on the bottom line.
     */
    char buf[48] = "";
    size_t n = 0;

    if (green != NULL)
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "G:%s ", green);
    if (p1 != NULL && n < sizeof(buf))
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "P1:%s ", p1);
    if (p2 != NULL && n < sizeof(buf))
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "P2:%s ", p2);
    if (red != NULL && n < sizeof(buf))
        snprintf(buf + n, sizeof(buf) - n, "R:%s", red);

    point_t pos = { CONFIG_SCREEN_WIDTH / 2,
                    CONFIG_SCREEN_HEIGHT - CH_HINTS_H + 2 };
    gfx_print(pos, FONT_SIZE_5PT, TEXT_ALIGN_CENTER, color_white, "%s", buf);
}

static void drawHeader(const char *title)
{
    point_t pos = { 2, 2 };
    gfx_print(pos, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white, "%s", title);

    point_t bat = { CONFIG_SCREEN_WIDTH - 2, 2 };
    gfx_print(bat, FONT_SIZE_5PT, TEXT_ALIGN_RIGHT, color_white, "%u%%",
              (unsigned)state.charge);

    drawHLine(CH_HEADER_H - 1);
}

static void drawList(void)
{
    char title[24];
    uint16_t count = (uint16_t)chstore_count();

    snprintf(title, sizeof(title), "CHANNELS %u/%u", (unsigned)count,
             (unsigned)CHSTORE_MAX_CHANNELS);
    drawHeader(title);

    if (count == 0) {
        point_t p = { CONFIG_SCREEN_WIDTH / 2, CH_HEADER_H + 30 };
        gfx_print(p, FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white,
                  "No channels");
        p.y += 14;
        gfx_print(p, FONT_SIZE_5PT, TEXT_ALIGN_CENTER, color_white,
                  "P1 adds the VFO");
        ui_channels_drawHints("TUNE", "ADD", NULL, "EXIT");
        return;
    }

    for (uint8_t row = 0; row < CH_LIST_ROWS; row++) {
        uint16_t idx = (uint16_t)(listTop + row);
        if (idx >= count)
            break;

        chstore_channel_t ch;
        if (chstore_get(idx, &ch) != 0)
            break;

        uint16_t y = (uint16_t)(CH_HEADER_H + row * CH_ROW_H);
        bool selected = (idx == listSel);

        if (selected) {
            point_t rect = { 0, (int16_t)y };
            gfx_drawRect(rect, CONFIG_SCREEN_WIDTH, CH_ROW_H, color_white,
                         true);
        }

        color_t fg = selected ? color_black : color_white;

        point_t l1 = { 2, (int16_t)(y + 1) };
        gfx_print(l1, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, fg, "%02u %s",
                  (unsigned)(idx + 1), ch.name);

        char freq[16];
        formatFreq(freq, sizeof(freq), ch.rx_freq);

        char tone[8] = "";
        if (ch.tx_tone_en)
            snprintf(tone, sizeof(tone), " T");

        point_t l2 = { 2, (int16_t)(y + 11) };
        gfx_print(l2, FONT_SIZE_5PT, TEXT_ALIGN_LEFT, fg, "%s %s%s%s", freq,
                  chstore_modeName(ch.mode),
                  (ch.flags & CHSTORE_FLAG_RX_ONLY) ? " RX" : tone,
                  (ch.flags & CHSTORE_FLAG_SIMPLEX) ? " S" : "");
    }

    if (msgBuf[0] != '\0' && getTick() < msgUntil)
        ui_channels_drawHints(msgBuf, NULL, NULL, NULL);
    else
        ui_channels_drawHints("TUNE", "NEW", "EDIT", "EXIT");
}

static void fieldValue(uint8_t f, char *buf, size_t len)
{
    switch (f)
    {
        case CF_NAME:
            snprintf(buf, len, "%s", edit.name);
            break;

        case CF_RX:
            formatFreq(buf, len, edit.rx_freq);
            break;

        case CF_TX:
            formatFreq(buf, len, edit.tx_freq);
            break;

        case CF_SIMPLEX:
            snprintf(buf, len, "%s", (edit.flags & CHSTORE_FLAG_SIMPLEX)
                                     ? "ON" : "OFF");
            break;

        case CF_MODE:
            snprintf(buf, len, "%s", chstore_modeName(edit.mode));
            break;

        case CF_BW:
            snprintf(buf, len, "%s", (edit.bandwidth == BW_12_5)
                                     ? "12.5" : "25");
            break;

        case CF_POWER:
            formatPower(buf, len, edit.power);
            break;

        case CF_TONE_TX:
            formatTone(buf, len, edit.tx_tone_en, edit.tx_tone);
            break;

        case CF_TONE_RX:
            formatTone(buf, len, edit.rx_tone_en, edit.rx_tone);
            break;

        case CF_DMR:
            snprintf(buf, len, "CC%u TS%u", (unsigned)(edit.dmr_cc & 0x0F),
                     (unsigned)(edit.dmr_ts ? 2 : 1));
            break;

        default:
            buf[0] = '\0';
            break;
    }
}

static void drawEditor(void)
{
    char title[24];
    char value[20];
    char label[12];

    if (editIdx == CHSTORE_NONE)
        snprintf(title, sizeof(title), "NEW CHANNEL");
    else
        snprintf(title, sizeof(title), "EDIT %u/%u", (unsigned)(editIdx + 1),
                 (unsigned)chstore_count());

    drawHeader(title);

    uint8_t firstRow = fieldRow(field);
    uint8_t scroll = 0;
    if (firstRow >= CH_FIELD_ROWS)
        scroll = (uint8_t)(firstRow - CH_FIELD_ROWS + 1);

    uint8_t row = 0;
    for (uint8_t f = 0; f < CF_NUM; f++) {
        if (!fieldVisible(f))
            continue;

        if (row < scroll) {
            row++;
            continue;
        }

        uint16_t y = (uint16_t)(CH_HEADER_H + (row - scroll) * CH_FIELD_H);
        bool selected = (f == field);
        color_t fg = color_white;
        bool full = true;

        if (selected) {
            point_t rect = { 0, (int16_t)y };
            full = !fieldEdit;
            fg = fieldEdit ? color_white : color_black;
            gfx_drawRect(rect, CONFIG_SCREEN_WIDTH, CH_FIELD_H, color_white,
                         full);
        }

        snprintf(label, sizeof(label), "%s", fieldLabels[f]);
        point_t lp = { 2, (int16_t)(y + 2) };
        gfx_print(lp, FONT_SIZE_5PT, TEXT_ALIGN_LEFT, fg, "%s", label);

        fieldValue(f, value, sizeof(value));

        point_t vp = { 47, (int16_t)(y + 2) };
        gfx_print(vp, FONT_SIZE_5PT, TEXT_ALIGN_LEFT, fg, "%s", value);

        /* Underline the character being edited */
        if (selected && fieldEdit && (f == CF_NAME || f == CF_RX || f == CF_TX)) {
            uint16_t x0 = (uint16_t)(vp.x + prefixWidth(FONT_SIZE_5PT, value,
                                                        cursor));
            uint16_t x1 = (uint16_t)(vp.x + prefixWidth(FONT_SIZE_5PT, value,
                                                        (uint8_t)(cursor + 1)));
            point_t upos = { (int16_t)x0, (int16_t)(y + CH_FIELD_H - 3) };
            gfx_drawRect(upos, (uint16_t)((x1 > x0) ? (x1 - x0) : 5), 1,
                         color_white, true);
        }

        row++;
    }

    drawHLine(CH_HEADER_H + (CH_FIELD_ROWS * CH_FIELD_H) + 1);

    if (msgBuf[0] != '\0' && getTick() < msgUntil)
        ui_channels_drawHints(msgBuf, NULL, NULL, "BACK");
    else
        ui_channels_drawHints(fieldEdit ? "OK" : "EDIT", NULL, "SAVE",
                              fieldEdit ? "BACK" : "CANCEL");
}

static void drawConfirm(void)
{
    drawHeader("DELETE");

    chstore_channel_t ch;
    point_t p = { CONFIG_SCREEN_WIDTH / 2, CH_HEADER_H + 26 };

    if (chstore_get(listSel, &ch) == 0)
        gfx_print(p, FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white,
                  "Delete %u?", (unsigned)(listSel + 1));
    else
        gfx_print(p, FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white, "Delete?");

    p.y += 16;
    if (chstore_get(listSel, &ch) == 0)
        gfx_print(p, FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white, "%.15s",
                  ch.name);

    ui_channels_drawHints("YES", NULL, NULL, "NO");
}

void ui_channels_draw(void)
{
    gfx_clearScreen();

    switch (screen)
    {
        case CHS_LIST:
            drawList();
            break;

        case CHS_EDIT:
            drawEditor();
            break;

        case CHS_CONFIRM:
            drawConfirm();
            break;

        default:
            break;
    }
}

void ui_channels_open(void)
{
    if (chstore_init() != 0)
        return;

    screen = CHS_LIST;
    msgBuf[0] = '\0';
    fieldEdit = false;

    int last = chstore_lastUsed();
    listSel = (last >= 0) ? (uint16_t)last : 0;
    listTop = 0;
    clampList();
}

bool ui_channels_isOpen(void)
{
    return screen != CHS_CLOSED;
}
