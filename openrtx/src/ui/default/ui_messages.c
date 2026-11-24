/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <hwconfig.h>
#include "core/graphics.h"
#include "core/state.h"
#include "core/messaging.h"
#include "core/msg_service.h"
#include "core/beeps.h"
#include "core/voicePrompts.h"
#include "interfaces/delays.h"
#include "ui/ui_default.h"
#include "ui/ui_hints.h"
#include "ui/ui_messages.h"
#include "ui/ui_layout.h"
#include "ui/ui_contacts.h"

/*
 * Messaging UI. See docs/UI.md for the key table.
 *
 * Nothing here keeps messages of its own: the inbox, the sent folder, the
 * delivery states and the draft are the shared store (core/messaging.h), and
 * msg_service translates a recipient into an address.  A screen is therefore
 * always showing what the transports are really doing.
 *
 * Inbox / sent: UP/DOWN move, green opens (or the status page), P1 switches
 * inbox<->sent (P2 replies from the inbox), red goes back.
 *
 * Compose has four rows: TP (transport), TO (recipient), TXT (text), SEND.
 * UP/DOWN move between rows. On TP and TO, P1/P2 change the value; on the
 * text row, green starts multi-tap typing: digits type, P1 is backspace, P2
 * is space, green ends typing and red leaves the row. Green on SEND queues it.
 *
 * The draft lives in the message store, so it survives a power cycle with the
 * same two-sector discipline as the queues.
 */

#define MG_HEADER_H UI_HEADER_H
#define MG_ROW_H    UI_ROW_H
#define MG_ROWS     ((CONFIG_SCREEN_HEIGHT - UI_LIST_TOP - UI_HINTS_H) / MG_ROW_H)
#define MG_MSG_MS   2000
#define MG_TAP_MS   1200   /* multi-tap window                          */
#define MG_POLL_MS  500
#define MG_DRAFT_MS 3000   /* idle time before the draft reaches flash  */

/* Multi-tap groups, indexed by the digit key (0..9) */
static const char *const tapMap[10] = {
    " 0-/_@",
    ".,?!1",
    "ABC2",
    "DEF3",
    "GHI4",
    "JKL5",
    "MNO6",
    "PQRS7",
    "TUV8",
    "WXYZ9"
};

enum mgRow
{
    MG_ROW_TP = 0,
    MG_ROW_TO,
    MG_ROW_TEXT,
    MG_ROW_SEND,
    MG_ROW_NUM
};

static struct
{
    uint8_t sel;          /**< cursor in the list screens                 */
    uint8_t row;          /**< cursor in the composer                     */
    bool typing;          /**< composing: the text row takes the digits   */
    uint8_t tp;           /**< chosen transport, enum msgTransport        */
    uint8_t id;           /**< message on the view/status screen          */
    char peer[MSG_PEER_LEN];
    char text[MSG_TEXT_MAX + 1];
    uint8_t pos;
    uint8_t tapKey;       /**< 0..9 while a multi-tap burst is open       */
    uint8_t tapIdx;
    uint8_t tapPos;       /**< where the current tap character sits       */
    long long tapTick;
    long long lastEdit;
    long long lastPoll;
    bool draftTouched;    /**< the store still has to be told we edited   */
    char msg[22];
    long long msgUntil;
} mg;

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
    snprintf(mg.msg, sizeof(mg.msg), "%s", text);
    mg.msgUntil = getTick() + MG_MSG_MS;
}

static const char *tpName(uint8_t tp)
{
    return (tp < MSG_TRANSPORT_COUNT) ? msg_service_name((enum msgTransport)tp)
                                      : "-";
}

/* The name a message shows: the peer as typed, or the address it came from */
static void peerText(const struct message *m, char *buf, size_t len)
{
    if (m->peer[0] != '\0') {
        snprintf(buf, len, "%s", m->peer);
        return;
    }
    if ((m->flags & MSG_FLAG_INBOUND) != 0)
        snprintf(buf, len, "%u", (unsigned)m->src);
    else
        snprintf(buf, len, "%u", (unsigned)m->dst);
}

/* A message by id, in the inbox or in the outbox.  NULL when it is gone. */
static const struct message *findMsg(uint8_t id)
{
    for (int i = 0; i < MSG_INBOX_LEN; i++) {
        const struct message *m = messaging_inbox(i);

        if (m == NULL)
            break;
        if (m->id == id)
            return m;
    }
    return messaging_find(id);
}

