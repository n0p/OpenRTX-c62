/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tools menu (ui/ui_tools.h, docs/UI.md "Herramientas").
 *
 * P1 held down on MAIN_VFO or MAIN_MEM opens a list of five rows, each one the
 * door to something that had no key of its own:
 *
 *   Mode      the mode selector: the registry's modes, the receive-only ones
 *             marked RX, and green applies the choice and returns
 *   Scan      the memory scanner (core/scan_service.c), the same machine the
 *             Scan page of the settings configures
 *   VFO scan  the carrier search (core/vfo_search_service.c): exactly what a
 *             three second hold of UP/DOWN starts, without the hold
 *   Spectrum  the analyser's sweep (core/spectrum_service.c), opened on the
 *             frequency the radio is tuned to
 *   Channels  the channel list, which is the one thing P1 long used to do
 *
 * Geometry is the shared one (ui/ui_layout.h): 14 px header with the title at
 * the common baseline, 12 px rows with the 6 pt font on ui_row_baseline_at(),
 * and the last row of the panel for the hints. Nothing here invents a number.
 *
 * The module owns the screen while it is open: state.ui_screen is TOOLS_MENU,
 * which is not one of the two main screens, so state.txDisable is true for the
 * whole time (the PTT cannot key the radio from inside a menu) and the block in
 * ui.c claims every key before the macro grid or the mode shortcuts see it.
 * Closing restores the screen it was opened from, and _ui_syncTxDisable() of
 * ui.c re-arms the transmitter on the way out, unless the row opened a module
 * that owns the keypad itself (the channel list, the analyser).
 *
 * Every row is compiled with the thing it starts: an image without CONFIG_SCAN
 * has no "Scan" row, so no row of this menu can be a key that does nothing.
 */

#include <stdio.h>
#include <string.h>

#include "hwconfig.h"
#include "core/graphics.h"
#include "core/state.h"
#include "core/beeps.h"
#include "core/channel_store.h"
#include "core/voicePrompts.h"
#include "core/voicePromptUtils.h"
#include "interfaces/delays.h"
#include "rtx/rtx.h"
#include "ui/ui_channels.h"
#include "ui/ui_default.h"
#include "ui/ui_hints.h"
#include "ui/ui_layout.h"
#include "ui/ui_modes.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_spectrum.h"
#include "ui/ui_tools.h"

#ifdef CONFIG_SCAN
#include "core/scanner.h"
#include "core/scan_service.h"
#endif

#ifdef CONFIG_VFO_SEARCH
#include "core/vfo_search.h"
#include "core/vfo_search_service.h"
#endif

/* Rows of the panel: the list fits in one screen, so there is no scrolling
   window to keep; ui_list_clamp() still runs so that a build with more rows
   than 12 px rows would behave like every other list. */
#define T_ROWS ((CONFIG_SCREEN_HEIGHT - UI_LIST_TOP - UI_HINTS_H) / UI_ROW_H)

/* One slot per registry entry; the registry is eleven today. */
#define T_MODE_MAX 12

/* Blocks the hint bar allows: green, P1, P2, red (ui_hints.h) */
static const ui_hints_t t_hints = { "OK", "Up", "Down", "Back" };

/* What the rows of the list are. The id is the row, not the index: a build
   without the scanner has four rows, the last three of which are 2, 3 and 4. */
enum tRow
{
    TR_MODE = 0,
    TR_SCAN_MEM,
    TR_SCAN_VFO,
    TR_SPECTRUM,
    TR_CHANNELS,
    TR_PORT
};

/* The list, in the order the operator walks it. One entry per action. */
typedef struct
{
    uint8_t id;
    const char *label;
} tRow_t;