/* ------------------------------------------------------------------------ */
/* Draft                                                                    */
/* ------------------------------------------------------------------------ */

static void draftLoad(void)
{
    struct message *d = messaging_draft();

    if ((d->text[0] == '\0') && (d->peer[0] == '\0'))
        return;

    mg.tp = (d->transport < MSG_TRANSPORT_COUNT) ? d->transport
                                                 : msg_service_default();
    snprintf(mg.peer, sizeof(mg.peer), "%s", d->peer);
    snprintf(mg.text, sizeof(mg.text), "%s", d->text);
    mg.pos = (uint8_t)strlen(mg.text);
}

static void draftStore(void)
{
    struct message *d = messaging_draft();

    snprintf(d->peer, sizeof(d->peer), "%s", mg.peer);
    snprintf(d->text, sizeof(d->text), "%s", mg.text);
    d->transport = mg.tp;
    messaging_draftTouch();
    mg.draftTouched = false;
}

static void draftDrop(void)
{
    memset(mg.peer, 0, sizeof(mg.peer));
    memset(mg.text, 0, sizeof(mg.text));
    mg.pos = 0;
    mg.tapKey = 0xFF;
    mg.draftTouched = false;
    messaging_draftClear();
}

/* ------------------------------------------------------------------------ */
/* Text entry                                                               */
/* ------------------------------------------------------------------------ */

static uint8_t textMax(void)
{
    /* With no transport available the draft still has to be writable: fall
       back to the roomiest limit of any transport, not to zero. */
    uint8_t m = (mg.tp < MSG_TRANSPORT_COUNT)
                    ? msg_service_maxLen((enum msgTransport)mg.tp)
                    : MSG_TEXT_MAX;
    if (m == 0 || m > MSG_TEXT_MAX)
        m = MSG_TEXT_MAX;
    return m;
}

/*
 * The composer types into the field under the cursor: the recipient (TO) or
 * the message body (TXT). Both are plain C strings, but the recipient has its
 * own limit and character set.
 */
static char *activeBuf(void)
{
    return (mg.row == MG_ROW_TO) ? mg.peer : mg.text;
}

static uint8_t activeMax(void)
{
    return (mg.row == MG_ROW_TO) ? (MSG_PEER_LEN - 1) : textMax();
}

static bool peerChar(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '/' || c == '_';
}

static void textChanged(void)
{
    mg.pos = (uint8_t)strlen(activeBuf());
    mg.lastEdit = getTick();
    mg.draftTouched = true;
}

static void textAppend(char c)
{
    char *buf = activeBuf();
    size_t len = strlen(buf);

    if (len >= (size_t)activeMax())
        return;

    if (buf == mg.peer) {
        c = (char)toupper((unsigned char)c);
        if (!peerChar(c))
            return;               /* a callsign has no spaces nor commas */
    }

    buf[len] = c;
    buf[len + 1] = '\0';
    textChanged();
}

static void textBackspace(void)
{
    char *buf = activeBuf();
    size_t len = strlen(buf);

    if (len == 0)
        return;
    buf[len - 1] = '\0';
    mg.tapKey = 0xFF;
    textChanged();
}

/* One press of a digit key: append, or cycle the character already there. */
static void tapKeyPress(uint8_t digit, bool literal, long long now)
{
    const char *group = tapMap[digit % 10];
    int n = (int)strlen(group);

    if (literal) {
        char c = (char)('0' + digit % 10);
        if (digit == 0)
            c = '0';
        /* A long press inserts the digit itself, not the letter group */
        textAppend(c);
        mg.tapKey = 0xFF;
        return;
    }

    char *buf = activeBuf();
    size_t len = strlen(buf);
    bool continueBurst = (mg.tapKey == digit)
                      && ((now - mg.tapTick) < MG_TAP_MS)
                      && ((size_t)mg.tapPos < len)
                      && ((size_t)mg.tapPos + 1 == len);

    if (continueBurst) {
        mg.tapIdx = (uint8_t)((mg.tapIdx + 1) % n);
        buf[mg.tapPos] = group[mg.tapIdx];
        mg.pos = mg.tapPos + 1;
        mg.lastEdit = now;
        mg.draftTouched = true;
    } else {
        if (len >= (size_t)activeMax())
            return;
        mg.tapKey = digit;
        mg.tapIdx = 0;
        mg.tapPos = (uint8_t)len;
        buf[len] = group[0];
        buf[len + 1] = '\0';
        textChanged();
    }

    mg.tapTick = now;
}

/* ------------------------------------------------------------------------ */
/* List helpers                                                             */
/* ------------------------------------------------------------------------ */

/*
 * The inbox is newest first; the outbox is stored oldest first, so the sent
 * folder walks it backwards to show the last message at the top.
 */
static uint8_t listCount(bool sent)
{
    if (!sent) {
        uint8_t n = 0;

        while ((n < MSG_INBOX_LEN) && (messaging_inbox(n) != NULL))
            n++;
        return n;
    }

    int n = messaging_outCount();

    return (n > MSG_OUTBOX_LEN) ? (uint8_t)MSG_OUTBOX_LEN : (uint8_t)n;
}

static const struct message *listAt(bool sent, uint8_t n)
{
    if (!sent)
        return messaging_inbox(n);
    return messaging_outbox(messaging_outCount() - 1 - (int)n);
}

static void clampSel(bool sent)
{
    uint8_t n = listCount(sent);
    if (n == 0)
        mg.sel = 0;
    else if (mg.sel >= n)
        mg.sel = (uint8_t)(n - 1);
}

/* ------------------------------------------------------------------------ */
/* Compose actions                                                          */
/* ------------------------------------------------------------------------ */

static void composeEnter(const char *peer)
{
    draftLoad();

    if (peer != NULL) {
        snprintf(mg.peer, sizeof(mg.peer), "%s", peer);
        mg.draftTouched = true;
    }

    if ((mg.tp >= MSG_TRANSPORT_COUNT)
        || !msg_service_available((enum msgTransport)mg.tp))
        mg.tp = (uint8_t)msg_service_default();

    mg.row = MG_ROW_TO;
    mg.typing = false;
    mg.tapKey = 0xFF;
    mg.msg[0] = '\0';
    state.ui_screen = MSG_COMPOSE;
}

static void cycleTransport(int dir)
{
    if (msg_service_default() == MSG_TRANSPORT_COUNT) {
        beepError();
        setMessage("NO LINK");
        return;
    }

    uint8_t tp = mg.tp;
    for (uint8_t i = 0; i < MSG_TRANSPORT_COUNT; i++) {
        tp = (uint8_t)((tp + MSG_TRANSPORT_COUNT + dir) % MSG_TRANSPORT_COUNT);
        if (msg_service_available((enum msgTransport)tp)) {
            mg.tp = tp;
            mg.draftTouched = true;
            beepOk();
            return;
        }
    }
    beepError();
}

/* Recipient from the contact book, walking forward through it. */
static void cyclePeer(int dir)
{
    uint16_t n = ui_contacts_count();
    if (n == 0) {
        beepError();
        return;
    }

    char cur[MSG_PEER_LEN];
    snprintf(cur, sizeof(cur), "%s", mg.peer);
    uint16_t idx = ui_contacts_find(cur);
    if (idx >= n) {
        idx = (dir > 0) ? 0 : (uint16_t)(n - 1);
    } else {
        idx = (uint16_t)((idx + n + dir) % n);
    }

    char call[MSG_PEER_LEN];
    if (ui_contacts_callAt(idx, call, sizeof(call))) {
        snprintf(mg.peer, sizeof(mg.peer), "%s", call);
        mg.draftTouched = true;
    }
}

static bool peerValid(void)
{
    size_t len = strlen(mg.peer);
    if (len == 0 || len >= MSG_PEER_LEN)
        return false;

    for (size_t i = 0; i < len; i++) {
        char c = mg.peer[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '/')
            return false;
    }
    return true;
}

static void composeSend(void)
{
    if (msg_service_default() == MSG_TRANSPORT_COUNT
        || !msg_service_available((enum msgTransport)mg.tp)) {
        beepError();
        setMessage("NO LINK");
        return;
    }
    if (!peerValid()) {
        beepError();
        setMessage("BAD CALL");
        return;
    }
    if (mg.text[0] == '\0') {
        beepError();
        setMessage("EMPTY");
        return;
    }
    if (strlen(mg.text) > textMax()) {
        beepError();
        setMessage("TOO LONG");
        return;
    }

    int id = msg_service_queue((enum msgTransport)mg.tp, mg.peer, mg.text,
                               true, (uint32_t)getTick());
    if (id < 0) {
        beepError();
        setMessage("SEND FAILED");
        return;
    }

    mg.id = (uint8_t)id;
    mg.typing = false;
    draftDrop();
    beepOk();
    state.ui_screen = MSG_STATUS;
}