static const tRow_t rows[] =
{
    { TR_MODE,     "Mode"     },
#ifdef CONFIG_SCAN
    { TR_SCAN_MEM, "Scan"     },
#endif
#ifdef CONFIG_VFO_SEARCH
    { TR_SCAN_VFO, "VFO scan" },
#endif
    { TR_SPECTRUM, "Spectrum" },
    { TR_CHANNELS, "Channels" },
    /*
     * The accessory pad (docs/ACCESSORY.md).  One row for the whole radio, in
     * the one menu reachable from every main screen: as a row of each mode's
     * table it appeared inside every mode's settings page as well, and a
     * setting that decides whether the radio transmits does not belong in a
     * per-mode list.
     */
    { TR_PORT,     "Puerto"   }
};

#define T_COUNT ((uint8_t)(sizeof(rows) / sizeof(rows[0])))

enum tScreen
{
    TS_CLOSED = 0,
    TS_LIST,
    TS_MODES
};

static struct
{
    enum tScreen screen;
    uint8_t backScreen;   /**< MAIN_VFO or MAIN_MEM, where red goes back to */
    uint8_t sel;          /**< Cursor of the list                          */
    uint8_t first;        /**< First visible row of the list               */
    uint8_t msel;         /**< Cursor of the mode submenu                  */
    uint8_t mfirst;       /**< First visible row of the mode submenu       */
    uint8_t modes;        /**< Entries of the mode submenu                 */
    uint8_t modeId[T_MODE_MAX];
} ts;

/******************************************************************************
 *                              Small helpers                                 *
 ******************************************************************************/

static void beepOk(void)
{
    vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
}

/* Only the two rows that a service can refuse use it: a build with neither the
   scanner nor the search compiles it out instead of warning about it */
#if defined(CONFIG_SCAN) || defined(CONFIG_VFO_SEARCH)
static void beepError(void)
{
    vp_beep(400, SHORT_BEEP * 2);
}
#endif

/** True when the registry entry belongs in the operator's mode list. */
static bool modeSelectable(const ui_mode_t *m)
{
    return (m != NULL) && ((m->flags & UI_MODE_SELECT) != 0);
}

/**
 * Fill the mode submenu from the registry, in its order.
 *
 * The registry is the only list of modes in the UI (ui/ui_modes.h): the side
 * key walks the same entries, so a mode that is compiled in appears in both
 * places or in neither, and a mode compiled out cannot be chosen here.
 */
static void buildModes(void)
{
    ts.modes = 0;

    for (uint8_t i = 0; i < ui_mode_count() && ts.modes < T_MODE_MAX; i++) {
        const ui_mode_t *m = ui_mode_at(i);

        if (modeSelectable(m))
            ts.modeId[ts.modes++] = m->mode;
    }
}

/** Cursor of the mode submenu on the mode in use, so it opens on it. */
static void modesSelectCurrent(void)
{
    uint16_t sel = 0;
    uint16_t first = 0;

    for (uint8_t i = 0; i < ts.modes; i++) {
        if (ts.modeId[i] == state.channel.mode) {
            sel = i;
            break;
        }
    }

    ui_list_clamp(&sel, &first, ts.modes, (uint8_t)T_ROWS);
    ts.msel   = (uint8_t)sel;
    ts.mfirst = (uint8_t)first;
}

#ifdef CONFIG_VFO_SEARCH
/**
 * Give the VFO screen back, the way red on MAIN_MEM does.
 *
 * The carrier search only starts on the VFO screen (vfo_search_service.c
 * refuses anywhere else) and it sweeps the VFO's frequency, so the row does
 * what the red key does -- restore the VFO channel the memory was loaded from
 * and go back to the main VFO screen -- and then sweeps from there. Without
 * this the row would be a no-op on the memory screen.
 */
static void toVfoScreen(bool *sync_rtx)
{
    if (state.ui_screen != MAIN_MEM)
        return;

    state.channel = state.vfo_channel;
    state.ui_screen = MAIN_VFO;
    *sync_rtx = true;
}
#endif /* CONFIG_VFO_SEARCH */