static void reply(void)
{
    const struct message *r = listAt(false, mg.sel);
    if (r == NULL)
        return;

    char peer[MSG_PEER_LEN];
    peerText(r, peer, sizeof(peer));
    composeEnter(peer);
}

/* ------------------------------------------------------------------------ */
/* Key handling                                                             */
/* ------------------------------------------------------------------------ */

static bool inboxKey(kbd_msg_t *msg, bool sent)
{
    uint8_t n = listCount(sent);

    if (msg->keys & KEY_UP) {
        if (n > 0)
            mg.sel = (mg.sel > 0) ? (uint8_t)(mg.sel - 1) : (uint8_t)(n - 1);
    } else if (msg->keys & KEY_DOWN) {
        if (n > 0)
            mg.sel = (mg.sel + 1 < n) ? (uint8_t)(mg.sel + 1) : 0;
    } else if (msg->keys & KEY_ENTER) {
        const struct message *r = listAt(sent, mg.sel);
        if (r != NULL) {
            mg.id = r->id;
            if (!sent && ((r->flags & MSG_FLAG_UNREAD) != 0))
                messaging_markReadId((int)r->id);
            state.ui_screen = MSG_VIEW;
        }
    } else if (msg->keys & KEY_F1) {
        /* Switch folder */
        mg.sel = 0;
        state.ui_screen = sent ? MENU_MESSAGES : MSG_SENT;
    } else if (msg->keys & KEY_F2) {
        if (!sent) {
            reply();
        } else {
            const struct message *r = listAt(true, mg.sel);
            if ((r != NULL) && msg_service_cancel((int)r->id,
                                                 (uint32_t)getTick())) {
                beepOk();
                mg.id = r->id;
                state.ui_screen = MSG_STATUS;
            } else {
                beepError();
            }
        }
    } else if (msg->keys & KEY_ESC) {
        state.ui_screen = sent ? MENU_MESSAGES : MENU_TOP;
    }

    return true;
}

static bool viewKey(kbd_msg_t *msg)
{
    if (msg->keys & KEY_ENTER) {
        reply();
    } else if (msg->keys & KEY_ESC) {
        state.ui_screen = MENU_MESSAGES;
    } else if ((msg->keys & KEY_F1) && (findMsg(mg.id) != NULL)) {
        msg_service_cancel((int)mg.id, (uint32_t)getTick());
        state.ui_screen = MSG_SENT;
    }

    return true;
}

static bool composeKey(kbd_msg_t *msg, long long now)
{
    uint8_t digit = 0xFF;
    for (uint8_t d = 0; d <= 9; d++) {
        if (msg->keys & (1u << d)) {
            digit = d;
            break;
        }
    }

    if (mg.typing) {
        if (digit != 0xFF) {
            tapKeyPress(digit, msg->long_press, now);
            return true;
        }
        if (msg->keys & KEY_F1) {
            textBackspace();
            return true;
        }
        if (msg->keys & KEY_F2) {
            textAppend(' ');
            return true;
        }
        if (msg->keys & KEY_ENTER) {
            mg.typing = false;
            if (mg.row == MG_ROW_TO)
                mg.row = MG_ROW_TEXT;
            if (mg.draftTouched)
                draftStore();
            beepOk();
            return true;
        }
        if (msg->keys & KEY_ESC) {
            mg.typing = false;
            if (mg.draftTouched)
                draftStore();
            return true;
        }
        return true; /* the text row swallows everything else */
    }

    if (msg->keys & KEY_UP) {
        mg.row = (uint8_t)((mg.row + MG_ROW_NUM - 1) % MG_ROW_NUM);
    } else if (msg->keys & KEY_DOWN) {
        mg.row = (uint8_t)((mg.row + 1) % MG_ROW_NUM);
    } else if (msg->keys & KEY_F1) {
        if (mg.row == MG_ROW_TP)
            cycleTransport(-1);
        else if (mg.row == MG_ROW_TO)
            cyclePeer(-1);
        else if (mg.row == MG_ROW_TEXT && strlen(mg.text) > 0) {
            mg.text[strlen(mg.text) - 1] = '\0';
            textChanged();
        }
    } else if (msg->keys & KEY_F2) {
        if (mg.row == MG_ROW_TP)
            cycleTransport(+1);
        else if (mg.row == MG_ROW_TO)
            cyclePeer(+1);
    } else if (msg->keys & KEY_ENTER) {
        switch (mg.row) {
            case MG_ROW_TP:
                cycleTransport(+1);
                break;
            case MG_ROW_TO:
                mg.typing = true;
                mg.tapKey = 0xFF;
                break;
            case MG_ROW_TEXT:
                mg.typing = true;
                mg.tapKey = 0xFF;
                mg.tapPos = (uint8_t)strlen(mg.text);
                break;
            case MG_ROW_SEND:
            default:
                composeSend();
                break;
        }
    } else if (msg->keys & KEY_ESC) {
        if (mg.draftTouched)
            draftStore();
        state.ui_screen = MENU_MESSAGES;
    }

    return true;
}