/** Leave the menu for the screen it was opened from. */
static void closeMenu(bool *sync_rtx)
{
    ts.screen = TS_CLOSED;
    state.ui_screen = (ts.backScreen == MAIN_MEM) ? MAIN_MEM : MAIN_VFO;

    /* txDisable is decided by _ui_syncTxDisable() in ui.c right after this
       call: the screen is a main one again and nothing owns the keypad, so the
       transmitter comes back armed exactly as it was before P1 long. */
    if (sync_rtx != NULL)
        *sync_rtx = true;
}

/** Apply a mode chosen in the submenu, the way the side key does. */
static void applyMode(uint8_t id, bool *sync_rtx)
{
#ifdef CONFIG_SCAN
    /* A running scan retunes the radio at every step: it stops before the
       operator's change, as docs/SCAN.md requires of any manual change */
    scan_service_stop();
#endif

    state.channel.mode = id;

    /* A receive-only mode takes one frequency: no stale TX frequency is left
       behind for anything downstream to pick up */
    if (!opmode_canTransmit(id))
        state.channel.tx_frequency = state.channel.rx_frequency;

    /* Airband brings its own raster with it (see ui_channels.c) */
    qm_am_applyRaster();

    *sync_rtx = true;
    vp_announceRadioMode(id, vp_getVoiceLevelQueueFlags());
}

/******************************************************************************
 *                              Row values                                    *
 ******************************************************************************/

/** Right-hand value of a row of the list. "" when the row has nothing to say. */
static const char *listValue(uint8_t id, char *buf, size_t len)
{
    switch (id) {
        case TR_MODE:
            /* The mode in use: the row says what green would change and from
               what, instead of leaving the operator to guess */
            return ui_mode_title(state.channel.mode);

        case TR_SCAN_MEM:
        case TR_CHANNELS:
            /* The bank the row would walk, so an empty bank is visible before
               the key is pressed (the scanner refuses it with a beep) */
            snprintf(buf, len, "%d CH", chstore_count());
            return buf;

        case TR_SCAN_VFO:
            /* The hold of UP starts it upward, and so does this row */
            return "UP";

        case TR_PORT:
            /* Two states, because those are the two the firmware has: the pad
             * is the serial port, or it is the switch. */
            return (state.settings.pc_port == (uint8_t)PORT_MODE_ALWAYS)
                       ? "SERIE" : "PTT";

        default:
            break;
    }

    return "";
}

/** Right-hand value of a row of the mode submenu: the capability. */
static const char *modeValue(uint8_t id, char *buf, size_t len)
{
    (void)buf;
    (void)len;

    /* A receive-only mode says so, so that choosing it cannot mislead */
    return ui_mode_canTx(id) ? "TX" : "RX";
}

/******************************************************************************
 *                              Actions                                       *
 ******************************************************************************/

/** Green on a row of the list. */
static void activate(uint8_t index, bool *sync_rtx)
{
    if (index >= T_COUNT)
        return;

    switch (rows[index].id) {
        case TR_PORT:
            /* Green flips it and stays here: the row says what it did. */
            state.settings.pc_port =
                (state.settings.pc_port == (uint8_t)PORT_MODE_ALWAYS)
                    ? (uint8_t)PORT_MODE_NEVER : (uint8_t)PORT_MODE_ALWAYS;
            beepOk();
            return;

        case TR_MODE:
            /* A submenu, not a screen of its own: the list stays open below */
            ts.screen = TS_MODES;
            modesSelectCurrent();
            beepOk();
            return;

#ifdef CONFIG_SCAN
        case TR_SCAN_MEM:
            /* The service is what the whole feature hangs from: it walks the
               bank, samples the squelch and retunes from state_task() */
            closeMenu(sync_rtx);
            if (scan_service_start(SCAN_MODE_MEMORY))
                beepOk();
            else
                beepError();      /* empty bank, already scanning, or TX */
            return;
#endif

#ifdef CONFIG_VFO_SEARCH
        case TR_SCAN_VFO:
            closeMenu(sync_rtx);
            toVfoScreen(sync_rtx);
            if (vfoSearchService_start(VFOS_UP))
                beepOk();
            else
                beepError();      /* no band, transmitting, or a scan running */
            return;
#endif

        case TR_SPECTRUM:
            closeMenu(sync_rtx);
            /* ui_open_screen() knows the analyser can refuse (a digital mode
               needs the discriminator, a stopped receiver has no RSSI) and
               leaves the screen where it was, with a beep */
            ui_open_screen(SPECTRUM, 0);
            return;

        case TR_CHANNELS:
            /* The shortcut this menu inherited: the channel list on top of the
               main screen, which owns the keypad and the transmitter itself */
            closeMenu(sync_rtx);
            ui_channels_open();
            return;

        default:
            break;
    }
}

/******************************************************************************
 *                              Drawing                                       *
 ******************************************************************************/

static void drawHeader(void)
{
    char right[12];

    if (ts.screen == TS_MODES) {
        /* The mode in use on the right: the submenu opens on it and the
           operator can always read what green would change from */
        ui_list_header("MODE", ui_mode_title(state.channel.mode));
        return;
    }

    snprintf(right, sizeof(right), "%u%%", (unsigned)state.charge);
    ui_list_header("TOOLS", right);
}

/** One row: inverted rectangle when selected, 6 pt text on its baseline. */
static void drawRow(uint8_t row, bool selected, const char *label,
                    const char *value)
{
    int16_t top  = ui_row_top(row);
    int16_t base = ui_row_baseline_at(top, ui_row_height());

    if (selected)
        ui_row_selection(top, ui_row_height(), true);

    ui_row_text(base, UI_ROW_FONT, TEXT_ALIGN_LEFT, selected, "%s", label);

    if ((value != NULL) && (value[0] != '\0'))
        ui_row_text(base, UI_ROW_FONT, TEXT_ALIGN_RIGHT, selected, "%s", value);
}

static void drawList(void)
{
    char buf[12];

    drawHeader();

    for (uint8_t row = 0; row < T_ROWS; row++) {
        uint8_t idx = (uint8_t)(ts.first + row);

        if (idx >= T_COUNT)
            break;

        drawRow(row, idx == ts.sel, rows[idx].label,
                listValue(rows[idx].id, buf, sizeof(buf)));
    }

    ui_hints_draw(&t_hints);
}

static void drawModes(void)
{
    drawHeader();

    for (uint8_t row = 0; row < T_ROWS; row++) {
        uint8_t idx = (uint8_t)(ts.mfirst + row);

        if (idx >= ts.modes)
            break;

        drawRow(row, idx == ts.msel, ui_mode_title(ts.modeId[idx]),
                modeValue(ts.modeId[idx], NULL, 0));
    }

    ui_hints_draw(&t_hints);
}

/******************************************************************************
 *                              Key handling                                  *
 ******************************************************************************/

static void moveCursor(int dir)
{
    const bool modes = (ts.screen == TS_MODES);
    const uint8_t count = modes ? ts.modes : T_COUNT;
    uint16_t sel   = modes ? ts.msel   : ts.sel;
    uint16_t first = modes ? ts.mfirst : ts.first;

    if (count == 0)
        return;

    int next = (int)sel + dir;

    if (next < 0)
        next = count - 1;
    if (next >= count)
        next = 0;

    sel = (uint16_t)next;
    ui_list_clamp(&sel, &first, count, (uint8_t)T_ROWS);

    if (modes) {
        ts.msel   = (uint8_t)sel;
        ts.mfirst = (uint8_t)first;
    }
    else {
        ts.sel   = (uint8_t)sel;
        ts.first = (uint8_t)first;
    }
}