static bool statusKey(kbd_msg_t *msg)
{
    if (msg->keys & KEY_F2) {
        if (msg_service_cancel((int)mg.id, (uint32_t)getTick())) {
            beepOk();
        } else {
            beepError();
        }
    } else if (msg->keys & (KEY_ENTER | KEY_ESC)) {
        state.ui_screen = MSG_SENT;
    }

    return true;
}

bool ui_msgs_handleEvent(kbd_msg_t *msg, bool *sync_rtx)
{
    (void)sync_rtx;
    long long now = getTick();

    switch (state.ui_screen) {
        case MENU_MESSAGES: return inboxKey(msg, false);
        case MSG_SENT:      return inboxKey(msg, true);
        case MSG_VIEW:      return viewKey(msg);
        case MSG_COMPOSE:   return composeKey(msg, now);
        case MSG_STATUS:    return statusKey(msg);
        default:            return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                  */
/* ------------------------------------------------------------------------ */

static void drawHeader(const char *title, const char *right)
{
    ui_list_header(title, (right != NULL && right[0] != '\0') ? right : NULL);
}

/* Wrap the text into `width` characters per line, `max` lines. */
static uint8_t textLines(const char *text, char lines[][40], uint8_t max,
                         uint8_t width)
{
    uint8_t n = 0;
    size_t len = strlen(text);
    size_t pos = 0;

    while (pos < len && n < max) {
        size_t take = len - pos;
        if (take > width)
            take = width;
        memcpy(lines[n], text + pos, take);
        lines[n][take] = '\0';
        pos += take;
        n++;
    }
    return n;
}

static void drawFolder(bool sent)
{
    char right[20];
    uint8_t n = listCount(sent);

    snprintf(right, sizeof(right), "%u%s", (unsigned)n,
             messaging_unread() ? " !" : "");
    drawHeader(sent ? "SENT" : "INBOX", right);

    if (n == 0) {
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2, UI_LIST_TOP + 24 },
                  FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white,
                  sent ? "Nothing sent" : "No messages");
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2, UI_LIST_TOP + 40 },
                  FONT_SIZE_5PT, TEXT_ALIGN_CENTER, color_white,
                  "P1 switches folder");
        ui_hints_draw(&(ui_hints_t){ sent ? "STATUS" : "OPEN", "INBOX",
                                      sent ? "CANC" : "REPLY", "BACK" });
        return;
    }

    for (uint8_t row = 0; row < MG_ROWS; row++) {
        if (row >= n)
            break;

        const struct message *r = listAt(sent, row);
        if (r == NULL)
            break;

        int16_t y = (int16_t)(UI_LIST_TOP + row * MG_ROW_H);
        int16_t baseline = ui_row_baseline_at(y, MG_ROW_H);
        bool selected = (row == mg.sel);

        if (selected)
            ui_row_selection(y, MG_ROW_H, true);

        color_t fg = selected ? color_black : color_white;
        char who[MSG_PEER_LEN + 2];
        char peer[MSG_PEER_LEN];

        peerText(r, peer, sizeof(peer));
        snprintf(who, sizeof(who), "%s%s",
                 (r->flags & MSG_FLAG_UNREAD) ? "*" : "", peer);
        gfx_print((point_t){ UI_TEXT_X, baseline }, FONT_SIZE_6PT,
                  TEXT_ALIGN_LEFT, fg, "%s", who);
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH - UI_TEXT_X, baseline },
                  FONT_SIZE_5PT, TEXT_ALIGN_RIGHT, fg, "%s",
                  sent ? msg_service_statusText(r->state) : tpName(r->transport));
    }

    ui_hints_draw(&(ui_hints_t){ sent ? "STATUS" : "OPEN", "SWAP",
                                  sent ? "CANC" : "REPLY", "BACK" });
}