/** Keys of the mode submenu. */
static void modesKey(kbd_msg_t *msg, bool *sync_rtx)
{
    if (msg->keys & KEY_ESC) {
        /* Back to the list, cursor where it was: the Mode row */
        ts.screen = TS_LIST;
        return;
    }

    if (msg->keys & (KEY_UP | KEY_F1)) {
        moveCursor(-1);
    }
    else if (msg->keys & (KEY_DOWN | KEY_F2)) {
        moveCursor(+1);
    }
    else if (msg->keys & KEY_ENTER) {
        if (ts.msel < ts.modes) {
            uint8_t id = ts.modeId[ts.msel];

            /* The operator asked for this mode and gets it: apply, then out
               to the main screen, the way a channel is tuned */
            closeMenu(sync_rtx);
            applyMode(id, sync_rtx);
            beepOk();
        }
    }
}

/** Keys of the list. */
static void listKey(kbd_msg_t *msg, bool *sync_rtx)
{
    if (msg->keys & KEY_ESC) {
        closeMenu(sync_rtx);
        return;
    }

    if (msg->keys & (KEY_UP | KEY_F1)) {
        moveCursor(-1);
    }
    else if (msg->keys & (KEY_DOWN | KEY_F2)) {
        moveCursor(+1);
    }
    else if (msg->keys & KEY_ENTER) {
        activate(ts.sel, sync_rtx);
    }
}

/******************************************************************************
 *                              Public API                                    *
 ******************************************************************************/

void ui_tools_open(void)
{
#ifdef CONFIG_SCAN
    /*
     * The menu is the door to changing the channel, the mode or the frequency,
     * and a running memory scan retunes the radio under all three of them:
     * docs/SCAN.md asks the interface to stop it before any manual change, and
     * a menu whose Mode row flickered with the channel being scanned would be
     * lying about what green is going to apply.
     */
    scan_service_stop();
#endif

    ts.backScreen = (state.ui_screen == MAIN_MEM) ? MAIN_MEM : MAIN_VFO;
    ts.screen = TS_LIST;
    ts.sel    = 0;
    ts.first  = 0;
    ts.msel   = 0;
    ts.mfirst = 0;
    buildModes();

    state.ui_screen = TOOLS_MENU;

    /* The menu owns the keypad, so it owns the transmitter: same rule and same
       door as the channel list (state.txDisable travels in rtxStatus_t) */
    state.txDisable = true;
    ui_rtxSyncRequest = true;
}

bool ui_tools_isOpen(void)
{
    return ts.screen != TS_CLOSED;
}

bool ui_tools_handleEvent(kbd_msg_t *msg, bool *sync_rtx)
{
    if (ts.screen == TS_CLOSED)
        return false;

    if (ts.screen == TS_MODES)
        modesKey(msg, sync_rtx);
    else
        listKey(msg, sync_rtx);

    return true;
}

void ui_tools_draw(void)
{
    switch (ts.screen) {
        case TS_MODES:
            drawModes();
            break;

        case TS_LIST:
            drawList();
            break;

        default:
            break;
    }
}

bool ui_tools_inModes(void)
{
    return ts.screen == TS_MODES;
}

uint8_t ui_tools_selected(void)
{
    return (ts.screen == TS_MODES) ? ts.msel : ts.sel;
}

uint8_t ui_tools_rowCount(void)
{
    return (ts.screen == TS_MODES) ? ts.modes : T_COUNT;
}

const char *ui_tools_rowLabel(uint8_t row)
{
    if (ts.screen == TS_MODES)
        return (row < ts.modes) ? ui_mode_title(ts.modeId[row]) : NULL;

    return (row < T_COUNT) ? rows[row].label : NULL;
}

const char *ui_tools_rowValue(uint8_t row, char *buf, size_t len)
{
    static char scratch[12];

    if ((buf == NULL) || (len == 0)) {
        buf = scratch;
        len = sizeof(scratch);
    }

    buf[0] = '\0';

    if (ts.screen == TS_MODES)
        return (row < ts.modes) ? modeValue(ts.modeId[row], buf, len) : "";

    return (row < T_COUNT) ? listValue(rows[row].id, buf, len) : "";
}

const char *ui_tools_title(void)
{
    return (ts.screen == TS_MODES) ? "MODE" : "TOOLS";
}

uint8_t ui_tools_backScreen(void)
{
    return ts.backScreen;
}