static void drawView(void)
{
    const struct message *r = findMsg(mg.id);
    char lines[4][40];
    char right[16];
    char peer[MSG_PEER_LEN];

    if (r == NULL) {
        drawHeader("MESSAGE", NULL);
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2, UI_LIST_TOP + 24 },
                  FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white, "Gone");
        ui_hints_draw(&(ui_hints_t){ NULL, NULL, NULL, "BACK" });
        return;
    }

    peerText(r, peer, sizeof(peer));
    snprintf(right, sizeof(right), "%s", tpName(r->transport));
    drawHeader(peer, right);

    uint8_t n = textLines(r->text, lines, 6, 38);
    for (uint8_t i = 0; i < n; i++) {
        gfx_print((point_t){ 2, (int16_t)(UI_LIST_TOP + 4 + i * 9) },
                  FONT_SIZE_5PT, TEXT_ALIGN_LEFT, color_white, "%s", lines[i]);
    }

    ui_hints_draw(&(ui_hints_t){ "REPLY", NULL, NULL, "BACK" });
}

static void drawCompose(void)
{
    char right[20];
    char value[MSG_TEXT_MAX + 1];
    char lines[3][40];

    snprintf(right, sizeof(right), "%u/%u", (unsigned)strlen(mg.text),
             (unsigned)textMax());
    drawHeader("COMPOSE", right);

    static const char *const labels[MG_ROW_NUM] = {
        "TP", "TO", "TXT", "SEND"
    };

    uint8_t shown = 0;
    for (uint8_t row = 0; row < MG_ROW_NUM; row++) {
        bool selected = (row == mg.row);
        int16_t y = (int16_t)(UI_LIST_TOP + 2 + shown * 9);

        if (selected && !mg.typing)
            gfx_drawRect((point_t){ 0, y }, CONFIG_SCREEN_WIDTH, 9, color_white,
                         true);

        color_t fg = (selected && !mg.typing) ? color_black : color_white;
        color_t lab = (selected && mg.typing) ? yellow_fab413 : fg;

        gfx_print((point_t){ 2, (int16_t)(y + 7) }, FONT_SIZE_5PT,
                  TEXT_ALIGN_LEFT, lab, "%s", labels[row]);

        if (row == MG_ROW_TP)
            snprintf(value, sizeof(value), "%s",
                     msg_service_available((enum msgTransport)mg.tp)
                         ? tpName(mg.tp) : "NO LINK");
        else if (row == MG_ROW_TO)
            snprintf(value, sizeof(value), "%s",
                     mg.peer[0] ? mg.peer : "-");
        else if (row == MG_ROW_TEXT) {
            /* One line of 38 characters on the row itself; the rest is shown
               in the typing box or in the view screen. */
            if (mg.text[0])
                snprintf(value, sizeof(value), "%.38s", mg.text);
            else
                snprintf(value, sizeof(value), "-");
        }
        else
            snprintf(value, sizeof(value), "%s", ">");

        gfx_print((point_t){ 32, (int16_t)(y + 7) }, FONT_SIZE_5PT,
                  TEXT_ALIGN_LEFT, fg, "%s", value);
        shown++;
        if (row == MG_ROW_TEXT && mg.typing) {
            /* The text gets a box of its own: 3 lines of 38 characters */
            uint8_t n = textLines(mg.text, lines, 3, 38);
            for (uint8_t i = 0; i < n; i++) {
                int16_t ty = (int16_t)(y + 10 + i * 9);
                gfx_print((point_t){ 2, ty }, FONT_SIZE_5PT, TEXT_ALIGN_LEFT,
                          color_white, "%s", lines[i]);
                shown++;
            }
        }
    }

    if (mg.msg[0] != '\0' && getTick() < mg.msgUntil)
        ui_hints_draw(&(ui_hints_t){ mg.msg, NULL, NULL, NULL });
    else if (mg.typing)
        ui_hints_draw(&(ui_hints_t){ "DONE", "DEL", "SPACE", "EXIT" });
    else
        ui_hints_draw(&(ui_hints_t){ "OK", "-", "+", "BACK" });
}

static void drawStatus(void)
{
    const struct message *r = findMsg(mg.id);
    char right[16];
    char lines[3][40];
    char peer[MSG_PEER_LEN];

    if (r == NULL) {
        drawHeader("STATUS", NULL);
        gfx_print((point_t){ CONFIG_SCREEN_WIDTH / 2, UI_LIST_TOP + 24 },
                  FONT_SIZE_6PT, TEXT_ALIGN_CENTER, color_white, "Gone");
        ui_hints_draw(&(ui_hints_t){ NULL, NULL, NULL, "BACK" });
        return;
    }

    peerText(r, peer, sizeof(peer));
    snprintf(right, sizeof(right), "%s", tpName(r->transport));
    drawHeader(peer, right);

    gfx_print((point_t){ 2, (int16_t)(UI_LIST_TOP + 6) }, FONT_SIZE_6PT,
              TEXT_ALIGN_LEFT, yellow_fab413, "%s",
              msg_service_statusText(r->state));
    gfx_print((point_t){ 2, (int16_t)(UI_LIST_TOP + 18) }, FONT_SIZE_5PT,
              TEXT_ALIGN_LEFT, color_white, "try %u of %u",
              (unsigned)r->attempts, (unsigned)MSG_MAX_ATTEMPTS);

    uint8_t n = textLines(r->text, lines, 2, 38);
    for (uint8_t i = 0; i < n; i++) {
        gfx_print((point_t){ 2, (int16_t)(UI_LIST_TOP + 24 + i * 9) },
                  FONT_SIZE_5PT, TEXT_ALIGN_LEFT, color_white, "%s", lines[i]);
    }

    if (mg.msg[0] != '\0' && getTick() < mg.msgUntil)
        ui_hints_draw(&(ui_hints_t){ mg.msg, NULL, NULL, NULL });
    else
        ui_hints_draw(&(ui_hints_t){ "BACK", NULL, "CANCEL", "BACK" });
}

void ui_msgs_draw(uint8_t screen)
{
    switch (screen) {
        case MENU_MESSAGES: drawFolder(false); break;
        case MSG_SENT:      drawFolder(true);  break;
        case MSG_VIEW:      drawView();        break;
        case MSG_COMPOSE:   drawCompose();     break;
        case MSG_STATUS:    drawStatus();      break;
        default:            break;
    }
}

/* ------------------------------------------------------------------------ */
/* Entry points                                                             */
/* ------------------------------------------------------------------------ */

bool ui_msgs_isScreen(uint8_t screen)
{
    return (screen == MENU_MESSAGES) || (screen == MSG_VIEW)
        || (screen == MSG_SENT) || (screen == MSG_COMPOSE)
        || (screen == MSG_STATUS);
}

void ui_msgs_open(void)
{
    msg_service_init();
    mg.sel = 0;
    mg.msg[0] = '\0';
    clampSel(false);
    state.ui_screen = MENU_MESSAGES;
}

void ui_msgs_compose(const char *peer)
{
    msg_service_init();
    composeEnter(peer);
}

void ui_msgs_tick(long long now)
{
    if ((now - mg.lastPoll) >= MG_POLL_MS) {
        mg.lastPoll = now;
        msg_service_poll((uint32_t)now);
    }

    /* The store debounces the write; handing it the change is enough */
    if (mg.draftTouched && (now - mg.lastEdit) >= MG_DRAFT_MS)
        draftStore();

    /* The multi-tap burst ends by itself: the next press starts a new char */
    if (mg.tapKey <= 9 && (now - mg.tapTick) > MG_TAP_MS)
        mg.tapKey = 0xFF;
}

void ui_msgs_fmt_unread(char *buf, int len)
{
    if (buf == NULL || len <= 0)
        return;

    int n = messaging_unread();
    if (n <= 0)
        buf[0] = '\0';
    else
        snprintf(buf, len, "!%d", n);
}
