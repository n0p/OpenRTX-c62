/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The graphical user interface (GUI) works by splitting the screen in
 * horizontal rows, with row height depending on vertical resolution.
 *
 * The general screen layout is composed by an upper status bar at the
 * top of the screen and a lower status bar at the bottom.
 * The central portion of the screen is filled by two big text/number rows
 * And a small row.
 *
 * Below is shown the row height for two common display densities.
 *
 *        160x128 display (MD380)            Recommended font size
 *      ┌─────────────────────────┐
 *      │  top_status_bar (16px)  │  8 pt (11 px) font with 2 px vertical padding
 *      │      top_pad (4px)      │  4 px padding
 *      │      Line 1 (20px)      │  8 pt (11 px) font with 4 px vertical padding
 *      │      Line 2 (20px)      │  8 pt (11 px) font with 4 px vertical padding
 *      │                         │
 *      │      Line 3 (40px)      │  16 pt (xx px) font with 6 px vertical padding
 *      │ RSSI+squelch bar (20px) │  20 px
 *      │      bottom_pad (4px)   │  4 px padding
 *      └─────────────────────────┘
 *
 *         128x64 display (GD-77)
 *      ┌─────────────────────────┐
 *      │  top_status_bar (11 px) │  6 pt (9 px) font with 1 px vertical padding
 *      │      top_pad (1px)      │  1 px padding
 *      │      Line 1 (10px)      │  6 pt (9 px) font without vertical padding
 *      │      Line 2 (10px)      │  6 pt (9 px) font with 2 px vertical padding
 *      │      Line 3 (18px)      │  12 pt (xx px) font with 0 px vertical padding
 *      │ RSSI+squelch bar (11px) │  11 px
 *      │      bottom_pad (1px)   │  1 px padding
 *      └─────────────────────────┘
 *
 *         128x48 display (RD-5R)
 *      ┌─────────────────────────┐
 *      │  top_status_bar (11 px) │  6 pt (9 px) font with 1 px vertical padding
 *      ├─────────────────────────┤  1 px line
 *      │      Line 2 (10px)      │  8 pt (11 px) font with 4 px vertical padding
 *      │      Line 3 (18px)      │  8 pt (11 px) font with 4 px vertical padding
 *      └─────────────────────────┘
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "ui/ui_default.h"
#ifdef CONFIG_C62_NOISE_SUPPRESS
#include "core/noise_suppress.h"
#endif
#include "rtx/rtx.h"
#ifdef UI_MODE_TETRA
#include "protocols/TETRA/TetraUplink.h"
#endif
#include "core/airband.h"
#include "core/channel_store.h"
#include "interfaces/platform.h"
#include "interfaces/display.h"
#include "interfaces/cps_io.h"
#include "ui/ui_channels.h"
#include "ui/ui_backup.h"
#include "ui/ui_spectrum.h"
#include "ui/ui_contacts.h"
#include "ui/ui_messages.h"
#include "interfaces/nvmem.h"
#include "interfaces/delays.h"
#include <string.h>
#include "core/battery.h"
#include "core/input.h"
#include "core/utils.h"
#include "hwconfig.h"
#include "core/voicePromptUtils.h"
#include "core/beeps.h"
#include "ui/ui_hints.h"
#include "ui/ui_zones.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_tools.h"
#ifdef CONFIG_APRS
#include "ui/ui_aprs.h"
#include "core/aprs_service.h"
#endif
#ifdef CONFIG_KISS
#include "ui/ui_packet.h"
#include "core/packet_service.h"
#endif
#ifdef CONFIG_VFO_SEARCH
#include "core/vfo_search.h"
#include "core/vfo_search_service.h"
#endif

#ifdef UI_CAPTURE
#include "core/gfx_capture.h"
#endif

/* UI main screen functions, their implementation is in "ui_main.c" */
extern void _ui_drawMainBackground();
extern void _ui_drawMainTop(ui_state_t* ui_state);
extern void _ui_drawVFOMiddle();
extern void _ui_drawMEMMiddle();
extern void _ui_drawVFOBottom();
extern void _ui_drawMEMBottom();
extern void _ui_drawMainVFO(ui_state_t* ui_state);
extern void _ui_drawMainVFOInput(ui_state_t* ui_state);
extern void _ui_drawMainMEM(ui_state_t* ui_state);
/* UI menu functions, their implementation is in "ui_menu.c" */
extern void _ui_drawMenuTop(ui_state_t* ui_state);
extern void _ui_drawMenuBank(ui_state_t* ui_state);
extern void _ui_drawMenuChannel(ui_state_t* ui_state);
extern void _ui_drawMenuContacts(ui_state_t* ui_state);
#ifdef CONFIG_GPS
extern void _ui_drawMenuGPS();
extern void _ui_drawSettingsGPS(ui_state_t* ui_state);
#endif
extern void _ui_drawSettingsAccessibility(ui_state_t* ui_state);
extern void _ui_drawMenuSettings(ui_state_t* ui_state);
extern void _ui_drawMenuBackupRestore(ui_state_t* ui_state);
extern void _ui_drawMenuInfo(ui_state_t* ui_state);
extern void _ui_drawMenuAbout(ui_state_t* ui_state);
#ifdef CONFIG_RTC
extern void _ui_drawSettingsTimeDate();
extern void _ui_drawSettingsTimeDateSet(ui_state_t* ui_state);
#endif
extern void _ui_drawSettingsDisplay(ui_state_t* ui_state);
extern void _ui_drawSettingsM17(ui_state_t* ui_state);
extern void _ui_drawSettingsFM(ui_state_t* ui_state);
#ifdef CONFIG_TETRA_AFC
extern void _ui_drawSettingsTetra(ui_state_t* ui_state);
#endif
extern void _ui_drawSettingsVoicePrompts(ui_state_t* ui_state);
extern void _ui_drawSettingsReset2Defaults(ui_state_t* ui_state);
extern void _ui_drawSettingsRadio(ui_state_t* ui_state);
extern void _ui_drawSettingsIdentity(ui_state_t* ui_state);
extern void _ui_drawSettingsAudio(ui_state_t* ui_state);
extern bool _ui_drawMacroMenu(ui_state_t* ui_state);
extern void _ui_reset_menu_anouncement_tracking();
// TODO: get these from ui strings / currentLanguage
const char *menu_items[] =
{
    "Banks",
    "Channels",
    "Zones",
    "Contacts",
    "Messages",
#ifdef CONFIG_GPS
    "GPS",
#endif
    "Settings",
    /*
     * The internal backup of the stores (docs/BACKUP.md). Between Settings and
     * Info because it is maintenance, not an operating mode: the two rows
     * behind it are MENU_BACKUP and MENU_RESTORE, which the generic branch had
     * since the MD-3x0 and no key ever reached.
     */
    "Backup",
    "Info",
    "About"
};

const char *settings_items[] =
{
    "Display",
#ifdef CONFIG_RTC
    "Time & Date",
#endif
#ifdef CONFIG_GPS
    "GPS",
#endif
    "Radio",
    "Identity",
    "Audio",
#ifdef UI_MODE_M17
    "M17",
#endif
#ifdef CONFIG_TETRA_AFC
    "TETRA",
#endif
    "FM",
#define UI_MODE_PAGE(id, title) title,
    UI_MODE_PAGES(UI_MODE_PAGE)
#undef UI_MODE_PAGE
    "Accessibility",
    "Default Settings"
};

const char *display_items[] =
{
#ifdef CONFIG_SCREEN_BRIGHTNESS
    "Brightness",
#endif
#ifdef CONFIG_SCREEN_CONTRAST
    "Contrast",
#endif
    "Timer",
    "Battery Icon",
#ifdef CONFIG_KEY_BACKLIGHT
    "Key Light",
#endif
};

#ifdef CONFIG_GPS
const char *settings_gps_items[] =
{
    "GPS Enabled",
#ifdef CONFIG_RTC
    "GPS Set Time",
    "UTC Timezone"
#endif
};
#endif

const char *settings_radio_items[] =
{
    "Offset",
    "Direction",
    "Step",
    "Squelch",
    "TX Power",
    /*
     * The accessory jack's pad is not here any more either: it is one row of
     * the tools menu (P1 held down), the only menu reachable from every main
     * screen. It used to be a row of this page and a row of every mode's
     * popup at the same time, and an operator who set one and saw the other
     * say something else could not tell which one the firmware obeyed
     * (docs/ACCESSORY.md).
     */
};

const char *settings_audio_items[] =
{
    "Voice",
    "Phonetic",
#ifdef CONFIG_C62_NOISE_SUPPRESS
    "RX Noise",
#endif
};

/*
 * Settings -> Identity. Two rows, and no mode owns either: the callsign is the
 * station's name on every mode that carries one, the DMR ID the number DMR
 * carries in the source field of every link control.
 */
const char *settings_identity_items[] =
{
    "Callsign",
    "DMR ID",
};

const char * settings_m17_items[] =
{
    "Meta Txt",
    "CAN",
    "CAN RX Check"
};

#ifdef CONFIG_TETRA_AFC
const char *settings_tetra_items[] =
{
    "AFC",
};
#endif

/*
 * Settings -> FM. The four rows that used to sit here after "CTCSS En." --
 * "CTCSS TX", "CTCSS RX" and "Cancel CT" -- were the same two flags a second
 * and a third time: "CTCSS En." walks all four combinations (None, TX, RX,
 * Both) and clearing both is None. The popup has always had the one row, and
 * two screens that can set one flag disagreeing is how an operator ends up
 * not knowing which one the radio obeys, so the page keeps the popup's row.
 */
const char * settings_fm_items[] =
{
    "CTCSS Tone",
    "CTCSS En.",
    "Bandwidth",
};

const char * settings_accessibility_items[] =
{
    "Macro Latch",
};

const char *backup_restore_items[] =
{
    "Backup",
    "Restore"
};

const char *info_items[] =
{
    "",
    "Bat. Voltage",
    "Bat. Charge",
    "RSSI",
    "Used heap",
    "Band",
    "VHF",
    "UHF",
    "Hw Version",
#ifdef PLATFORM_TTWRPLUS
    "Radio",
    "Radio FW",
#endif
};

const char *authors[] =
{
    "Niccolo' IU2KIN",
    "Silvano IU2KWO",
    "Federico IU2NUO",
    "Fred IU2NRO",
    "Joseph VK7JS",
    "Morgan ON4MOD",
    "Marco DM4RCO"
};

static const char *symbols_ITU_T_E161[] =
{
    " 0",
    ",.?1",
    "abc2ABC",
    "def3DEF",
    "ghi4GHI",
    "jkl5JKL",
    "mno6MNO",
    "pqrs7PQRS",
    "tuv8TUV",
    "wxyz9WXYZ",
    "-/*",
    "#"
};

static const char *symbols_ITU_T_E161_callsign[] =
{
    "0 ",
    "1",
    "ABC2",
    "DEF3",
    "GHI4",
    "JKL5",
    "MNO6",
    "PQRS7",
    "TUV8",
    "WXYZ9",
    "-/",
    ""
};

// Calculate number of menu entries
const uint8_t menu_num = sizeof(menu_items)/sizeof(menu_items[0]);
const uint8_t settings_num = sizeof(settings_items)/sizeof(settings_items[0]);
const uint8_t display_num = sizeof(display_items)/sizeof(display_items[0]);
#ifdef CONFIG_GPS
const uint8_t settings_gps_num = sizeof(settings_gps_items)/sizeof(settings_gps_items[0]);
#endif
const uint8_t settings_radio_num = sizeof(settings_radio_items)/sizeof(settings_radio_items[0]);
const uint8_t settings_audio_num = sizeof(settings_audio_items)/sizeof(settings_audio_items[0]);
const uint8_t settings_identity_num = sizeof(settings_identity_items)/sizeof(settings_identity_items[0]);
#ifdef UI_MODE_M17
const uint8_t settings_m17_num = sizeof(settings_m17_items)/sizeof(settings_m17_items[0]);
#endif
const uint8_t settings_fm_num = sizeof(settings_fm_items) / sizeof(settings_fm_items[0]);
#ifdef CONFIG_TETRA_AFC
const uint8_t settings_tetra_num = sizeof(settings_tetra_items) / sizeof(settings_tetra_items[0]);
#endif
const uint8_t settings_accessibility_num = sizeof(settings_accessibility_items)/sizeof(settings_accessibility_items[0]);
const uint8_t backup_restore_num = sizeof(backup_restore_items)/sizeof(backup_restore_items[0]);
const uint8_t info_num = sizeof(info_items)/sizeof(info_items[0]);
const uint8_t author_num = sizeof(authors)/sizeof(authors[0]);

const color_t color_black = {0, 0, 0, 255};
const color_t color_grey = {60, 60, 60, 255};
const color_t color_white = {255, 255, 255, 255};
const color_t yellow_fab413 = {250, 180, 19, 255};

layout_t layout;
state_t last_state;
bool macro_latched;
static ui_state_t ui_state;
static bool macro_menu = false;
static bool layout_ready = false;
static bool redraw_needed = true;

/*
 * Key claim trace.
 *
 * Every block of the dispatch that consumes a key event says so before it
 * returns, into these two bytes: which block took it and how many took it.
 * The tests walk every screen with every key and require exactly one claim per
 * event, which is the mechanical form of the two defects the operator hit
 * (a screen left deaf by a stale edit_mode, and a block that ate keys that
 * belonged to another screen). It costs one byte of RAM and one store per key
 * press; the interface is in ui/ui_default.h.
 */
static uint8_t keyClaim  = UI_CLAIM_NONE;
static uint8_t keyClaims = 0;

#define KEY_CLAIM(block)  do { keyClaim = (uint8_t)(block); keyClaims++; } while (0)

uint8_t ui_lastClaim(void)
{
    return keyClaim;
}

uint8_t ui_lastClaimCount(void)
{
    return keyClaims;
}

uint8_t ui_menu_selected(void)
{
    return ui_state.menu_selected;
}

bool ui_edit_mode(void)
{
    return ui_state.edit_mode;
}

/*
 * Passes of the drawing loop, counted whether or not the screen changed.
 *
 * The simulator's control channel waits for this to advance before it reads a
 * screen or takes a picture (docs/SIMULATOR.md): a key press reaches the UI
 * thread asynchronously, and "the UI has run at least twice since the key was
 * queued" is what makes a dump describe the screen after the key and not
 * before it. It costs one store per pass on the radio.
 */
static volatile uint32_t frameCount;

uint32_t ui_frames(void)
{
    return frameCount;
}

void ui_requestRedraw(void)
{
    redraw_needed = true;
}

bool ui_macroGridOpen(void)
{
    return macro_menu;
}


/*
 * Set by code that changes the radio configuration outside the UI FSM (the
 * quick menu setters, the remote control) so that the UI thread sends it to
 * the rtx thread. Declared in core/ui.h.
 */
volatile bool ui_rtxSyncRequest = false;

static bool standby = false;
static long long last_event_tick = 0;

#ifdef CONFIG_VFO_SEARCH
/*
 * Carrier search arming.
 *
 * The keypad reports the long press of a key 700 ms after it went down, so
 * that event is what starts the clock: the sweep begins when UP or DOWN has
 * been held for three seconds, and letting go before that cancels the arming
 * silently.  The key stops tuning the moment it is armed, because a long press
 * of an arrow on the VFO screen is the search and nothing else.
 */
#define VFO_SEARCH_HOLD_MS 3000

static keyboard_t searchArmKey;      ///< KEY_UP / KEY_DOWN being held, 0 none
static long long  searchArmTick;     ///< When that key went down
static long long  searchRedrawTick;  ///< Last periodic redraw of the screen
static bool       searchMsgShown;    ///< A stop reason is on screen
#endif

// UI event queue
static uint8_t evQueue_rdPos;
static uint8_t evQueue_wrPos;
static event_t evQueue[MAX_NUM_EVENTS];

uint8_t ui_pendingEvents(void)
{
    return (uint8_t)((evQueue_wrPos + MAX_NUM_EVENTS - evQueue_rdPos)
                     % MAX_NUM_EVENTS);
}

/*
 * The last key event the FSM dispatched, as the keypad delivered it: the keys
 * and whether it was the long press. The simulator's dump shows it, because
 * "the key arrived as a short press" and "the key never arrived" look the same
 * on the screen and are two very different defects (docs/SIMULATOR.md).
 */
static volatile uint32_t lastKeyEvent = 0;

uint32_t ui_lastKeyEvent(void)
{
    return lastKeyEvent;
}


static void _ui_calculateLayout(layout_t *layout)
{
    // Horizontal line height
    static const uint16_t hline_h = 1;
    // Compensate for fonts printing below the start position
    static const uint16_t text_v_offset = 1;

    // Calculate UI layout depending on vertical resolution
    // Tytera MD380, MD-UV380
    #if CONFIG_SCREEN_HEIGHT > 127

    // Height and padding shown in diagram at beginning of file
    static const uint16_t top_h = 16;
    static const uint16_t top_pad = 4;
    static const uint16_t line1_h = 20;
    static const uint16_t line2_h = 20;
    static const uint16_t line3_h = 20;
    static const uint16_t line3_large_h = 40;
    static const uint16_t line4_h = 20;
    /* Menus and lists: 6 pt rows, 12 px, as requested for the 160x128 panel.
       Ten entries fit above the hints row. */
    static const uint16_t menu_h = 12;
    static const uint16_t bottom_h = 23;
    static const uint16_t bottom_pad = top_pad;
    static const uint16_t status_v_pad = 2;
    static const uint16_t small_line_v_pad = 2;
    static const uint16_t big_line_v_pad = 6;
    static const uint16_t horizontal_pad = 4;

    // Top bar font: 8 pt
    static const fontSize_t   top_font = FONT_SIZE_8PT;
    static const symbolSize_t top_symbol_size = SYMBOLS_SIZE_8PT;
    // Text line font: 8 pt
    static const fontSize_t line1_font = FONT_SIZE_8PT;
    static const symbolSize_t line1_symbol_size = SYMBOLS_SIZE_8PT;
    static const fontSize_t line2_font = FONT_SIZE_8PT;
    static const symbolSize_t line2_symbol_size = SYMBOLS_SIZE_8PT;
    static const fontSize_t line3_font = FONT_SIZE_8PT;
    static const symbolSize_t line3_symbol_size = SYMBOLS_SIZE_8PT;
    static const fontSize_t line4_font = FONT_SIZE_8PT;
    static const symbolSize_t line4_symbol_size = SYMBOLS_SIZE_8PT;
    // Message font
    const fontSize_t message_font = FONT_SIZE_6PT;
    // Frequency line font: 16 pt
    static const fontSize_t line3_large_font = FONT_SIZE_16PT;
    // Bottom bar font: 8 pt
    static const fontSize_t bottom_font = FONT_SIZE_8PT;
    // TimeDate/Frequency input font
    static const fontSize_t input_font = FONT_SIZE_12PT;
    // Menu font: 6 pt, the top bar keeps 8 pt for hierarchy
    static const fontSize_t menu_font = FONT_SIZE_6PT;

    // Radioddity GD-77
    #elif CONFIG_SCREEN_HEIGHT > 63

    // Height and padding shown in diagram at beginning of file
    static const uint16_t top_h = 11;
    static const uint16_t top_pad = 1;
    static const uint16_t line1_h = 10;
    static const uint16_t line2_h = 10;
    static const uint16_t line3_h = 10;
    static const uint16_t line3_large_h = 16;
    static const uint16_t line4_h = 10;
    static const uint16_t menu_h = 10;
    static const uint16_t bottom_h = 15;
    static const uint16_t bottom_pad = 0;
    static const uint16_t status_v_pad = 1;
    static const uint16_t small_line_v_pad = 1;
    static const uint16_t big_line_v_pad = 0;
    static const uint16_t horizontal_pad = 4;

    // Top bar font: 6 pt
    static const fontSize_t   top_font = FONT_SIZE_6PT;
    static const symbolSize_t top_symbol_size = SYMBOLS_SIZE_6PT;
    // Middle line fonts: 5, 8, 8 pt
    static const fontSize_t line1_font = FONT_SIZE_6PT;
    static const symbolSize_t line1_symbol_size = SYMBOLS_SIZE_6PT;
    static const fontSize_t line2_font = FONT_SIZE_6PT;
    static const symbolSize_t line2_symbol_size = SYMBOLS_SIZE_6PT;
    static const fontSize_t line3_font = FONT_SIZE_6PT;
    static const symbolSize_t line3_symbol_size = SYMBOLS_SIZE_6PT;
    static const fontSize_t line3_large_font = FONT_SIZE_10PT;
    static const fontSize_t line4_font = FONT_SIZE_6PT;
    static const symbolSize_t line4_symbol_size = SYMBOLS_SIZE_6PT;
    // Message font
    const fontSize_t message_font = FONT_SIZE_6PT;
    // Bottom bar font: 6 pt
    static const fontSize_t bottom_font = FONT_SIZE_6PT;
    // TimeDate/Frequency input font
    static const fontSize_t input_font = FONT_SIZE_8PT;
    // Menu font
    static const fontSize_t menu_font = FONT_SIZE_6PT;

    // Radioddity RD-5R
    #elif CONFIG_SCREEN_HEIGHT > 47

    // Height and padding shown in diagram at beginning of file
    static const uint16_t top_h = 11;
    static const uint16_t top_pad = 1;
    static const uint16_t line1_h = 0;
    static const uint16_t line2_h = 10;
    static const uint16_t line3_h = 10;
    static const uint16_t line3_large_h = 18;
    static const uint16_t line4_h = 10;
    static const uint16_t menu_h = 10;
    static const uint16_t bottom_h = 0;
    static const uint16_t bottom_pad = 0;
    static const uint16_t status_v_pad = 1;
    static const uint16_t small_line_v_pad = 1;
    static const uint16_t big_line_v_pad = 0;
    static const uint16_t horizontal_pad = 4;

    // Top bar font: 6 pt
    static const fontSize_t   top_font = FONT_SIZE_6PT;
    static const symbolSize_t top_symbol_size = SYMBOLS_SIZE_6PT;
    // Middle line fonts: 16, 16
    static const fontSize_t line2_font = FONT_SIZE_6PT;
    static const fontSize_t line3_font = FONT_SIZE_6PT;
    static const fontSize_t line4_font = FONT_SIZE_6PT;
    static const fontSize_t line3_large_font = FONT_SIZE_12PT;
    // TimeDate/Frequency input font
    static const fontSize_t input_font = FONT_SIZE_8PT;
    // Menu font
    static const fontSize_t menu_font = FONT_SIZE_6PT;
    // Message font
    const fontSize_t message_font = FONT_SIZE_6PT;
    // Not present on this resolution
    static const fontSize_t line1_font = 0;
    static const fontSize_t bottom_font = 0;

    #else
    #error Unsupported vertical resolution!
    #endif

    // Calculate printing positions
    static const uint16_t top_pos   = top_h - status_v_pad - text_v_offset;
    static const uint16_t line1_pos = top_h + top_pad + line1_h - small_line_v_pad - text_v_offset;
    static const uint16_t line2_pos = top_h + top_pad + line1_h + line2_h - small_line_v_pad - text_v_offset;
    static const uint16_t line3_pos = top_h + top_pad + line1_h + line2_h + line3_h - small_line_v_pad - text_v_offset;
    static const uint16_t line4_pos = top_h + top_pad + line1_h + line2_h + line3_h + line4_h - small_line_v_pad - text_v_offset;
    static const uint16_t line3_large_pos = top_h + top_pad + line1_h + line2_h + line3_large_h - big_line_v_pad - text_v_offset;
    static const uint16_t bottom_pos = CONFIG_SCREEN_HEIGHT - bottom_pad - status_v_pad - text_v_offset;

    layout_t new_layout =
    {
        hline_h,
        top_h,
        line1_h,
        line2_h,
        line3_h,
        line3_large_h,
        line4_h,
        menu_h,
        bottom_h,
        bottom_pad,
        status_v_pad,
        horizontal_pad,
        text_v_offset,
        {horizontal_pad, top_pos},
        {horizontal_pad, line1_pos},
        {horizontal_pad, line2_pos},
        {horizontal_pad, line3_pos},
        {horizontal_pad, line3_large_pos},
        {horizontal_pad, line4_pos},
        {horizontal_pad, bottom_pos},
        top_font,
        top_symbol_size,
        line1_font,
        line1_symbol_size,
        line2_font,
        line2_symbol_size,
        line3_font,
        line3_symbol_size,
        line3_large_font,
        line4_font,
        line4_symbol_size,
        bottom_font,
        input_font,
        menu_font,
        message_font
    };

    memcpy(layout, &new_layout, sizeof(layout_t));
}

static void _ui_drawLowBatteryScreen()
{
    gfx_clearScreen();
    uint16_t bat_width = CONFIG_SCREEN_WIDTH / 2;
    uint16_t bat_height = CONFIG_SCREEN_HEIGHT / 3;
    point_t bat_pos = {CONFIG_SCREEN_WIDTH / 4, CONFIG_SCREEN_HEIGHT / 8};
    gfx_drawBattery(bat_pos, bat_width, bat_height, 10);
    point_t text_pos_1 = {0, CONFIG_SCREEN_HEIGHT * 2 / 3};
    point_t text_pos_2 = {0, CONFIG_SCREEN_HEIGHT * 2 / 3 + 16};

    gfx_print(text_pos_1,
              FONT_SIZE_6PT,
              TEXT_ALIGN_CENTER,
              color_white,
              currentLanguage->forEmergencyUse);
    gfx_print(text_pos_2,
              FONT_SIZE_6PT,
              TEXT_ALIGN_CENTER,
              color_white,
              currentLanguage->pressAnyButton);
}

static freq_t _ui_freq_add_digit(freq_t freq, uint8_t pos, uint8_t number)
{
    freq_t coefficient = FREQ_DIGIT_RESOLUTION;
    for(uint8_t i=0; i < FREQ_DIGITS - pos; i++)
    {
        coefficient *= 10;
    }
    return freq += number * coefficient;
}

#ifdef CONFIG_RTC
static void _ui_timedate_add_digit(datetime_t *timedate, uint8_t pos,
                                   uint8_t number)
{
    vp_flush();
    vp_queueInteger(number);
    if (pos == 2 || pos == 4)
        vp_queuePrompt(PROMPT_SLASH);
    // just indicates separation of date and time.
    if (pos==6) // start of time.
        vp_queueString("hh:mm", vpAnnounceCommonSymbols|vpAnnounceLessCommonSymbols);
    if (pos == 8)
        vp_queuePrompt(PROMPT_COLON);
    vp_play();

    switch(pos)
    {
        // Set date
        case 1:
            timedate->date += number * 10;
            break;
        case 2:
            timedate->date += number;
            break;
        // Set month
        case 3:
            timedate->month += number * 10;
            break;
        case 4:
            timedate->month += number;
            break;
        // Set year
        case 5:
            timedate->year += number * 10;
            break;
        case 6:
            timedate->year += number;
            break;
        // Set hour
        case 7:
            timedate->hour += number * 10;
            break;
        case 8:
            timedate->hour += number;
            break;
        // Set minute
        case 9:
            timedate->minute += number * 10;
            break;
        case 10:
            timedate->minute += number;
            break;
    }
}
#endif

static bool _ui_freq_check_limits(freq_t freq)
{
    bool valid = false;
    const hwInfo_t* hwinfo = platform_getHwInfo();
    if(hwinfo->vhf_band)
    {
        // hwInfo_t frequencies are in MHz
        if(freq >= (hwinfo->vhf_minFreq * 1000000) &&
           freq <= (hwinfo->vhf_maxFreq * 1000000))
        valid = true;
    }
    if(hwinfo->uhf_band)
    {
        // hwInfo_t frequencies are in MHz
        if(freq >= (hwinfo->uhf_minFreq * 1000000) &&
           freq <= (hwinfo->uhf_maxFreq * 1000000))
        valid = true;
    }
    return valid;
}

static bool _ui_channel_valid(channel_t* channel)
{
return _ui_freq_check_limits(channel->rx_frequency) &&
       _ui_freq_check_limits(channel->tx_frequency);
}

static bool _ui_drawDarkOverlay()
{
    color_t alpha_grey = {0, 0, 0, 255};
    point_t origin = {0, 0};
    gfx_drawRect(origin, CONFIG_SCREEN_WIDTH, CONFIG_SCREEN_HEIGHT, alpha_grey, true);
    return true;
}

static int _ui_fsm_loadChannel(int16_t channel_index, bool *sync_rtx)
{
    channel_t channel;
    int32_t selected_channel = channel_index;
    // If a bank is active, get index from current bank
    if(state.bank_enabled)
    {
        bankHdr_t bank = { 0 };
        cps_readBankHeader(&bank, state.bank);
        if((channel_index < 0) || (channel_index >= bank.ch_count))
            return -1;
        channel_index = cps_readBankData(state.bank, channel_index);
    }

    int result = cps_readChannel(&channel, channel_index);
    // Read successful and channel is valid
    if((result != -1) && _ui_channel_valid(&channel))
    {
        // Set new channel index
        state.channel_index = selected_channel;
        // Copy channel read to state
        state.channel = channel;
        /*
         * A channel can be an airband one: put the VFO step back on the raster
         * the operator chose, so tuning away from it steps by 25 or 8.33 kHz
         * and not by whatever the previous mode left behind.
         */
        qm_am_applyRaster();
        *sync_rtx = true;
    }

    return result;
}

/**
 * Snap a frequency to the airband 8.33 kHz raster, when that applies.
 *
 * Only in the airband mode and only with the snap enabled: the 8.33 kHz step of
 * the VFO is a whole number of hertz, so stepping drifts off the real carrier
 * (25/3 kHz), and a designator copied from a chart ("118.005") is not the
 * carrier of the channel it names (118.00833). Frequencies outside the band are
 * left alone, which is what airband_snap() already does.
 */
static void _ui_airbandSnap(freq_t *freq)
{
    if ((state.channel.mode != OPMODE_AIRBAND) || (state.settings.am_snap == 0))
        return;

    freq_t snapped = airband_snap(*freq);

    if (snapped != 0)
        *freq = snapped;
}

/**
 * Apply the frequencies typed in the VFO input screen.
 *
 * In a receive-only mode (TETRA, DMR) there is one frequency: RX and TX are
 * set to the same value and the user is not asked for a second one.
 *
 * @return true if a valid frequency was applied.
 */
static bool _ui_applyVFOInput(bool *sync_rtx)
{
    bool rxOnly = !opmode_canTransmit(state.channel.mode);

    if(ui_state.new_tx_frequency == 0 || rxOnly)
    {
        ui_state.new_tx_frequency = ui_state.new_rx_frequency;
    }

    /*
     * Airband with the snap on: a frequency typed by hand lands on the nearest
     * 8.33 kHz carrier (a designator such as 118.005 is not a frequency, its
     * carrier is 118.00833), so the receiver is never a third of a channel off
     * after the operator copied a designator from a chart.
     */
    _ui_airbandSnap(&ui_state.new_rx_frequency);
    if (ui_state.new_tx_frequency == ui_state.new_rx_frequency)
        ui_state.new_tx_frequency = ui_state.new_rx_frequency;
    else
        _ui_airbandSnap(&ui_state.new_tx_frequency);

    if((ui_state.new_rx_frequency == 0) ||
       (_ui_freq_check_limits(ui_state.new_rx_frequency) == false) ||
       (_ui_freq_check_limits(ui_state.new_tx_frequency) == false))
    {
        return false;
    }

    state.channel.rx_frequency = ui_state.new_rx_frequency;
    state.channel.tx_frequency = ui_state.new_tx_frequency;
    *sync_rtx = true;
    return true;
}

static void _ui_fsm_confirmVFOInput(bool *sync_rtx)
{
    vp_flush();

    /*
     * Green is "Set" on this screen, as the key table in docs/UI.md says: it
     * applies what has been typed and returns to the VFO.
     *
     * It used to do something else when the RX field was being edited and the
     * mode can transmit -- it moved to the TX field and applied nothing -- so
     * the operator who typed a frequency and pressed green saw the field go
     * blank and the receiver stay where it was: "I type the frequency and
     * nothing happens". The TX field is still there for a split, reached with
     * UP/DOWN (the field switch) before typing the second frequency, and an
     * empty one takes a copy of RX in _ui_applyVFOInput().
     */
    if(_ui_applyVFOInput(sync_rtx))
    {
        // force init to clear any prompts in progress.
        // defer play because play is called at the end of the function
        //due to above freq queuing.
        vp_announceFrequencies(state.channel.rx_frequency,
                               state.channel.tx_frequency, vpqInit);
        state.ui_screen = MAIN_VFO;
    }
    else
    {
        // Do not silently drop the frequency: say so and stay in the
        // input screen so the digits can be corrected.
        vp_announceError(vpqInit);
    }

    vp_play();
}

static void _ui_fsm_insertVFONumber(kbd_msg_t msg, bool *sync_rtx)
{
    /*
     * The field is full and the frequency has not been applied (out of range):
     * wait for ENTER or ESC. A further digit would index past FREQ_DIGITS in
     * _ui_freq_add_digit() and shift the frequency out of the freq_t range.
     */
    if(ui_state.input_position >= FREQ_DIGITS)
        return;

    // Advance input position
    ui_state.input_position += 1;
    // clear any prompts in progress.
    vp_flush();
    // Save pressed number to calculate frequency and show in GUI
    ui_state.input_number = input_getPressedNumber(msg);
    // queue the digit just pressed.
    vp_queueInteger(ui_state.input_number);
    // queue  point if user has entered three digits.
    if (ui_state.input_position == 3)
        vp_queuePrompt(PROMPT_POINT);

    if(ui_state.input_set == SET_RX)
    {
        if(ui_state.input_position == 1)
            ui_state.new_rx_frequency = 0;
        // Calculate portion of the new RX frequency
        ui_state.new_rx_frequency = _ui_freq_add_digit(ui_state.new_rx_frequency,
                                                       ui_state.input_position,
                                                       ui_state.input_number);
        if(ui_state.input_position >= FREQ_DIGITS)
        {
            // A receive-only mode has no TX field: apply what was typed
            if(opmode_canTransmit(state.channel.mode) == false)
            {
                if(_ui_applyVFOInput(sync_rtx))
                {
                    vp_announceFrequencies(state.channel.rx_frequency,
                                           state.channel.tx_frequency, vpqInit);
                    state.ui_screen = MAIN_VFO;
                }
                else
                {
                    // Out of range: stay here so the digits can be corrected
                    vp_announceError(vpqInit);
                }
            }
            else
            {
                // queue the rx freq just completed.
                vp_queueFrequency(ui_state.new_rx_frequency);
                /// now queue tx as user has changed fields.
                vp_queuePrompt(PROMPT_TRANSMIT);
                // Switch to TX input
                ui_state.input_set = SET_TX;
                // Reset input position
                ui_state.input_position = 0;
                // Reset TX frequency
                ui_state.new_tx_frequency = 0;
            }
        }
    }
    else if(ui_state.input_set == SET_TX)
    {
        if(ui_state.input_position == 1)
            ui_state.new_tx_frequency = 0;
        // Calculate portion of the new TX frequency
        ui_state.new_tx_frequency = _ui_freq_add_digit(ui_state.new_tx_frequency,
                                                       ui_state.input_position,
                                                       ui_state.input_number);
        if(ui_state.input_position >= FREQ_DIGITS)
        {
            // Save both inserted frequencies
            if(_ui_applyVFOInput(sync_rtx))
            {
                // play is called at end.
                vp_announceFrequencies(state.channel.rx_frequency,
                                       state.channel.tx_frequency, vpqInit);
            }
            else
            {
                vp_announceError(vpqInit);
            }

            state.ui_screen = MAIN_VFO;
        }
    }

    vp_play();
}

#ifdef CONFIG_SCREEN_BRIGHTNESS
static void _ui_changeBrightness(int variation)
{
    state.settings.brightness += variation;

    // Max value for brightness is 100, min value is set to 5 to avoid complete
    //  display shutdown.
    if(state.settings.brightness > 100) state.settings.brightness = 100;
    if(state.settings.brightness < 5)   state.settings.brightness = 5;

    display_setBacklightLevel(state.settings.brightness);
}
#endif

#ifdef CONFIG_SCREEN_CONTRAST
static void _ui_changeContrast(int variation)
{
    if(variation >= 0)
        state.settings.contrast =
        (255 - state.settings.contrast < variation) ? 255 : state.settings.contrast + variation;
    else
        state.settings.contrast =
        (state.settings.contrast < -variation) ? 0 : state.settings.contrast + variation;

    display_setContrast(state.settings.contrast);
}
#endif

static void _ui_changeTimer(int variation)
{
    if ((state.settings.display_timer == TIMER_OFF && variation < 0) ||
        (state.settings.display_timer == TIMER_1H && variation > 0))
    {
        return;
    }

    state.settings.display_timer += variation;
}

static void _ui_changeMacroLatch(bool newVal)
{
    state.settings.macroMenuLatch = newVal ? 1 : 0;
    vp_announceSettingsOnOffToggle(&currentLanguage->macroLatching,
                                   vp_getVoiceLevelQueueFlags(),
                                   state.settings.macroMenuLatch);
}

#ifdef UI_MODE_M17
static inline void _ui_changeM17Can(int variation)
{
    uint8_t can = state.settings.m17_can;
    state.settings.m17_can = (can + variation) % 16;
}
#endif

static void _ui_changeVoiceLevel(int variation)
{
    if ((state.settings.vpLevel == vpNone && variation < 0) ||
        (state.settings.vpLevel == vpHigh && variation > 0))
        {
            return;
        }

    state.settings.vpLevel += variation;

    // Force these flags to ensure the changes are spoken for levels 1 through 3.
    enum vpQueueFlags flags = vpqInit
                         | vpqAddSeparatingSilence
                         | vpqPlayImmediately;

    if (!vp_isPlaying())
    {
        flags |= vpqIncludeDescriptions;
    }

    vp_announceSettingsVoiceLevel(flags);
}

#ifdef CONFIG_C62_NOISE_SUPPRESS
static void _ui_changeNoiseSuppression(bool newVal)
{
    /* The stage refuses to enable itself if RNNoise is not available: mirror
     * the result back into the setting so the menu never lies. */
    state.settings.noiseSuppression = noiseSuppress_setEnabled(newVal);
}
#endif

static void _ui_changePhoneticSpell(bool newVal)
{
    state.settings.vpPhoneticSpell = newVal ? 1 : 0;

    vp_announceSettingsOnOffToggle(&currentLanguage->phonetic,
                                   vp_getVoiceLevelQueueFlags(),
                                   state.settings.vpPhoneticSpell);
}

bool _ui_checkStandby(long long time_since_last_event)
{
    if (standby)
    {
        return false;
    }

    switch (state.settings.display_timer)
    {
        case TIMER_OFF:
            return false;
        case TIMER_5S:
        case TIMER_10S:
        case TIMER_15S:
        case TIMER_20S:
        case TIMER_25S:
        case TIMER_30S:
            return time_since_last_event >= (5000 * state.settings.display_timer);
        case TIMER_1M:
        case TIMER_2M:
        case TIMER_3M:
        case TIMER_4M:
        case TIMER_5M:
            return time_since_last_event >=
                (60000 * (state.settings.display_timer - (TIMER_1M - 1)));
        case TIMER_15M:
        case TIMER_30M:
        case TIMER_45M:
            return time_since_last_event >=
                (60000 * 15 * (state.settings.display_timer - (TIMER_15M - 1)));
        case TIMER_1H:
            return time_since_last_event >= 60 * 60 * 1000;
    }

    // unreachable code
    return false;
}

static void _ui_enterStandby()
{
    if(standby)
        return;

    standby = true;
    redraw_needed = false;
    display_setBacklightLevel(0);
}

static bool _ui_exitStandby(long long now)
{
    last_event_tick = now;

    if(!standby)
        return false;

    standby = false;
    redraw_needed = true;
    display_setBacklightLevel(state.settings.brightness);

    return true;
}

// TODO: find a better home for this function
int _ui_handleToneSelectScroll(bool direction_up)
{
    bool tone_tx_enable = state.channel.fm.txToneEn;
    bool tone_rx_enable = state.channel.fm.rxToneEn;
    uint8_t tone_flags = tone_tx_enable << 1 | tone_rx_enable;

    if(direction_up)
        tone_flags++;
    else
        tone_flags--;

    tone_flags %= 4;
    tone_tx_enable = tone_flags >> 1;
    tone_rx_enable = tone_flags & 1;
    state.channel.fm.txToneEn = tone_tx_enable;
    state.channel.fm.rxToneEn = tone_rx_enable;

    return 1;
}

static void _ui_fsm_menuMacro(kbd_msg_t msg, bool *sync_rtx)
{
    // If there is no keyboard left and right select the menu entry to edit
#if defined(CONFIG_UI_NO_KEYBOARD)
    if (msg.keys & KNOB_LEFT)
    {
        ui_state.macro_menu_selected--;
        ui_state.macro_menu_selected += 9;
        ui_state.macro_menu_selected %= 9;
    }
    if (msg.keys & KNOB_RIGHT)
    {
        ui_state.macro_menu_selected++;
        ui_state.macro_menu_selected %= 9;
    }
    if ((msg.keys & KEY_ENTER) && !msg.long_press)
        ui_state.input_number = ui_state.macro_menu_selected + 1;
    else
        ui_state.input_number = 0;
#else // CONFIG_UI_NO_KEYBOARD
    ui_state.input_number = input_getPressedNumber(msg);
#endif // CONFIG_UI_NO_KEYBOARD
    // CTCSS Encode/Decode Selection
    enum vpQueueFlags queueFlags = vp_getVoiceLevelQueueFlags();

    switch(ui_state.input_number)
    {
        case 1:
            if(state.channel.mode == OPMODE_FM)
            {
                _ui_handleToneSelectScroll(true);
                *sync_rtx                 = true;
                vp_announceCTCSS(
                    state.channel.fm.rxToneEn, state.channel.fm.rxTone,
                    state.channel.fm.txToneEn, state.channel.fm.txTone,
                    queueFlags | vpqIncludeDescriptions);
            }
            break;
        case 2:
            if (state.channel.mode == OPMODE_FM)
            {
                if (state.channel.fm.txTone == 0)
                {
                    state.channel.fm.txTone = CTCSS_FREQ_NUM-1;
                }
                else
                {
                    state.channel.fm.txTone--;
                }

                state.channel.fm.txTone %= CTCSS_FREQ_NUM;
                state.channel.fm.rxTone = state.channel.fm.txTone;
                *sync_rtx = true;
                vp_announceCTCSS(state.channel.fm.rxToneEn,
                                 state.channel.fm.rxTone,
                                 state.channel.fm.txToneEn,
                                 state.channel.fm.txTone,
                                 queueFlags);
            }
            break;

        case 3:
            if(state.channel.mode == OPMODE_FM)
            {
                state.channel.fm.txTone++;
                state.channel.fm.txTone %= CTCSS_FREQ_NUM;
                state.channel.fm.rxTone = state.channel.fm.txTone;
                *sync_rtx = true;
                vp_announceCTCSS(state.channel.fm.rxToneEn,
                                 state.channel.fm.rxTone,
                                 state.channel.fm.txToneEn,
                                 state.channel.fm.txTone,
                                 queueFlags |vpqIncludeDescriptions);
            }
            break;
        case 4:
            if(state.channel.mode == OPMODE_FM)
            {
                state.channel.bandwidth++;
                state.channel.bandwidth %= 2;
                *sync_rtx = true;
                vp_announceBandwidth(state.channel.bandwidth, queueFlags);
            }
            break;
        case 5:
        {
            /*
             * Cycle through the modes this build has. The order and the
             * membership are the mode registry's (ui/ui_modes.h): a mode that
             * is compiled in, or compiled out, changes nothing here.
             */
            uint8_t nextMode = ui_mode_next(state.channel.mode, +1);

            state.channel.mode = nextMode;

            /*
             * A receive-only mode has one frequency: keep TX equal to RX so
             * that nothing downstream sees a stale transmit frequency.
             */
            if(opmode_canTransmit(state.channel.mode) == false)
                state.channel.tx_frequency = state.channel.rx_frequency;

            /*
             * Airband: the operator's raster comes back with the mode, so the
             * VFO steps on the same grid the quick menu shows.
             */
            qm_am_applyRaster();

            *sync_rtx = true;
            vp_announceRadioMode(state.channel.mode, queueFlags);
            break;
        }
        case 6:

            switch(state.channel.power)
            {
                case 1000:
                    state.channel.power = 2500;
                    break;

                case 2500:
                    state.channel.power = 5000;
                    break;

                default:
                    state.channel.power = 1000;
            }

            *sync_rtx = true;
            vp_announcePower(state.channel.power, queueFlags);
            break;
#ifdef CONFIG_SCREEN_BRIGHTNESS
        case 7:
            _ui_changeBrightness(-5);
            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                   state.settings.brightness);
            break;
        case 8:
            _ui_changeBrightness(+5);
            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                   state.settings.brightness);
            break;
#endif
        case 9:
            if (!ui_state.input_locked)
                ui_state.input_locked = true;
            else
                ui_state.input_locked = false;
            break;
    }

#if defined(PLATFORM_TTWRPLUS)
    if(msg.keys & KEY_VOLDOWN)
#else
    if(msg.keys & KEY_LEFT || msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
#endif // PLATFORM_TTWRPLUS
    {
#ifdef CONFIG_KNOB_ABSOLUTE // If the radio has an absolute position knob
        state.settings.sqlLevel = platform_getChSelector() - 1;
        if(state.settings.sqlLevel > SLEVEL_MAX)
            state.settings.sqlLevel = SLEVEL_MAX;
#endif // CONFIG_KNOB_ABSOLUTE
        if(state.settings.sqlLevel > 0)
        {
            state.settings.sqlLevel -= 1;
            *sync_rtx = true;
            vp_announceSquelch(state.settings.sqlLevel, queueFlags);
        }
    }

#if defined(PLATFORM_TTWRPLUS)
    else if(msg.keys & KEY_VOLUP)
#else
    else if(msg.keys & KEY_RIGHT || msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
#endif // PLATFORM_TTWRPLUS
    {
#ifdef CONFIG_KNOB_ABSOLUTE
        state.settings.sqlLevel = platform_getChSelector() - 1;
        if(state.settings.sqlLevel > SLEVEL_MAX)
            state.settings.sqlLevel = SLEVEL_MAX;
#endif
        // Levels saved by older versions went up to 15
        if(state.settings.sqlLevel > SLEVEL_MAX)
            state.settings.sqlLevel = SLEVEL_MAX;

        if(state.settings.sqlLevel < SLEVEL_MAX)
        {
            state.settings.sqlLevel += 1;
            *sync_rtx = true;
            vp_announceSquelch(state.settings.sqlLevel, queueFlags);
        }
    }
}

static void _ui_menuUp(uint8_t menu_entries)
{
    if(ui_state.menu_selected > 0)
        ui_state.menu_selected -= 1;
    else
        ui_state.menu_selected = menu_entries - 1;
    vp_playMenuBeepIfNeeded(ui_state.menu_selected==0);
}

static void _ui_menuDown(uint8_t menu_entries)
{
    if(ui_state.menu_selected < menu_entries - 1)
        ui_state.menu_selected += 1;
    else
        ui_state.menu_selected = 0;
    vp_playMenuBeepIfNeeded(ui_state.menu_selected==0);
}

static void _ui_menuBack(uint8_t prev_state)
{
    if(ui_state.edit_mode)
    {
        ui_state.edit_mode = false;
    }
    else
    {
        // Return to previous menu
        state.ui_screen = prev_state;
        // Reset menu selection
        ui_state.menu_selected = 0;
        vp_playMenuBeepIfNeeded(true);
    }
}

static void _ui_textInputReset(char *buf, size_t bufSize)
{
    ui_state.input_number = 0;
    ui_state.input_position = 0;
    ui_state.input_set = 0;
    ui_state.last_keypress = 0;
    memset(buf, 0, bufSize);
    buf[0] = '_';
}

static void _ui_textInputKeypad(char *buf, uint8_t max_len, kbd_msg_t msg,
                         bool callsign)
{
    long long now = getTick();
    // Get currently pressed number key
    uint8_t num_key = input_getPressedChar(msg);

    bool key_timeout = ((now - ui_state.last_keypress) >= input_longPressTimeout);
    bool same_key = ui_state.input_number == num_key;
    // Get number of symbols related to currently pressed key
    uint8_t num_symbols = 0;
    if(callsign)
    {
        num_symbols = strlen(symbols_ITU_T_E161_callsign[num_key]);
        if(num_symbols == 0)
            return;
    }
    else
        num_symbols = strlen(symbols_ITU_T_E161[num_key]);

    // Return if max length is reached or finished editing last character
    if((ui_state.input_position >= max_len) || ((ui_state.input_position == (max_len-1)) && (key_timeout || !same_key)))
        return;

    // Skip keypad logic for first keypress
    if(ui_state.last_keypress != 0)
    {
        // Same key pressed and timeout not expired: cycle over chars of current key
        if(same_key && !key_timeout)
        {
            ui_state.input_set = (ui_state.input_set + 1) % num_symbols;
        }
        // Different key pressed: save current char and change key
        else
        {
            ui_state.input_position += 1;
            ui_state.input_set = 0;
        }
    }
    // Show current character on buffer
    if(callsign)
        buf[ui_state.input_position] = symbols_ITU_T_E161_callsign[num_key][ui_state.input_set];
    else
    {
        buf[ui_state.input_position] = symbols_ITU_T_E161[num_key][ui_state.input_set];
    }
    // Announce the character
    vp_announceInputChar(buf[ui_state.input_position]);
    // Update reference values
    ui_state.input_number = num_key;
    ui_state.last_keypress = now;
}

static void _ui_textInputConfirm(char *buf)
{
    buf[ui_state.input_position + 1] = '\0';
}

static void _ui_textInputDel(char *buf)
{
    // announce the char about to be backspaced.
    // Note this assumes editing callsign.
    // If we edit a different buffer which allows the underline char, we may
    // not want to exclude it, but when editing callsign, we do not want to say
    // underline since it means the field is empty.
    if(buf[ui_state.input_position]
    && buf[ui_state.input_position]!='_')
        vp_announceInputChar(buf[ui_state.input_position]);

    buf[ui_state.input_position] = '\0';
    // Move back input cursor
    if(ui_state.input_position > 0)
    {
        ui_state.input_position--;
    // If we deleted the initial character, reset starting condition
    }
    else
        ui_state.last_keypress = 0;
    ui_state.input_set = 0;
}

static void _ui_numberInputKeypad(uint32_t *num, kbd_msg_t msg)
{
    long long now = getTick();

#ifdef CONFIG_UI_NO_KEYBOARD
    // If knob is turned, increment or Decrement
    if (msg.keys & KNOB_LEFT)
    {
        *num = *num + 1;
        if (*num % 10 == 0)
            *num = *num - 10;
    }

    if (msg.keys & KNOB_RIGHT)
    {
        if (*num == 0)
            *num = 9;
        else
        {
            *num = *num - 1;
            if (*num % 10 == 9)
                *num = *num + 10;
        }
    }

    // If enter is pressed, advance to the next digit
    if (msg.keys & KEY_ENTER)
    {
        uint32_t tmp = 0;
        if (!__builtin_mul_overflow(*num, 10, &tmp))
            *num = tmp;
    }

    // Announce the character
    vp_announceInputChar('0' + *num % 10);

    // Update reference values
    ui_state.input_number = *num % 10;
#else
    // Get currently pressed number key
    uint8_t num_key = input_getPressedNumber(msg);

    uint32_t tmp = 0;

    // Add a another digit onto the offset only if
    // it won't cause an overflow
    if (__builtin_mul_overflow(*num, 10, &tmp) ||
        __builtin_add_overflow(tmp, num_key, &tmp))
        return;

    *num = tmp;

    // Announce the character
    vp_announceInputChar('0' + num_key);

    // Update reference values
    ui_state.input_number = num_key;
#endif

    ui_state.last_keypress = now;
}

static void _ui_numberInputDel(uint32_t *num)
{
    // announce the digit about to be backspaced.
    vp_announceInputChar('0' + *num % 10);

    // Move back input cursor
    if(ui_state.input_position > 0)
        ui_state.input_position--;
    else
        ui_state.last_keypress = 0;

    ui_state.input_set = 0;
}

void ui_init()
{
#ifdef CONFIG_APRS
    /* The APRS service owns the station list, the mailbox and the receive tap:
     * it must be initialised before the first frame can arrive. */
    aprs_service_init();
#endif
#ifdef CONFIG_KISS
    /* The packet modem owns its thread, the KISS decoder and the transmit
     * queue; the transport, if it is the cable one, was bound at boot. */
    packet_service_init();
#endif
    last_event_tick = getTick();
    redraw_needed = true;
    _ui_calculateLayout(&layout);
    layout_ready = true;
    // Initialize struct ui_state to all zeroes
    // This syntax is called compound literal
    // https://stackoverflow.com/questions/6891720/initialize-reset-struct-to-zero-null
    ui_state = (const struct ui_state_t){ 0 };
}

void ui_drawSplashScreen()
{
    gfx_clearScreen();

    #if CONFIG_SCREEN_HEIGHT > 64
    static const point_t    logo_orig = {0, (CONFIG_SCREEN_HEIGHT / 2) - 6};
    static const point_t    call_orig = {0, CONFIG_SCREEN_HEIGHT - 8};
    static const fontSize_t logo_font = FONT_SIZE_12PT;
    static const fontSize_t call_font = FONT_SIZE_8PT;
    #else
    static const point_t    logo_orig = {0, 19};
    static const point_t    call_orig = {0, CONFIG_SCREEN_HEIGHT - 8};
    static const fontSize_t logo_font = FONT_SIZE_8PT;
    static const fontSize_t call_font = FONT_SIZE_6PT;
    #endif

    gfx_print(logo_orig, logo_font, TEXT_ALIGN_CENTER, yellow_fab413, "O P N\nR T X");
    gfx_print(call_orig, call_font, TEXT_ALIGN_CENTER, color_white, state.settings.callsign);

    vp_announceSplashScreen();
}

void ui_saveState()
{
    last_state = state;
}

/*
 * The keypad lock lives in the UI state: the quick menu needs to read and set
 * it, so it gets these two accessors instead of the whole structure.
 */
bool ui_getInputLock(void)
{
    return ui_state.input_locked;
}

void ui_setInputLock(bool on)
{
    ui_state.input_locked = on;
}

/*
 * Overlay entry point (quick menu, macro grid). Duplicated by hand in a dozen
 * places before: half of them forgot txDisable, and the ones that opened a
 * Settings page forgot the row. See ui_open_screen() in ui_default.h.
 */
void ui_menu_select(uint8_t index)
{
    ui_state.menu_selected = index;
}

void ui_open_screen(uint8_t screen, uint8_t item)
{
    /* Come back to the main screen the overlay was on, not to the first one */
    if ((state.ui_screen == MAIN_VFO) || (state.ui_screen == MAIN_MEM))
        ui_state.last_main_state = state.ui_screen;

    /* An overlay is never opened in the middle of a field edit, and a stale
       edit_mode would make the first key on the new screen a "leave edit"
       instead of a navigation: clear it here, once. */
    ui_state.edit_mode = false;
    ui_state.menu_selected = item;

    switch (screen) {
        case SPECTRUM:
            /*
             * The analyser can refuse: a digital mode needs the discriminator
             * continuously, and a receiver that is not receiving has no RSSI to
             * sweep. Refusing leaves the screen where it was, which is the
             * honest answer to "show me a spectrum of this mode".
             */
            if (ui_spectrum_enter() == false)
            {
                vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
                return;
            }
            break;

        case MENU_MESSAGES:
            ui_msgs_open();
            break;

        case MSG_COMPOSE:
            /* Compose with the transport of the current mode (msg_service) */
            ui_msgs_compose(NULL);
            break;

#ifdef CONFIG_APRS
        case APRS_CFG:
            ui_aprs_config();
            break;
#endif

#ifdef CONFIG_KISS
        case PKT_CFG:
            ui_packet_config();
            break;
#endif

        case MENU_CONTACTS:
            state.ui_screen = MENU_CONTACTS;
            ui_contacts_open();
            break;

        case MENU_ZONES:
            state.ui_screen = MENU_ZONES;
            ui_zones_open();
            break;

        default:
            state.ui_screen = screen;
            break;
    }

    /* Screens that own the keypad never transmit: same rule as the channels */
    bool ownsKeys = ui_channels_isOpen() || ui_contacts_isOpen()
                 || ui_zones_isOpen() || ui_msgs_isScreen(state.ui_screen)
                 || (state.ui_screen == SPECTRUM)
#ifdef CONFIG_APRS
                 || ui_aprs_isScreen(state.ui_screen)
#endif
#ifdef CONFIG_KISS
                 || ui_packet_isScreen(state.ui_screen)
#endif
                 ;
    if (state.txDisable != ownsKeys) {
        state.txDisable = ownsKeys;
        ui_rtxSyncRequest = true;
    }
}

#ifdef CONFIG_GPS
static uint16_t priorGPSSpeed = 0;
static int16_t  priorGPSAltitude = 0;
static int16_t  priorGPSDirection = 500; // impossible value init.
static uint8_t  priorGPSFixQuality= 0;
static uint8_t  priorGPSFixType = 0;
static uint8_t  priorSatellitesInView = 0;
static uint32_t vpGPSLastUpdate = 0;

static enum vpGPSInfoFlags GetGPSDirectionOrSpeedChanged()
{
    if (!state.settings.gps_enabled)
        return vpGPSNone;

    uint32_t now = getTick();
    if (now - vpGPSLastUpdate < 8000)
        return vpGPSNone;

    enum vpGPSInfoFlags whatChanged = vpGPSNone;

    if (state.gps_data.fix_quality != priorGPSFixQuality)
    {
        whatChanged |= vpGPSFixQuality;
        priorGPSFixQuality= state.gps_data.fix_quality;
    }

    if (state.gps_data.fix_type != priorGPSFixType)
    {
        whatChanged |= vpGPSFixType;
        priorGPSFixType = state.gps_data.fix_type;
    }

    if (state.gps_data.speed != priorGPSSpeed)
    {
        whatChanged |= vpGPSSpeed;
        priorGPSSpeed = state.gps_data.speed;
    }

    if (state.gps_data.altitude != priorGPSAltitude)
    {
        whatChanged |= vpGPSAltitude;
        priorGPSAltitude = state.gps_data.altitude;
    }

    if (state.gps_data.tmg_true != priorGPSDirection)
    {
        whatChanged |= vpGPSDirection;
        priorGPSDirection = state.gps_data.tmg_true;
    }

    if (state.gps_data.satellites_in_view != priorSatellitesInView)
    {
        whatChanged |= vpGPSSatCount;
        priorSatellitesInView = state.gps_data.satellites_in_view;
    }

    if (whatChanged)
        vpGPSLastUpdate=now;

    return whatChanged;
}
#endif // CONFIG_GPS

#ifdef CONFIG_VFO_SEARCH
/**
 * Periodic part of the VFO carrier search, run from ui_updateFSM() whether or
 * not a key arrives.
 *
 * Three jobs: arm the sweep when an arrow has been held for three seconds,
 * follow it on screen while it runs (the frequency changes under the operator,
 * so the screen has to be redrawn even with no event), and take the stop reason
 * off the screen once it has been there long enough.
 */
static void _ui_vfoSearchTick(long long now)
{
    if (vfoSearchService_active()) {
        if ((now - searchRedrawTick) >= 100) {
            searchRedrawTick = now;
            redraw_needed = true;
        }
        searchMsgShown = false;
        return;
    }

    /* The reason a finished search stopped stays up for a moment */
    struct vfoSearchStatus st;
    if (vfoSearchService_status(&st) && (st.result != VFOS_RESULT_NONE) &&
        (st.age < VFO_SEARCH_MESSAGE_MS)) {
        if ((now - searchRedrawTick) >= 250) {
            searchRedrawTick = now;
            redraw_needed = true;
        }
        searchMsgShown = true;
    }
    else if (searchMsgShown) {
        /* Expired: one last redraw takes it away */
        searchMsgShown = false;
        redraw_needed = true;
    }

    if (searchArmKey == 0)
        return;

    if ((now - searchArmTick) < VFO_SEARCH_HOLD_MS)
        return;

    uint8_t direction = (searchArmKey == KEY_UP) ? VFOS_UP : VFOS_DOWN;
    searchArmKey = 0;

    if (vfoSearchService_start(direction)) {
        searchRedrawTick = now;
        redraw_needed = true;
    }
}
#endif /* CONFIG_VFO_SEARCH */


/*
 * One rule for "may the PTT key the radio now".
 *
 * It used to be written by hand in seven places -- the six screens that own
 * the keypad and the tail of the dispatch -- and each one asked a different
 * question. The six asked "is my screen still on?" and so they re-armed the
 * transmitter whenever their red key went *back to the menu*: the operator
 * opened the inbox, pressed red, and the radio was keyable from the main menu
 * until the next key press. The rule is a property of the whole UI, not of a
 * screen: the PTT is armed on the two main screens only, and no overlay and no
 * menu is one of those.
 */
static void _ui_syncTxDisable(bool *sync_rtx)
{
    bool keyable = (state.ui_screen == MAIN_VFO) || (state.ui_screen == MAIN_MEM);

    if (keyable && (macro_menu || qm_state.open || ui_channels_isOpen()
                    || ui_contacts_isOpen() || ui_zones_isOpen()
                    || ui_tools_isOpen()
                    || ui_msgs_isScreen(state.ui_screen)
                    || ui_spectrum_isOpen()))
        keyable = false;

#ifdef CONFIG_APRS
    if (ui_aprs_isScreen(state.ui_screen))
        keyable = false;
#endif
#ifdef CONFIG_KISS
    if (ui_packet_isScreen(state.ui_screen))
        keyable = false;
#endif

    /* txDisable is the opposite: true means "do not transmit" */
    const bool wantDisabled = !keyable;

    if (state.txDisable != wantDisabled)
    {
        state.txDisable = wantDisabled;
        *sync_rtx = true;
    }
}

void ui_updateFSM(bool *sync_rtx)
{
    long long now = getTick();
    bool txOngoing = platform_getPttStatus();

    /*
     * Periodic part: it must run even when no key event arrives, otherwise the
     * quick menu timeout would only expire on the next keypress (which is
     * exactly the bug that made the 15 s auto-close dead code), and the
     * message poll would never refresh a delivery state on its own.
     */
    if(macro_latched && txOngoing)
    {
        macro_latched = false;
        macro_menu = false;
        redraw_needed = true;
    }

    /* The quick menu is an overlay: PTT closes it and keys the radio */
    if(qm_state.open && txOngoing)
    {
        qm_close();
        redraw_needed = true;
    }

    /* Auto-close the quick menu and follow mode changes made elsewhere */
    if(qm_tick(now))
        redraw_needed = true;

    /*
     * The analyser refreshes itself: a new sweep is a redraw, at the rate the
     * panel can take (ui_spectrum_tick() owns the limit), and a screen change
     * made behind its back takes the receiver with it.
     */
    if(ui_spectrum_tick(now))
        redraw_needed = true;

    /* Delivery states, received messages and the compose draft */
    ui_msgs_tick(now);

#ifdef CONFIG_VFO_SEARCH
    /* Carrier search: arm, follow and expire, with or without key events */
    _ui_vfoSearchTick(now);
#endif

#ifdef UI_MODE_TETRA
    /*
     * The "Cell uplink" session (protocols/TETRA/TetraUplink.h): while it
     * lasts the TETRA mode is on another frequency, and it has to give the
     * receiver back when the cell goes away, the operator changes mode or the
     * frequency is taken away. Both sides move together: the uplink frequency
     * of a cell must never be left as the transmit frequency of a mode that
     * can key the PA.
     */
    uint32_t ulBackHz = 0;
    if (tetraUplink_tick(state.channel.mode, state.channel.rx_frequency,
                         &ulBackHz)) {
        state.channel.rx_frequency = ulBackHz;
        state.channel.tx_frequency = ulBackHz;
        if (sync_rtx != NULL)
            *sync_rtx = true;
        redraw_needed = true;
    }
#endif

    // Check for events
    if(evQueue_wrPos == evQueue_rdPos) return;

    // Pop an event from the queue
    uint8_t newTail = (evQueue_rdPos + 1) % MAX_NUM_EVENTS;
    event_t event   = evQueue[evQueue_rdPos];
    evQueue_rdPos   = newTail;

    // There is some event to process, we need an UI redraw.
    // UI redraw request is cancelled if we're in standby mode.
    redraw_needed = true;
    if(standby) redraw_needed = false;

    // Check if battery has enough charge to operate.
    // Check is skipped if there is an ongoing transmission, since the voltage
    // drop caused by the RF PA power absorption causes spurious triggers of
    // the low battery alert.
#if !defined(PLATFORM_TTWRPLUS)
    if ((!state.emergency) && (!txOngoing) && (state.charge <= 0))
    {
        state.ui_screen = LOW_BAT;
        if(event.type == EVENT_KBD && event.payload)
        {
            state.ui_screen = MAIN_VFO;
            state.emergency = true;
        }
        return;
    }
#endif // PLATFORM_TTWRPLUS

    // Process pressed keys
    if(event.type == EVENT_KBD)
    {
        kbd_msg_t msg;
        msg.value = event.payload;
        if (msg.keys != 0)
            lastKeyEvent = event.payload;
        bool f1Handled = false;
        enum vpQueueFlags queueFlags = vp_getVoiceLevelQueueFlags();

        /* One event, one owner: reset the trace and let the block that takes
           the key say so (see KEY_CLAIM) */
        keyClaim  = UI_CLAIM_NONE;
        keyClaims = 0;

        // If we get out of standby, we ignore the kdb event
        // unless is the MONI key for the MACRO functions
        if (_ui_exitStandby(now) && !(msg.keys & KEY_MONI))
        {
            KEY_CLAIM(UI_CLAIM_STANDBY);
            return;
        }

#ifdef CONFIG_VFO_SEARCH
        /*
         * An armed long press is cancelled by letting go of the arrow before
         * the three seconds are up: the keypad reports the release as an event
         * with the remaining keys, so no keypad scan is needed here.
         */
        if ((searchArmKey != 0) &&
            ((msg.keys & (KEY_UP | KEY_DOWN)) != searchArmKey)) {
            searchArmKey = 0;
            redraw_needed = true;
        }

        /*
         * While a carrier search runs, the keypad belongs to it: any key -- a
         * second long press, a short one, the red key -- stops the sweep and
         * does nothing else, which is what the operator expects from a
         * receiver that is busy.  The PTT is handled by the service, which
         * sees the transmission from state_task().
         *
         * The event that reports the keys being *released* (keys == 0) is not
         * a keystroke: letting go of the arrow that started the sweep must not
         * stop it, so it is ignored here.
         */
        if (vfoSearchService_active() && (msg.keys != 0))
        {
            vfoSearchService_stop();
            searchArmKey  = 0;
            redraw_needed = true;
            if (state.settings.vpLevel == vpBeep)
                vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
            KEY_CLAIM(UI_CLAIM_SEARCH);
            return;
        }
#endif

        /*
         * A locked keypad has to be unlockable from the keypad. The LOCK row of
         * the quick menu sets the lock and the lock then blocks every key,
         * including the one that opens the menu: the only way out was a power
         * cycle, which is a one-way door and not a lock. A long press of red is
         * the way out, and it does nothing else while the lock is on.
         */
        if (ui_state.input_locked && (msg.keys & KEY_ESC) && msg.long_press)
        {
            ui_state.input_locked = false;
            if (state.settings.vpLevel > vpBeep)
                vp_play();
            redraw_needed = true;
            KEY_CLAIM(UI_CLAIM_UNLOCK);
            return;
        }

#ifdef CONFIG_APRS
        /*
         * Entry point to the APRS screens, and the one deliberate exception to
         * the key map: on the FM main screen a long press of P2 opens them. In
         * FM that long press used to open the macro grid; the side key (MONI)
         * still does, short or long, so nothing is lost. Everywhere else P2
         * keeps its meaning: short opens the quick menu, long opens the macro
         * grid, and nothing about APRS is reachable by accident.
         *
         * This has to be decided *before* the popup, and it has to close the
         * popup: the keypad reports the press and the long press as two
         * events, so the popup is already open -- it opens on the press edge --
         * by the time the long press arrives, and its handler takes every key
         * while it is up. That is why P2 held on the FM main screen only
         * latched the popup, and the station list was unreachable from the
         * keypad (tests/unit/ui_aprs_entry.cpp presses the two events in the
         * order the keypad delivers them).
         */
        if (((state.ui_screen == MAIN_VFO) || (state.ui_screen == MAIN_MEM))
            && (state.channel.mode == OPMODE_FM)
            && (msg.keys & KEY_F2) && msg.long_press
            && !ui_state.input_locked && !ui_state.edit_mode
            && !ui_channels_isOpen())
        {
            if (qm_state.open)
                qm_close();

            ui_aprs_enter();
            state.txDisable = true;
            *sync_rtx = true;
            redraw_needed = true;
            KEY_CLAIM(UI_CLAIM_APRS_ENTER);
            return;
        }
#endif /* CONFIG_APRS */

        /*
         * Contextual quick menu (P2, KEY_F2). Short press opens the popup on
         * the main screens; while it is open it owns the keypad: UP/DOWN move
         * or change the value, ENTER edits, ESC/P2 close it, digits jump.
         */
        if(qm_state.open)
        {
            qm_state.lastKeyTick = now;
            if(msg.keys & (KEY_UP | KNOB_LEFT))
                qm_move(-1);
            else if(msg.keys & (KEY_DOWN | KNOB_RIGHT))
                qm_move(+1);
            else if(msg.keys & KEY_F1)
                qm_adjust_step(-1, msg.long_press);   /* P1: value down */
            else if(msg.keys & KEY_F2)
            {
                /* P2 short: value up. P2 long: latch the popup open. */
                if(msg.long_press)
                    qm_toggleLatch();
                else
                    qm_adjust_step(+1, false);
            }
            else if(msg.keys & KEY_ENTER)
                qm_enter();
            else if(msg.keys & KEY_ESC)
                qm_cancel();
            else if(input_isNumberPressed(msg))
            {
                uint8_t digit = input_getPressedNumber(msg);
                qm_select(digit == 0 ? 9 : digit - 1);
            }
            if(event.payload != 0)
                vp_play();
            KEY_CLAIM(UI_CLAIM_QM);
            return;
        }

        /*
         * The analyser's screen owns every key while it is on, the way a list
         * owns them: it consumes them all (so nothing below can act on a key it
         * did not mean) and the popup, which is opened from inside it, has
         * already had its turn above.
         */
        if ((state.ui_screen == SPECTRUM) && (ui_channels_isOpen() == false))
        {
            if (ui_spectrum_handleEvent(&msg, sync_rtx))
            {
                redraw_needed = true;
                KEY_CLAIM(UI_CLAIM_SPECTRUM);
                return;
            }
        }

        if((msg.keys & KEY_F2) && !msg.long_press
           && ((state.ui_screen == MAIN_VFO) || (state.ui_screen == MAIN_MEM))
           && (ui_state.input_locked == false) && (ui_state.edit_mode == false)
           && (ui_channels_isOpen() == false))
        {
            qm_open();
            KEY_CLAIM(UI_CLAIM_QM_OPEN);
            return;
        }

#ifdef CONFIG_APRS
        /*
         * APRS screens own the keypad while one of them is on screen, the way
         * the channel list does: they consume every key (ui_aprs_fsm() returns
         * true for all of them) and the radio does not transmit on the PTT
         * from a menu. The dispatch sits before the main switch so that the
         * menu rework and the APRS screens stay independent, but after the
         * quick menu, which keeps the keys while it is open.
         */
        if (ui_aprs_isScreen(state.ui_screen))
        {
            if (ui_aprs_fsm(msg, sync_rtx))
            {
                /* ESC can leave APRS for the VFO: re-arm the PTT in that case */
                _ui_syncTxDisable(sync_rtx);
                redraw_needed = true;
                KEY_CLAIM(UI_CLAIM_APRS);
                return;
            }
        }
#endif /* CONFIG_APRS */

#ifdef CONFIG_KISS
        /* The packet modem page owns the keypad like the APRS screens: the
         * radio must not transmit on the PTT while it is being configured. */
        if (ui_packet_isScreen(state.ui_screen))
        {
            if (ui_packet_fsm(msg, sync_rtx))
            {
                _ui_syncTxDisable(sync_rtx);
                redraw_needed = true;
                KEY_CLAIM(UI_CLAIM_PACKET);
                return;
            }
        }
#endif
        /*
         * Messaging screens (ui_messages.c) and the contact book
         * (ui_contacts.c) own the keypad and keep the radio from
         * transmitting, exactly like the channel list and the APRS screens.
         * They sit after the quick menu and before the macro grid, so P2 long
         * keeps opening the macro grid on the main screens.
         */
        if (ui_msgs_isScreen(state.ui_screen))
        {
            ui_msgs_handleEvent(&msg, sync_rtx);
            _ui_syncTxDisable(sync_rtx);
            redraw_needed = true;
            KEY_CLAIM(UI_CLAIM_MSGS);
            return;
        }

        if ((state.ui_screen == MENU_CONTACTS) && ui_contacts_isOpen())
        {
            ui_contacts_handleEvent(&msg, sync_rtx);
            _ui_syncTxDisable(sync_rtx);
            redraw_needed = true;
            KEY_CLAIM(UI_CLAIM_CONTACTS);
            return;
        }

        /* Zone browser: same contract as the channel list */
        if ((state.ui_screen == MENU_ZONES) && ui_zones_isOpen())
        {
            ui_zones_handleEvent(&msg, sync_rtx);
            _ui_syncTxDisable(sync_rtx);
            redraw_needed = true;
            KEY_CLAIM(UI_CLAIM_ZONES);
            return;
        }

        /*
         * Tools menu (P1 held down on a main screen, ui_tools.c): the list of
         * things that had no key of their own and the mode selector. It owns
         * every key while it is up -- MONI and the long P2 included -- which is
         * why this block goes before the macro grid: a menu that hands a key to
         * an overlay the operator cannot see is a key that goes nowhere.
         */
        if (ui_tools_isOpen())
        {
            ui_tools_handleEvent(&msg, sync_rtx);
            /* The row may have opened the channel list or the analyser: the
               one rule re-arms the PTT only when nothing owns it anymore */
            _ui_syncTxDisable(sync_rtx);
            redraw_needed = true;
            KEY_CLAIM(UI_CLAIM_TOOLS);
            return;
        }

        /*
         * Channel list and editor consume every key while they are open.
         *
         * This block used to sit after the macro grid below, which meant the
         * two keys the grid claims -- MONI and a long P2 -- never reached the
         * module: a long P2 in the list armed the macro grid behind a screen
         * drawn on top of it, so the delete confirmation of docs/CHANNELS.md
         * could not be reached from the keypad at all and the operator's next
         * key went to a menu he could not see. Every other overlay (the tools
         * menu, the messages screens, the contacts, the zones) is dispatched
         * before the grid for the same reason; the list is no different.
         */
        if (ui_channels_isOpen())
        {
            ui_channels_handleEvent(&msg, sync_rtx);
            _ui_syncTxDisable(sync_rtx);

            KEY_CLAIM(UI_CLAIM_CHANNELS);
            return;
        }

        // If MONI is pressed, activate MACRO functions; P2 long keeps the
        // legacy 3x3 grid as its shortcut everywhere APRS does not claim it
        bool moniPressed = (msg.keys & KEY_MONI)
                        || ((msg.keys & KEY_F2) && msg.long_press);
        if(moniPressed || macro_latched)
        {
            macro_menu = true;

            if(state.settings.macroMenuLatch == 1)
            {
                // long press moni on its own latches function.
                if (moniPressed && msg.long_press && !macro_latched)
                {
                    macro_latched = true;
                    vp_beep(BEEP_FUNCTION_LATCH_ON, LONG_BEEP);
                }
                else if (moniPressed && macro_latched)
                {
                    macro_latched = false;
                    vp_beep(BEEP_FUNCTION_LATCH_OFF, LONG_BEEP);
                }
            }

            _ui_fsm_menuMacro(msg, sync_rtx);
            KEY_CLAIM(UI_CLAIM_MACRO);
            return;
        }
        else
        {
            macro_menu = false;
        }
#if defined(PLATFORM_TTWRPLUS)
        // T-TWR Plus has no KEY_MONI, using KEY_VOLDOWN long press instead
        if ((msg.keys & KEY_VOLDOWN) && msg.long_press)
        {
            macro_menu = true;
            macro_latched = true;
        }
#endif // PLA%FORM_TTWRPLUS

        if(state.tone_enabled && !(msg.keys & KEY_HASH))
        {
            state.tone_enabled = false;
            *sync_rtx = true;
        }

        /*
         * P1 long press: open the tools menu (ui_tools.c). It used to open the
         * channel list directly; the list is now the last row of the menu, so
         * the shortcut survives and the scanner, the carrier search, the
         * analyser and the mode selector get a door. The module takes over the
         * keypad and the screen until it closes; see docs/UI.md.
         */
        if (!ui_tools_isOpen() && (msg.keys & KEY_F1) && msg.long_press &&
            !ui_state.input_locked &&
            (state.ui_screen == MAIN_VFO || state.ui_screen == MAIN_MEM))
        {
            ui_tools_open();
            /*
             * The menu owns the keypad from this moment, so it also owns the
             * transmitter: the PTT is not a key event and would otherwise key
             * the radio while a menu is on screen. ui_tools_open() sets
             * txDisable, and this path returns before the block below could
             * re-arm it.
             */
            if (state.txDisable != true)
            {
                state.txDisable = true;
                *sync_rtx = true;
            }
            redraw_needed = true;
            KEY_CLAIM(UI_CLAIM_TOOLS_OPEN);
            return;
        }

        int priorUIScreen = state.ui_screen;

        /* From here on the screen's own case takes the key */
        KEY_CLAIM(UI_CLAIM_SCREEN);

        switch(state.ui_screen)
        {
            // VFO screen
            case MAIN_VFO:
            {
                // Enable Tx in MAIN_VFO mode
                if (state.txDisable)
                {
                    state.txDisable = false;
                    *sync_rtx = true;
                }

                // Break out of the FSM if the keypad is locked but allow the
                // use of the hash key in FM mode for the 1750Hz tone.
                bool skipLock =  (state.channel.mode == OPMODE_FM)
                              && (msg.keys == KEY_HASH);

                if ((ui_state.input_locked == true) && (skipLock == false))
                    break;

                /*
                 * On a main screen the only field that can be edited is the M17
                 * destination, and only M17's key (#) enters the edit. A mode
                 * change with that field still being edited left the screen
                 * deaf to every key: the edit branch below has no exit for any
                 * other mode (its handlers are inside the M17 build), so P2
                 * stopped opening the quick menu, the digits never reached the
                 * frequency input and a long press of an arrow never armed the
                 * carrier search. Drop a stale edit instead.
                 */
                if (ui_state.edit_mode && (state.channel.mode != OPMODE_M17))
                    ui_state.edit_mode = false;

                if(ui_state.edit_mode)
                {
                    #ifdef CONFIG_M17
                    if(state.channel.mode == OPMODE_M17)
                    {
                        if(msg.keys & KEY_ENTER)
                        {
                            _ui_textInputConfirm(ui_state.new_callsign);
                            // Save selected dst ID and disable input mode
                            strncpy(state.settings.m17_dest, ui_state.new_callsign, 10);
                            ui_state.edit_mode = false;
                            *sync_rtx = true;
                            vp_announceM17Info(NULL,  ui_state.edit_mode,
                                               queueFlags);
                        }
                        else if(msg.keys & KEY_HASH)
                        {
                            // Save selected dst ID and disable input mode
                            strncpy(state.settings.m17_dest, "", 1);
                            ui_state.edit_mode = false;
                            *sync_rtx = true;
                            vp_announceM17Info(NULL,  ui_state.edit_mode,
                                               queueFlags);
                        }
                        else if(msg.keys & KEY_ESC)
                            // Discard selected dst ID and disable input mode
                            ui_state.edit_mode = false;
                        else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            _ui_textInputDel(ui_state.new_callsign);
                        else if(input_isCharPressed(msg))
                            _ui_textInputKeypad(ui_state.new_callsign, 9, msg, true);
                        break;
                    }
                    #endif
                }
                else
                {
#ifdef CONFIG_VFO_SEARCH
                    /*
                     * A long press of UP or DOWN on the VFO screen arms the
                     * carrier search.  It is not a tuning step: the key has to
                     * be held for three seconds (see _ui_vfoSearchTick()) and
                     * the sweep starts from the frequency on screen.
                     */
                    if((msg.keys & (KEY_UP | KEY_DOWN)) && msg.long_press)
                    {
                        searchArmKey  = (msg.keys & KEY_UP) ? KEY_UP : KEY_DOWN;
                        searchArmTick = now - input_longPressTimeout;
                        redraw_needed = true;
                    }
                    else
#endif
                    if(msg.keys & KEY_ENTER)
                    {
                        // Save current main state
                        ui_state.last_main_state = state.ui_screen;
                        // Open Menu
                        state.ui_screen = MENU_TOP;
                        // The selected item will be announced when the item is first selected.
                    }
                    else if(msg.keys & KEY_ESC)
                    {
                        // Save VFO channel
                        state.vfo_channel = state.channel;
                        int result = _ui_fsm_loadChannel(state.channel_index, sync_rtx);
                        // Read successful and channel is valid
                        if(result != -1)
                        {
                            // Switch to MEM screen
                            state.ui_screen = MAIN_MEM;
                            // anounce the active channel name.
                            vp_announceChannelName(&state.channel,
                                                   state.channel_index,
                                                   queueFlags);
                        }
                    }
                    else if(msg.keys & KEY_HASH)
                    {
                        #ifdef CONFIG_M17
                        // Only enter edit mode when using M17
                        if(state.channel.mode == OPMODE_M17)
                        {
                            // Enable dst ID input
                            ui_state.edit_mode = true;
                            // Reset text input variables
                            _ui_textInputReset(ui_state.new_callsign,
                                    sizeof(ui_state.new_callsign));
                            vp_announceM17Info(NULL,  ui_state.edit_mode,
                                               queueFlags);
                        }
                        else
                        #endif
                        {
                            if(!state.tone_enabled)
                            {
                                state.tone_enabled = true;
                                *sync_rtx = true;
                            }
                        }
                    }
                    else if(msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
                    {
                        // Increment TX and RX frequency by the current step
                        if(_ui_freq_check_limits(state.channel.rx_frequency + freq_steps[state.step_index]) &&
                           _ui_freq_check_limits(state.channel.tx_frequency + freq_steps[state.step_index]))
                        {
                            freq_t rx = state.channel.rx_frequency
                                      + freq_steps[state.step_index];
                            freq_t tx = state.channel.tx_frequency
                                      + freq_steps[state.step_index];

                            /* Airband: land on the raster after the step */
                            _ui_airbandSnap(&rx);
                            if (rx == tx)
                                tx = rx;
                            else
                                _ui_airbandSnap(&tx);

                            state.channel.rx_frequency = rx;
                            state.channel.tx_frequency = tx;
                            *sync_rtx = true;
                            vp_announceFrequencies(state.channel.rx_frequency,
                                                   state.channel.tx_frequency,
                                                   queueFlags);
                        }
                    }
                    else if(msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
                    {
                        // Decrement TX and RX frequency by the current step
                        if(_ui_freq_check_limits(state.channel.rx_frequency - freq_steps[state.step_index]) &&
                           _ui_freq_check_limits(state.channel.tx_frequency - freq_steps[state.step_index]))
                        {
                            freq_t rx = state.channel.rx_frequency
                                      - freq_steps[state.step_index];
                            freq_t tx = state.channel.tx_frequency
                                      - freq_steps[state.step_index];

                            _ui_airbandSnap(&rx);
                            if (rx == tx)
                                tx = rx;
                            else
                                _ui_airbandSnap(&tx);

                            state.channel.rx_frequency = rx;
                            state.channel.tx_frequency = tx;
                            *sync_rtx = true;
                            vp_announceFrequencies(state.channel.rx_frequency,
                                                   state.channel.tx_frequency,
                                                   queueFlags);
                        }
                    }
                    else if(msg.keys & KEY_F1)
                    {
                        if (state.settings.vpLevel > vpBeep)
                        {// quick press repeat vp, long press summary.
                            if (msg.long_press)
                                vp_announceChannelSummary(&state.channel, 0,
                                                          state.bank, vpAllInfo);
                            else
                                vp_replayLastPrompt();
                            f1Handled = true;
                        }
                    }
                    else if(input_isNumberPressed(msg))
                    {
                        // Open Frequency input screen
                        state.ui_screen = MAIN_VFO_INPUT;
                        // Reset input position and selection
                        ui_state.input_position = 1;
                        ui_state.input_set = SET_RX;
                        // do not play  because we will also announce the number just entered.
                        vp_announceInputReceiveOrTransmit(false, vpqInit);
                        vp_queueInteger(input_getPressedNumber(msg));
                        vp_play();

                        ui_state.new_rx_frequency = 0;
                        ui_state.new_tx_frequency = 0;
                        // Save pressed number to calculare frequency and show in GUI
                        ui_state.input_number = input_getPressedNumber(msg);
                        // Calculate portion of the new frequency
                        ui_state.new_rx_frequency = _ui_freq_add_digit(ui_state.new_rx_frequency,
                                                                       ui_state.input_position,
                                                                       ui_state.input_number);
                    }
                }
            }
                break;
            // VFO frequency input screen
            case MAIN_VFO_INPUT:
                if(msg.keys & KEY_ENTER)
                {
                    _ui_fsm_confirmVFOInput(sync_rtx);
                }
                else if(msg.keys & KEY_ESC)
                {
                    // Cancel frequency input, return to VFO mode
                    state.ui_screen = MAIN_VFO;
                }
                else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN)
                {
                    if(ui_state.input_set == SET_RX)
                    {
                        ui_state.input_set = SET_TX;
                        vp_announceInputReceiveOrTransmit(true, queueFlags);
                    }
                    else if(ui_state.input_set == SET_TX)
                    {
                        ui_state.input_set = SET_RX;
                        vp_announceInputReceiveOrTransmit(false, queueFlags);
                    }
                    // Reset input position
                    ui_state.input_position = 0;
                }
                else if(input_isNumberPressed(msg))
                {
                    _ui_fsm_insertVFONumber(msg, sync_rtx);
                }
                break;
            // MEM screen
            case MAIN_MEM:
                // Enable Tx in MAIN_MEM mode
                if (state.txDisable)
                {
                    state.txDisable = false;
                    *sync_rtx = true;
                }
                if (ui_state.input_locked)
                    break;

                /* Same stale-edit guard as MAIN_VFO: only M17 edits here */
                if (ui_state.edit_mode && (state.channel.mode != OPMODE_M17))
                    ui_state.edit_mode = false;

                // M17 Destination callsign input
                if(ui_state.edit_mode)
                {
                    {
                        if(msg.keys & KEY_ENTER)
                        {
                            _ui_textInputConfirm(ui_state.new_callsign);
                            // Save selected dst ID and disable input mode
                            strncpy(state.settings.m17_dest, ui_state.new_callsign, 10);
                            ui_state.edit_mode = false;
                            *sync_rtx = true;
                        }
                        else if(msg.keys & KEY_HASH)
                        {
                            // Save selected dst ID and disable input mode
                            strncpy(state.settings.m17_dest, "", 1);
                            ui_state.edit_mode = false;
                            *sync_rtx = true;
                        }
                        else if(msg.keys & KEY_ESC)
                            // Discard selected dst ID and disable input mode
                            ui_state.edit_mode = false;
                        else if(msg.keys & KEY_F1)
                        {
                            if (state.settings.vpLevel > vpBeep)
                            {
                                // Quick press repeat vp, long press summary.
                                if (msg.long_press)
                                {
                                    vp_announceChannelSummary(
                                            &state.channel,
                                            state.channel_index,
                                            state.bank,
                                            vpAllInfo);
                                }
                                else
                                {
                                    vp_replayLastPrompt();
                                }

                                f1Handled = true;
                            }
                        }
                        else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            _ui_textInputDel(ui_state.new_callsign);
                        else if(input_isCharPressed(msg))
                            _ui_textInputKeypad(ui_state.new_callsign, 9, msg, true);
                        break;
                    }
                }
                else
                {
                    if(msg.keys & KEY_ENTER)
                    {
                        // Save current main state
                        ui_state.last_main_state = state.ui_screen;
                        // Open Menu
                        state.ui_screen = MENU_TOP;
                    }
                    else if(msg.keys & KEY_ESC)
                    {
                        // Restore VFO channel
                        state.channel = state.vfo_channel;
                        // Update RTX configuration
                        *sync_rtx = true;
                        // Switch to VFO screen
                        state.ui_screen = MAIN_VFO;
                    }
                    else if(msg.keys & KEY_HASH)
                    {
                        // Only enter edit mode when using M17
                        if(state.channel.mode == OPMODE_M17)
                        {
                            // Enable dst ID input
                            ui_state.edit_mode = true;
                            // Reset text input variables
                            _ui_textInputReset(ui_state.new_callsign,
                                    sizeof(ui_state.new_callsign));
                        }
                        else
                        {
                            if(!state.tone_enabled)
                            {
                                state.tone_enabled = true;
                                *sync_rtx = true;
                            }
                        }
                    }
                    else if(msg.keys & KEY_F1)
                    {
                        if (state.settings.vpLevel > vpBeep)
                        {// quick press repeat vp, long press summary.
                            if (msg.long_press)
                            {
                                vp_announceChannelSummary(&state.channel,
                                                          state.channel_index+1,
                                                          state.bank, vpAllInfo);
                            }
                            else
                            {
                                vp_replayLastPrompt();
                            }

                            f1Handled = true;
                        }
                    }
                    else if(msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
                    {
                        _ui_fsm_loadChannel(state.channel_index + 1, sync_rtx);
                        vp_announceChannelName(&state.channel,
                                               state.channel_index+1,
                                               queueFlags);
                    }
                    else if(msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
                    {
                        _ui_fsm_loadChannel(state.channel_index - 1, sync_rtx);
                        vp_announceChannelName(&state.channel,
                                               state.channel_index+1,
                                               queueFlags);
                    }
                }
                break;
            // Top menu screen
            case MENU_TOP:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(menu_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(menu_num);
                else if(msg.keys & KEY_ENTER)
                {
                    switch(ui_state.menu_selected)
                    {
                        case M_BANK:
                            state.ui_screen = MENU_BANK;
                            break;
                        case M_CHANNEL:
#if defined(PLATFORM_C62)
                            /* No codeplug on this radio: the CPS list would be
                               empty. Open the in-radio bank editor instead;
                               the F1 long shortcut still works too. */
                            ui_channels_open();
                            /* No TX while the editor owns the screen */
                            state.txDisable = true;
                            *sync_rtx = true;
                            redraw_needed = true;
#else
                            state.ui_screen = MENU_CHANNEL;
#endif
                            break;
                        case M_ZONES:
                            ui_open_screen(MENU_ZONES, 0);
                            *sync_rtx = true;
                            break;
                        case M_CONTACTS:
                            state.ui_screen = MENU_CONTACTS;
                            ui_contacts_open();
                            state.txDisable = true;
                            *sync_rtx = true;
                            break;
                        case M_MESSAGES:
                            ui_msgs_open();
                            state.txDisable = true;
                            *sync_rtx = true;
                            break;
#ifdef CONFIG_GPS
                        case M_GPS:
                            state.ui_screen = MENU_GPS;
                            break;
#endif
                        case M_SETTINGS:
                            state.ui_screen = MENU_SETTINGS;
                            break;
                        case M_BACKUP_RESTORE:
                            /*
                             * The internal backup of the stores: two rows,
                             * backup and restore (docs/BACKUP.md). The screens
                             * behind them are this module's, ui_backup.c.
                             */
                            state.ui_screen = MENU_BACKUP_RESTORE;
                            break;
                        case M_INFO:
                            state.ui_screen = MENU_INFO;
                            break;
                        case M_ABOUT:
                            state.ui_screen = MENU_ABOUT;
                            break;
                    }
                    // Reset menu selection
                    ui_state.menu_selected = 0;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(ui_state.last_main_state);
                break;
            // Zone menu screen
            case MENU_BANK:
            // Channel menu screen
            case MENU_CHANNEL:
            // Contacts menu screen
            case MENU_CONTACTS:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    // Using 1 as parameter disables menu wrap around
                    _ui_menuUp(1);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                {
                    if(state.ui_screen == MENU_BANK)
                    {
                        bankHdr_t bank;
                        // manu_selected is 0-based
                        // bank 0 means "All Channel" mode
                        // banks (1, n) are mapped to banks (0, n-1)
                        if(cps_readBankHeader(&bank, ui_state.menu_selected) != -1)
                            ui_state.menu_selected += 1;
                    }
                    else if(state.ui_screen == MENU_CHANNEL)
                    {
                        channel_t channel;
                        if(cps_readChannel(&channel, ui_state.menu_selected + 1) != -1)
                            ui_state.menu_selected += 1;
                    }
                    else if(state.ui_screen == MENU_CONTACTS)
                    {
                        contact_t contact;
                        if(cps_readContact(&contact, ui_state.menu_selected + 1) != -1)
                            ui_state.menu_selected += 1;
                    }
                }
                else if(msg.keys & KEY_ENTER)
                {
                    if(state.ui_screen == MENU_BANK)
                    {
                        bankHdr_t newbank;
                        int result = 0;
                        // If "All channels" is selected, load default bank
                        if(ui_state.menu_selected == 0)
                            state.bank_enabled = false;
                        else
                        {
                            state.bank_enabled = true;
                            result = cps_readBankHeader(&newbank, ui_state.menu_selected - 1);
                            /*
                             * Only with a bank: on "All channels" the index used
                             * to be 0 - 1, which wraps round to 255 in a
                             * uint8_t and left state.bank pointing nowhere.
                             */
                            if(result != -1)
                                state.bank = ui_state.menu_selected - 1;
                        }
                        if(result != -1)
                        {
                            // If we were in VFO mode, save VFO channel
                            if(ui_state.last_main_state == MAIN_VFO)
                                state.vfo_channel = state.channel;
                            // Load bank first channel
                            _ui_fsm_loadChannel(0, sync_rtx);
                            // Switch to MEM screen
                            state.ui_screen = MAIN_MEM;
                        }
                    }
                    if(state.ui_screen == MENU_CHANNEL)
                    {
                        // If we were in VFO mode, save VFO channel
                        if(ui_state.last_main_state == MAIN_VFO)
                            state.vfo_channel = state.channel;
                        _ui_fsm_loadChannel(ui_state.menu_selected, sync_rtx);
                        // Switch to MEM screen
                        state.ui_screen = MAIN_MEM;
                    }
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
#ifdef CONFIG_GPS
            // GPS menu screen
            case MENU_GPS:
                if ((msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
                {// quick press repeat vp, long press summary.
                    if (msg.long_press)
                        vp_announceGPSInfo(vpGPSAll);
                    else
                        vp_replayLastPrompt();
                    f1Handled = true;
                }
                else if(msg.keys & KEY_ENTER)
                    _ui_menuBack(MENU_TOP);   /* green is "OK" on the bar */
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
#endif
            // Settings menu screen
            case MENU_SETTINGS:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_num);
                else if(msg.keys & KEY_ENTER)
                {

                    switch(ui_state.menu_selected)
                    {
                        case S_DISPLAY:
                            state.ui_screen = SETTINGS_DISPLAY;
                            break;
#ifdef CONFIG_RTC
                        case S_TIMEDATE:
                            state.ui_screen = SETTINGS_TIMEDATE;
                            break;
#endif
#ifdef CONFIG_GPS
                        case S_GPS:
                            state.ui_screen = SETTINGS_GPS;
                            break;
#endif
                        case S_RADIO:
                            state.ui_screen = SETTINGS_RADIO;
                            break;
                        case S_IDENTITY:
                            state.ui_screen = SETTINGS_IDENTITY;
                            break;
                        case S_AUDIO:
                            state.ui_screen = SETTINGS_AUDIO;
                            break;
#ifdef UI_MODE_M17
                        case S_M17:
                            state.ui_screen = SETTINGS_M17;
                            break;
#endif
#ifdef CONFIG_TETRA_AFC
                        case S_TETRA:
                            state.ui_screen = SETTINGS_TETRA;
                            break;
#endif
                        case S_FM:
                            state.ui_screen = SETTINGS_FM;
                            break;
#define UI_MODE_PAGE(id, title)                     \
                        case S_PAGE_##id:           \
                            qm_page_open(id);       \
                            state.ui_screen = SETTINGS_MODE; \
                            break;
                        UI_MODE_PAGES(UI_MODE_PAGE)
#undef UI_MODE_PAGE
                        case S_ACCESSIBILITY:
                            state.ui_screen = SETTINGS_ACCESSIBILITY;
                            break;
                        case S_RESET2DEFAULTS:
                            state.ui_screen = SETTINGS_RESET2DEFAULTS;
                            break;
                        default:
                            state.ui_screen = MENU_SETTINGS;
                    }
                    // Reset menu selection
                    ui_state.menu_selected = 0;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
            // Flash backup and restore menu screen
            case MENU_BACKUP_RESTORE:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(backup_restore_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(backup_restore_num);
                else if(msg.keys & KEY_ENTER)
                {

                    switch(ui_state.menu_selected)
                    {
                        case BR_BACKUP:
                            state.ui_screen = MENU_BACKUP;
                            ui_backup_open(MENU_BACKUP);
                            break;
                        case BR_RESTORE:
                            state.ui_screen = MENU_RESTORE;
                            ui_backup_open(MENU_RESTORE);
                            break;
                        default:
                            state.ui_screen = MENU_BACKUP_RESTORE;
                    }
                    // Reset menu selection
                    ui_state.menu_selected = 0;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
            /*
             * Taking a backup and restoring it (ui_backup.c, docs/BACKUP.md).
             * The module owns both screens: it asks before a restore and keeps
             * the result on the screen, so the dispatch only forwards the keys.
             */
            case MENU_BACKUP:
            case MENU_RESTORE:
                ui_backup_handleEvent(state.ui_screen, &msg, sync_rtx);
                break;
            // Info menu screen
            case MENU_INFO:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(info_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(info_num);
                else if(msg.keys & KEY_ENTER)
                    _ui_menuBack(MENU_TOP);   /* green is "OK" on the bar */
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
            // About screen, scroll without rollover
            case MENU_ABOUT:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                {
                    if(ui_state.menu_selected > 0)
                        ui_state.menu_selected -= 1;
                }
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                {
                    /*
                     * Bounded, like MENU_INFO: the list of authors is a fixed
                     * array and the draw indexes it with this value.  Left to
                     * grow, the index ran past the array as soon as the panel
                     * showed more rows than there are authors.
                     */
                    if(ui_state.menu_selected + 1 < author_num)
                        ui_state.menu_selected += 1;
                }
                else if(msg.keys & KEY_ENTER)
                    _ui_menuBack(MENU_TOP);   /* green is "OK" on the bar */
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
#ifdef CONFIG_RTC
            // Time&Date settings screen
            case SETTINGS_TIMEDATE:
                if(msg.keys & KEY_ENTER)
                {
                    // Switch to set Time&Date mode
                    state.ui_screen = SETTINGS_TIMEDATE_SET;
                    // Reset input position and selection
                    ui_state.input_position = 0;
                    memset(&ui_state.new_timedate, 0, sizeof(datetime_t));
                    vp_announceBuffer(&currentLanguage->timeAndDate,
                                      true, false, "dd/mm/yy");
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
            // Time&Date settings screen, edit mode
            case SETTINGS_TIMEDATE_SET:
                if(msg.keys & KEY_ENTER)
                {
                    // Save time only if all digits have been inserted
                    if(ui_state.input_position < TIMEDATE_DIGITS)
                        break;
                    // Return to Time&Date menu, saving values
                    // NOTE: The user inserted a local time, we must save an UTC time
                    datetime_t utc_time = localTimeToUtc(ui_state.new_timedate,
                                                         state.settings.utc_timezone);
                    platform_setTime(utc_time);
                    state.time = utc_time;
                    vp_announceSettingsTimeDate();
                    state.ui_screen = SETTINGS_TIMEDATE;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(SETTINGS_TIMEDATE);
                else if(input_isNumberPressed(msg))
                {
                    // Discard excess digits
                    if(ui_state.input_position > TIMEDATE_DIGITS)
                        break;
                    ui_state.input_position += 1;
                    ui_state.input_number = input_getPressedNumber(msg);
                    _ui_timedate_add_digit(&ui_state.new_timedate, ui_state.input_position,
                                            ui_state.input_number);
                }
                break;
#endif
            case SETTINGS_DISPLAY:
                if(msg.keys & KEY_LEFT || (ui_state.edit_mode &&
                   (msg.keys & (KEY_DOWN | KEY_F1 | KNOB_LEFT))))
                {
                    switch(ui_state.menu_selected)
                    {
#ifdef CONFIG_SCREEN_BRIGHTNESS
                        case D_BRIGHTNESS:
                            _ui_changeBrightness(-5);
                            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                                   state.settings.brightness);
                            break;
#endif
#ifdef CONFIG_SCREEN_CONTRAST
                        case D_CONTRAST:
                            _ui_changeContrast(-4);
                            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                                   state.settings.contrast);
                            break;
#endif
                        case D_TIMER:
                            _ui_changeTimer(-1);
                            vp_announceDisplayTimer();
                            break;
                        case D_BATTERY:
                            state.settings.showBatteryIcon = !state.settings.showBatteryIcon;
                            break;
#ifdef CONFIG_KEY_BACKLIGHT
                        case D_KEYLIGHT:
                            state.settings.keyBacklight =
                                !state.settings.keyBacklight;
                            c62_setKeyBacklight(state.settings.keyBacklight);
                            vp_announceSettingsOnOffToggle(
                                &currentLanguage->brightness, queueFlags,
                                state.settings.keyBacklight);
                            break;
#endif
                        default:
                            state.ui_screen = SETTINGS_DISPLAY;
                    }
                }
                else if(msg.keys & KEY_RIGHT || (ui_state.edit_mode &&
                        (msg.keys & (KEY_UP | KEY_F2 | KNOB_RIGHT))))
                {
                    switch(ui_state.menu_selected)
                    {
#ifdef CONFIG_SCREEN_BRIGHTNESS
                        case D_BRIGHTNESS:
                            _ui_changeBrightness(+5);
                            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                                   state.settings.brightness);
                            break;
#endif
#ifdef CONFIG_SCREEN_CONTRAST
                        case D_CONTRAST:
                            _ui_changeContrast(+4);
                            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                                   state.settings.contrast);
                            break;
#endif
                        case D_TIMER:
                            _ui_changeTimer(+1);
                            vp_announceDisplayTimer();
                            break;
#ifdef CONFIG_KEY_BACKLIGHT
                        case D_KEYLIGHT:
                            state.settings.keyBacklight =
                                !state.settings.keyBacklight;
                            c62_setKeyBacklight(state.settings.keyBacklight);
                            vp_announceSettingsOnOffToggle(
                                &currentLanguage->brightness, queueFlags,
                                state.settings.keyBacklight);
                            break;
#endif
                        default:
                            state.ui_screen = SETTINGS_DISPLAY;
                    }
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(display_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(display_num);
                else if(msg.keys & KEY_ENTER)
                    ui_state.edit_mode = !ui_state.edit_mode;
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
#ifdef CONFIG_GPS
            case SETTINGS_GPS:
                if(msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT ||
                   (ui_state.edit_mode &&
                   (msg.keys & (KEY_DOWN | KEY_UP | KEY_F1 | KEY_F2 |
                                KNOB_LEFT | KNOB_RIGHT))))
                {
                    switch(ui_state.menu_selected)
                    {
                        case G_ENABLED:
                            if(state.settings.gps_enabled)
                                state.settings.gps_enabled = 0;
                            else
                                state.settings.gps_enabled = 1;
                            vp_announceSettingsOnOffToggle(&currentLanguage->gpsEnabled,
                                                           queueFlags,
                                                           state.settings.gps_enabled);
                            break;
#ifdef CONFIG_RTC
                        case G_SET_TIME:
                            state.settings.gpsSetTime = !state.settings.gpsSetTime;
                            vp_announceSettingsOnOffToggle(&currentLanguage->gpsSetTime,
                                                           queueFlags,
                                                           state.settings.gpsSetTime);
                            break;
                        case G_TIMEZONE:
                            if(msg.keys & KEY_LEFT || msg.keys & KEY_DOWN ||
                               msg.keys & KNOB_LEFT)
                                state.settings.utc_timezone -= 1;
                            else if(msg.keys & KEY_RIGHT || msg.keys & KEY_UP ||
                                    msg.keys & KNOB_RIGHT)
                                state.settings.utc_timezone += 1;
                            vp_announceTimeZone(state.settings.utc_timezone, queueFlags);
                            break;
#endif
                        default:
                            state.ui_screen = SETTINGS_GPS;
                    }
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_gps_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_gps_num);
                else if(msg.keys & KEY_ENTER)
                    ui_state.edit_mode = !ui_state.edit_mode;
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
#endif
            // Radio Settings
            case SETTINGS_RADIO:
                // If the entry is selected with enter we are in edit_mode
                if (ui_state.edit_mode)
                {
                    switch(ui_state.menu_selected)
                    {
                        case R_OFFSET:
                            // Handle offset frequency input
#if defined(CONFIG_UI_NO_KEYBOARD)
                            if(msg.long_press && msg.keys & KEY_ENTER)
                            {
                                // Long press on CONFIG_UI_NO_KEYBOARD causes digits to advance by one
                                ui_state.new_offset /= 10;
#else
                            if(msg.keys & KEY_ENTER)
                            {
#endif
                                // Apply new offset if it is within the hardware
                                // limits of the radio
                                freq_t new_freq = state.channel.rx_frequency + ui_state.new_offset;
                                if (_ui_freq_check_limits(new_freq))
                                {
                                    state.channel.tx_frequency = new_freq;
                                    vp_queueStringTableEntry(&currentLanguage->offset);
                                    vp_queueFrequency(ui_state.new_offset);
                                    ui_state.edit_mode = false;
                                }
                            }
                            else
                            if(msg.keys & KEY_ESC)
                            {
                                // Announce old frequency offset
                                vp_queueStringTableEntry(&currentLanguage->offset);
                                vp_queueFrequency((int32_t)state.channel.tx_frequency - (int32_t)state.channel.rx_frequency);
                            }
                            else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                    msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            {
                                _ui_numberInputDel(&ui_state.new_offset);
                            }
#if defined(CONFIG_UI_NO_KEYBOARD)
                            else if(msg.keys & KNOB_LEFT || msg.keys & KNOB_RIGHT || msg.keys & KEY_ENTER)
#else
                            else if(input_isNumberPressed(msg))
#endif
                            {
                                _ui_numberInputKeypad(&ui_state.new_offset, msg);
                                ui_state.input_position += 1;
                            }
                            else if (msg.long_press && (msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
                            {
                                vp_queueFrequency(ui_state.new_offset);
                                f1Handled=true;
                            }
                            break;
                        case R_DIRECTION:
                            if(msg.keys & (KEY_UP | KEY_DOWN | KEY_LEFT |
                                           KEY_RIGHT | KEY_F1 | KEY_F2 |
                                           KNOB_LEFT | KNOB_RIGHT))
                            {
                                // Invert frequency offset direction
                                if (state.channel.tx_frequency >= state.channel.rx_frequency)
                                    state.channel.tx_frequency -= 2 * ((int32_t)state.channel.tx_frequency - (int32_t)state.channel.rx_frequency);
                                else // Switch to positive offset
                                    state.channel.tx_frequency -= 2 * ((int32_t)state.channel.tx_frequency - (int32_t)state.channel.rx_frequency);
                            }
                            break;
                        case R_STEP:
                            if (msg.keys & (KEY_UP | KEY_RIGHT | KEY_F2 |
                                            KNOB_RIGHT))
                            {
                                // Cycle over the available frequency steps
                                state.step_index++;
                                state.step_index %= n_freq_steps;
                            }
                            else if(msg.keys & (KEY_DOWN | KEY_LEFT | KEY_F1 |
                                                KNOB_LEFT))
                            {
                                state.step_index += n_freq_steps;
                                state.step_index--;
                                state.step_index %= n_freq_steps;
                            }
                            break;
                        case R_SQUELCH:
                            // Global squelch, the same setting the quick menu
                            // changes and OpMode_FM compares the RSSI with
                            if(msg.keys & (KEY_UP | KEY_RIGHT | KEY_F2 |
                                           KNOB_RIGHT))
                            {
                                if(state.settings.sqlLevel < 15)
                                    state.settings.sqlLevel++;
                                *sync_rtx = true;
                                vp_announceSquelch(state.settings.sqlLevel,
                                                   queueFlags);
                            }
                            else if(msg.keys & (KEY_DOWN | KEY_LEFT | KEY_F1 |
                                                KNOB_LEFT))
                            {
                                if(state.settings.sqlLevel > 0)
                                    state.settings.sqlLevel--;
                                *sync_rtx = true;
                                vp_announceSquelch(state.settings.sqlLevel,
                                                   queueFlags);
                            }
                            break;
                        case R_POWER:
                            // TX power, in the same 1 / 2.5 / 5 W steps as the
                            // quick menu and the macro grid
                            if(msg.keys & (KEY_UP | KEY_RIGHT | KEY_F2 |
                                           KNOB_RIGHT))
                            {
                                if(state.channel.power < 5000)
                                    state.channel.power += 500;
                                *sync_rtx = true;
                                vp_announcePower(state.channel.power, queueFlags);
                            }
                            else if(msg.keys & (KEY_DOWN | KEY_LEFT | KEY_F1 |
                                                KNOB_LEFT))
                            {
                                if(state.channel.power > 1000)
                                    state.channel.power -= 500;
                                *sync_rtx = true;
                                vp_announcePower(state.channel.power, queueFlags);
                            }
                            break;
                        default:
                            state.ui_screen = SETTINGS_RADIO;
                    }
                    // If ENTER or ESC are pressed, exit edit mode, R_OFFSET is managed separately
                    if((ui_state.menu_selected != R_OFFSET && msg.keys & KEY_ENTER) || msg.keys & KEY_ESC)
                        ui_state.edit_mode = false;
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_radio_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_radio_num);
                else if(msg.keys & KEY_ENTER) {
                    ui_state.edit_mode = true;
                    // If we are entering R_OFFSET clear temp offset
                    if (ui_state.menu_selected == R_OFFSET)
                        ui_state.new_offset = 0;
                    // Reset input position
                    ui_state.input_position = 0;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
            /*
             * Settings -> Identity (docs/UI.md): the callsign every
             * transmitting mode shares and the radio's own DMR ID.
             *
             * Both rows edit a field of settings_t and nothing else: the RTX
             * thread copies them into rtx_cfg on the next pass (core/threads.c),
             * so confirming asks for that pass with *sync_rtx, and the debouncer
             * in state_task() writes the record three seconds later like any
             * other setting. There is no second copy anywhere -- a CPS
             * `settings set --callsign --dmr-id` over the PC protocol lands in
             * the same two fields (docs/PC_PROTO.md).
             *
             * A value that fails the range check is refused, not clamped: it is
             * not written to the settings, the page says why under the rows
             * (enum identityAlert) and edit mode stays on so the operator can
             * correct the digits or leave with red.
             */
            case SETTINGS_IDENTITY:
                /*
                 * The reason a confirm was refused stays on the page until the
                 * operator presses a key, and no longer: this branch runs on
                 * every pass of the loop (the periodic part included, with no
                 * keys), so clearing it unconditionally would leave the warning
                 * on screen for one frame and no more.
                 */
                if(msg.keys != 0)
                    ui_state.identity_alert = ID_ALERT_NONE;
                if(ui_state.edit_mode)
                {
                    switch (ui_state.menu_selected)
                    {
                        case ID_CALLSIGN:
                            if(msg.keys & KEY_ENTER)
                            {
                                _ui_textInputConfirm(ui_state.new_callsign);
                                if(settings_callsignValid(ui_state.new_callsign))
                                {
                                    memset(state.settings.callsign, 0,
                                           sizeof(state.settings.callsign));
                                    strncpy(state.settings.callsign,
                                            ui_state.new_callsign,
                                            sizeof(state.settings.callsign) - 1);
                                    *sync_rtx = true;
                                    ui_state.edit_mode = false;
                                    vp_announceBuffer(&currentLanguage->callsign,
                                                      false, true,
                                                      state.settings.callsign);
                                }
                                else
                                {
                                    /* Empty, too long or outside the alphabet */
                                    ui_state.identity_alert = ID_ALERT_CALLSIGN;
                                }
                            }
                            else if(msg.keys & KEY_ESC)
                            {
                                ui_state.edit_mode = false;
                                vp_announceBuffer(&currentLanguage->callsign,
                                                  false, true,
                                                  state.settings.callsign);
                            }
                            else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                    msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT ||
                                    msg.keys & KNOB_LEFT || msg.keys & KNOB_RIGHT)
                            {
                                _ui_textInputDel(ui_state.new_callsign);
                            }
                            else if(input_isCharPressed(msg))
                            {
                                _ui_textInputKeypad(ui_state.new_callsign,
                                                    SETTINGS_CALLSIGN_LEN - 1,
                                                    msg, true);
                            }
                            else if (msg.long_press && (msg.keys & KEY_F1) &&
                                     (state.settings.vpLevel > vpBeep))
                            {
                                vp_announceBuffer(&currentLanguage->callsign,
                                                  true, true,
                                                  ui_state.new_callsign);
                                f1Handled=true;
                            }
                            break;
                        case ID_DMR_ID:
                            if(msg.keys & KEY_ENTER)
                            {
                                if(settings_dmrIdValid(ui_state.new_dmr_id))
                                {
                                    state.settings.dmr_id = ui_state.new_dmr_id;
                                    *sync_rtx = true;
                                    ui_state.edit_mode = false;
                                    vp_queueInteger((int32_t)state.settings.dmr_id);
                                }
                                else
                                {
                                    /* 0, or more than 24 bits */
                                    ui_state.identity_alert = ID_ALERT_DMR_ID;
                                }
                            }
                            else if(msg.keys & KEY_ESC)
                            {
                                ui_state.edit_mode = false;
                                vp_queueInteger((int32_t)state.settings.dmr_id);
                            }
                            else if(input_isNumberPressed(msg))
                            {
                                _ui_numberInputKeypad(&ui_state.new_dmr_id, msg);
                                if(ui_state.new_dmr_id_digits < 8)
                                    ui_state.new_dmr_id_digits++;
                            }
                            else if((msg.keys & (KEY_UP | KEY_DOWN | KEY_LEFT |
                                                 KEY_RIGHT | KNOB_LEFT |
                                                 KNOB_RIGHT)) &&
                                    (ui_state.new_dmr_id_digits > 0))
                            {
                                /* Backspace: drop the last digit typed */
                                ui_state.new_dmr_id /= 10;
                                ui_state.new_dmr_id_digits--;
                                ui_state.last_keypress = 0;
                                vp_queueInteger((int32_t)ui_state.new_dmr_id);
                            }
                            break;
                        default:
                            state.ui_screen = SETTINGS_IDENTITY;
                    }
                }
                else
                {
                    if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                        _ui_menuUp(settings_identity_num);
                    else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                        _ui_menuDown(settings_identity_num);
                    else if(msg.keys & KEY_ENTER)
                    {
                        ui_state.edit_mode = true;
                        ui_state.input_position = 0;
                        ui_state.input_set = 0;
                        ui_state.input_number = 0;
                        ui_state.last_keypress = 0;

                        if(ui_state.menu_selected == ID_CALLSIGN)
                        {
                            /* The entry starts empty: the operator types the
                             * whole callsign, as the M17 page did before this
                             * row moved here. */
                            _ui_textInputReset(ui_state.new_callsign,
                                               sizeof(ui_state.new_callsign));
                            vp_announceBuffer(&currentLanguage->callsign,
                                              true, true,
                                              ui_state.new_callsign);
                        }
                        else
                        {
                            /* Digits accumulate from zero; the current ID is
                             * already on the row if the operator wants it. */
                            ui_state.new_dmr_id = 0;
                            ui_state.new_dmr_id_digits = 0;
                            vp_queueInteger((int32_t)ui_state.new_dmr_id);
                        }
                    }
                    else if(msg.keys & KEY_ESC)
                        _ui_menuBack(MENU_SETTINGS);
                }
                break;
#ifdef UI_MODE_M17
            // M17 Settings
            case SETTINGS_M17:
                if(ui_state.edit_mode)
                {
                    switch (ui_state.menu_selected)
                    {
                        case M17_METATEXT:
                            // Handle text input for the M17 meta text
                            if(msg.keys & KEY_ENTER)
                            {
                                _ui_textInputConfirm(ui_state.new_meta_text);
                                // Save selected message and disable input mode
                                strncpy(state.settings.M17_meta_text, ui_state.new_meta_text, 52);
                                ui_state.edit_meta_text = false;
                                ui_state.edit_mode = false;
                                vp_announceBuffer(&currentLanguage->metaText,
                                                  false, true, state.settings.M17_meta_text);
                            }
                            else if(msg.keys & KEY_ESC)
                            {
                                // Discard selected message and disable input mode
                                ui_state.edit_meta_text = false;
                                ui_state.edit_mode = false;
                                vp_announceBuffer(&currentLanguage->metaText,
                                                  false, true, state.settings.M17_meta_text);
                            }
                            else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                     msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            {
                                _ui_textInputDel(ui_state.new_meta_text);
                            }
                            else if(input_isCharPressed(msg))
                            {
                                _ui_textInputKeypad(ui_state.new_meta_text, 52, msg, false);
                            }
                            else if (msg.long_press && (msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
                            {
                                vp_announceBuffer(&currentLanguage->metaText,
                                                  true, true, ui_state.new_meta_text);
                                f1Handled=true;
                            }
                            break;
                        case M17_CAN:
                            if(msg.keys & (KEY_DOWN | KEY_F1 | KNOB_LEFT))
                                _ui_changeM17Can(-1);
                            else if(msg.keys & (KEY_UP | KEY_F2 | KNOB_RIGHT))
                                _ui_changeM17Can(+1);
                            else if(msg.keys & KEY_ENTER)
                                ui_state.edit_mode = !ui_state.edit_mode;
                            else if(msg.keys & KEY_ESC)
                                ui_state.edit_mode = false;
                            break;
                        case M17_CAN_RX:
                            if(msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT ||
                                (ui_state.edit_mode &&
                                 (msg.keys & (KEY_DOWN | KEY_UP | KEY_F1 |
                                              KEY_F2 | KNOB_LEFT |
                                              KNOB_RIGHT))))
                            {
                                state.settings.m17_can_rx =
                                    !state.settings.m17_can_rx;
                            }
                            else if(msg.keys & KEY_ENTER)
                                ui_state.edit_mode = !ui_state.edit_mode;
                            else if(msg.keys & KEY_ESC)
                                ui_state.edit_mode = false;
                    }
                }
                else
                {
                    if(msg.keys & KEY_ENTER)
                    {
                        // Enable edit mode
                        ui_state.edit_mode = true;

                        // If message input, reset text input variables
                        if(ui_state.menu_selected == M17_METATEXT)
                        {
                            //   ui_state.edit_mode = false;
                            ui_state.edit_meta_text = true;
                            _ui_textInputReset(ui_state.new_meta_text,
                                    sizeof(ui_state.new_meta_text));
                            vp_announceBuffer(&currentLanguage->metaText,
                                            true, true, ui_state.new_meta_text);
                        }
                    }
                    else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                        _ui_menuUp(settings_m17_num);
                    else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                        _ui_menuDown(settings_m17_num);
                    else if((msg.keys & KEY_RIGHT) && (ui_state.menu_selected == M17_CAN))
                            _ui_changeM17Can(+1);
                    else if((msg.keys & KEY_LEFT)  && (ui_state.menu_selected == M17_CAN))
                            _ui_changeM17Can(-1);
                    else if(msg.keys & KEY_ESC)
                    {
                        *sync_rtx = true;
                        _ui_menuBack(MENU_SETTINGS);
                    }
                }
                break;
#endif
#ifdef CONFIG_TETRA_AFC
            case SETTINGS_TETRA:
                if (ui_state.edit_mode)
                {
                    switch (ui_state.menu_selected)
                    {
                        case T_AFC:
                            if (msg.keys & (KEY_LEFT | KEY_RIGHT | KEY_DOWN |
                                            KEY_UP | KEY_F1 | KEY_F2 |
                                            KNOB_LEFT | KNOB_RIGHT))
                            {
                                state.settings.tetraAfcOff =
                                    !state.settings.tetraAfcOff;
                            }
                            else if (msg.keys & KEY_ENTER ||
                                     msg.keys & KEY_ESC)
                                ui_state.edit_mode = false;
                            break;
#ifdef CONFIG_RX_FINE_OFFSET
                        case T_FINE:
                            if (msg.keys & (KEY_UP | KEY_RIGHT | KEY_F2 |
                                            KNOB_RIGHT))
                                qm_set_rx_fine(state.settings.rxFineHz + 10);
                            else if (msg.keys & (KEY_DOWN | KEY_LEFT | KEY_F1 |
                                                 KNOB_LEFT))
                                qm_set_rx_fine(state.settings.rxFineHz - 10);
                            else if (msg.keys & (KEY_ENTER | KEY_ESC))
                                ui_state.edit_mode = false;
                            break;
#endif
                    }
                }
                else if (msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_tetra_num);
                else if (msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_tetra_num);
                else if (msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                {
                    /* Toggle at once, as for the M17 CAN RX check */
                    if (ui_state.menu_selected == T_AFC)
                        state.settings.tetraAfcOff =
                            !state.settings.tetraAfcOff;
                }
                else if (msg.keys & KEY_ENTER)
                    ui_state.edit_mode = true;
                else if (msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
#endif
            case SETTINGS_AUDIO:
                if(ui_state.edit_mode)
                {
                    if(msg.keys & (KEY_UP | KEY_RIGHT | KEY_F2 | KNOB_RIGHT))
                    {
                        if(ui_state.menu_selected == AU_PHONETIC)
                            _ui_changePhoneticSpell(true);
#ifdef CONFIG_C62_NOISE_SUPPRESS
                        else if(ui_state.menu_selected == AU_NOISE)
                            _ui_changeNoiseSuppression(true);
#endif
                        else
                            _ui_changeVoiceLevel(1);
                    }
                    else if(msg.keys & (KEY_DOWN | KEY_LEFT | KEY_F1 | KNOB_LEFT))
                    {
                        if(ui_state.menu_selected == AU_PHONETIC)
                            _ui_changePhoneticSpell(false);
#ifdef CONFIG_C62_NOISE_SUPPRESS
                        else if(ui_state.menu_selected == AU_NOISE)
                            _ui_changeNoiseSuppression(false);
#endif
                        else
                            _ui_changeVoiceLevel(-1);
                    }
                    else if(msg.keys & (KEY_ENTER | KEY_ESC))
                        ui_state.edit_mode = false;
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_audio_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_audio_num);
                else if(msg.keys & KEY_ENTER)
                    ui_state.edit_mode = true;
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
            case SETTINGS_FM:
                if (ui_state.edit_mode)
                {
                    if (msg.keys & KEY_ESC)
                        ui_state.edit_mode = false;

                    switch (ui_state.menu_selected)
                    {
                        case CTCSS_Tone:
                            if (msg.keys & (KEY_DOWN | KEY_F1 | KNOB_LEFT))
                            {
                                if (state.channel.fm.txTone == 0)
                                {
                                    state.channel.fm.txTone =
                                        CTCSS_FREQ_NUM - 1;
                                }
                                else
                                {
                                    state.channel.fm.txTone--;
                                }
                            }
                            else if (msg.keys & (KEY_UP | KEY_F2 | KNOB_RIGHT))
                            {
                                state.channel.fm.txTone++;
                            } else if (msg.keys & KEY_ENTER) {
                                ui_state.edit_mode = false;
                            }
                            state.channel.fm.txTone %= CTCSS_FREQ_NUM;
                            state.channel.fm.rxTone = state.channel.fm.txTone;
                            *sync_rtx = true;
                            break;
                        case CTCSS_Enabled:
                            if (msg.keys & (KEY_DOWN | KEY_F1 | KNOB_LEFT))
                            {
                                _ui_handleToneSelectScroll(true);
                            } else if (msg.keys & (KEY_UP | KEY_F2 | KNOB_RIGHT))
                            {
                                _ui_handleToneSelectScroll(false);
                            } else if (msg.keys & KEY_ENTER) {
                                ui_state.edit_mode = false;
                            }

                            *sync_rtx = true;
                            break;
                        case FM_BANDWIDTH:
                            if (msg.keys & (KEY_UP | KEY_RIGHT | KEY_F2 |
                                            KNOB_RIGHT))
                            {
                                if (state.channel.bandwidth < BW_6_25)
                                    state.channel.bandwidth++;
                                *sync_rtx = true;
                                vp_announceBandwidth(state.channel.bandwidth,
                                                     queueFlags);
                            }
                            else if (msg.keys & (KEY_DOWN | KEY_LEFT | KEY_F1 |
                                                KNOB_LEFT))
                            {
                                if (state.channel.bandwidth > BW_12_5)
                                    state.channel.bandwidth--;
                                *sync_rtx = true;
                                vp_announceBandwidth(state.channel.bandwidth,
                                                     queueFlags);
                            }
                            else if (msg.keys & (KEY_ENTER | KEY_ESC))
                                ui_state.edit_mode = false;
                            break;
                    }
                }
                else if (msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_fm_num);
                else if (msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_fm_num);
                else if (msg.keys & KEY_ENTER)
                    ui_state.edit_mode = !ui_state.edit_mode;
                else if (msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;

            /*
             * A mode's own page (Settings → NXDN, Settings → AM): the mode's
             * quick menu rows, walked with the same engine, so the keys mean
             * here exactly what they mean in the popup.
             */
            case SETTINGS_MODE:
                if (msg.keys & (KEY_UP | KNOB_LEFT))
                    qm_page_move(-1);
                else if (msg.keys & (KEY_DOWN | KNOB_RIGHT))
                    qm_page_move(+1);
                else if (msg.keys & KEY_F1)
                    qm_page_adjust(-1, msg.long_press);
                else if (msg.keys & KEY_F2)
                    qm_page_adjust(+1, msg.long_press);
                else if (msg.keys & KEY_ENTER)
                    qm_page_enter();
                else if (msg.keys & KEY_ESC)
                {
                    qm_page_close();
                    _ui_menuBack(MENU_SETTINGS);
                }
                break;

            case SETTINGS_ACCESSIBILITY:
                if(msg.keys & KEY_LEFT || (ui_state.edit_mode &&
                   (msg.keys & (KEY_DOWN | KEY_F1 | KNOB_LEFT))))
                {
                    switch(ui_state.menu_selected)
                    {
                        case A_MACRO_LATCH:
                            _ui_changeMacroLatch(false);
                            break;
                        default:
                            state.ui_screen = SETTINGS_ACCESSIBILITY;
                    }
                }
                else if(msg.keys & KEY_RIGHT || (ui_state.edit_mode &&
                        (msg.keys & (KEY_UP | KEY_F2 | KNOB_RIGHT))))
                {
                    switch(ui_state.menu_selected)
                    {
                        case A_MACRO_LATCH:
                            _ui_changeMacroLatch(true);
                            break;
                        default:
                            state.ui_screen = SETTINGS_ACCESSIBILITY;
                    }
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_accessibility_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_accessibility_num);
                else if(msg.keys & KEY_ENTER)
                    ui_state.edit_mode = !ui_state.edit_mode;
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
            case SETTINGS_RESET2DEFAULTS:
                if(! ui_state.edit_mode){
                    //require a confirmation ENTER, then another
                    //edit_mode is slightly misused to allow for this
                    if(msg.keys & KEY_ENTER)
                    {
                        ui_state.edit_mode = true;
                    }
                    else if(msg.keys & KEY_ESC)
                    {
                        _ui_menuBack(MENU_SETTINGS);
                    }
                } else {
                    if(msg.keys & KEY_ENTER)
                    {
                        ui_state.edit_mode = false;
                        state_resetSettingsAndVfo();
                        _ui_menuBack(MENU_SETTINGS);
                    }
                    else if(msg.keys & KEY_ESC)
                    {
                        ui_state.edit_mode = false;
                        _ui_menuBack(MENU_SETTINGS);
                    }
                }
                break;
        }

        /*
         * Enable Tx only if the UI is on a main screen with no overlay: the
         * same rule the six screens above use, and the one that re-arms the
         * PTT when this very key brought the UI back to a main screen (leaving
         * a menu with red used to leave it disabled until the next key press).
         */
        _ui_syncTxDisable(sync_rtx);
        if (!f1Handled && (msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
        {
            vp_replayLastPrompt();
        }
        else if ((priorUIScreen!=state.ui_screen) && state.settings.vpLevel > vpLow)
        {
            // When we switch to VFO or Channel screen, we need to announce it.
            // Likewise for information screens.
            // All other cases are handled as needed.
            vp_announceScreen(state.ui_screen);
        }
        // generic beep for any keydown if beep is enabled.
        // At vp levels higher than beep, keys will generate voice so no need
        // to beep or you'll get an unwanted click.
        if ((msg.keys &0xffff) && (state.settings.vpLevel == vpBeep))
            vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
        // If we exit and re-enter the same menu, we want to ensure it speaks.
        if (msg.keys & KEY_ESC)
            _ui_reset_menu_anouncement_tracking();
    }
    else if(event.type == EVENT_STATUS)
    {
#ifdef CONFIG_GPS
        if ((state.ui_screen == MENU_GPS) &&
            (!vp_isPlaying()) &&
            (state.settings.vpLevel > vpLow) &&
            (!txOngoing && !rtx_rxSquelchOpen()))
        {// automatically read speed and direction changes only!
            enum vpGPSInfoFlags whatChanged = GetGPSDirectionOrSpeedChanged();
            if (whatChanged != vpGPSNone)
                vp_announceGPSInfo(whatChanged);
        }
#endif //            CONFIG_GPS

        if (txOngoing || rtx_rxSquelchOpen() || (state.volume != last_state.volume))
        {
            _ui_exitStandby(now);
            return;
        }

        if (_ui_checkStandby(now - last_event_tick))
        {
            _ui_enterStandby();
        }
    }
}

bool ui_updateGUI()
{
    frameCount++;

    if(redraw_needed == false)
        return false;

#ifdef UI_CAPTURE
    /* A new frame of the simulator's recorder (core/gfx_capture.h) */
    gfx_capture_begin();
#endif

    if(!layout_ready)
    {
        _ui_calculateLayout(&layout);
        layout_ready = true;
#ifdef CONFIG_KEY_BACKLIGHT
        /* The keypad backlight follows its setting, not the display level */
        c62_setKeyBacklight(state.settings.keyBacklight);
#endif
    }
    // Tools menu (P1 long): a screen of its own, drawn before anything else
    if(ui_tools_isOpen())
    {
        ui_tools_draw();
#ifdef UI_CAPTURE
        gfx_capture_commit();
#endif
        return true;
    }
    // Channel list and editor draw on top of whatever screen was open
    if(ui_channels_isOpen())
    {
        ui_channels_draw();
#ifdef UI_CAPTURE
        gfx_capture_commit();
#endif
        return true;
    }
    // Zone browser draws itself, with its own header and hint bar
    if(ui_zones_isOpen())
    {
        ui_zones_draw();
#ifdef UI_CAPTURE
        gfx_capture_commit();
#endif
        return true;
    }
    /*
     * The analyser owns the whole screen and its own plot: drawn here, but the
     * hints row and the popup are still the shared ones below.
     */
    if((last_state.ui_screen == SPECTRUM) && ui_spectrum_isOpen())
        ui_spectrum_draw();
    // Draw current GUI page
    switch(last_state.ui_screen)
    {
        // VFO main screen
        case MAIN_VFO:
            _ui_drawMainVFO(&ui_state);
            break;
        // VFO frequency input screen
        case MAIN_VFO_INPUT:
            _ui_drawMainVFOInput(&ui_state);
            break;
        // MEM main screen
        case MAIN_MEM:
            _ui_drawMainMEM(&ui_state);
            break;
#ifdef CONFIG_APRS
        /*
         * APRS screens draw themselves: they are self contained so that the
         * menus and hints rework does not collide with them.
         */
        case MENU_APRS:
        case APRS_DETAIL:
        case APRS_MSGS:
        case APRS_MSG_VIEW:
        case APRS_MSG_EDIT:
        case APRS_CFG:
            ui_aprs_draw(last_state.ui_screen);
            break;
#endif
#ifdef CONFIG_KISS
        case PKT_CFG:
        case PKT_STATUS:
            ui_packet_draw(last_state.ui_screen);
            break;
#endif
        /* Messaging screens draw themselves, like the APRS ones */
        case MENU_MESSAGES:
        case MSG_VIEW:
        case MSG_SENT:
        case MSG_COMPOSE:
        case MSG_STATUS:
            ui_msgs_draw(last_state.ui_screen);
            break;
        // Top menu screen
        case MENU_TOP:
            _ui_drawMenuTop(&ui_state);
            break;
        // Zone menu screen
        case MENU_BANK:
            _ui_drawMenuBank(&ui_state);
            break;
        // Channel menu screen
        case MENU_CHANNEL:
            _ui_drawMenuChannel(&ui_state);
            break;
        // Contacts menu screen: the in-radio book owns it on the C62
        case MENU_CONTACTS:
            if (ui_contacts_isOpen())
                ui_contacts_draw();
            else
                _ui_drawMenuContacts(&ui_state);
            break;
#ifdef CONFIG_GPS
        // GPS menu screen
        case MENU_GPS:
            _ui_drawMenuGPS();
            break;
#endif
        // Settings menu screen
        case MENU_SETTINGS:
            _ui_drawMenuSettings(&ui_state);
            break;
        // Flash backup and restore screen
        case MENU_BACKUP_RESTORE:
            _ui_drawMenuBackupRestore(&ui_state);
            break;
        /*
         * The backup and the restore themselves are drawn by their own module:
         * they carry a state (asked, done, failed) that a menu list has no room
         * for (ui_backup.c, docs/BACKUP.md).
         */
        case MENU_BACKUP:
        case MENU_RESTORE:
            ui_backup_draw(last_state.ui_screen);
            break;
        // Info menu screen
        case MENU_INFO:
            _ui_drawMenuInfo(&ui_state);
            break;
        // About menu screen
        case MENU_ABOUT:
            _ui_drawMenuAbout(&ui_state);
            break;
#ifdef CONFIG_RTC
        // Time&Date settings screen
        case SETTINGS_TIMEDATE:
            _ui_drawSettingsTimeDate();
            break;
        // Time&Date settings screen, edit mode
        case SETTINGS_TIMEDATE_SET:
            _ui_drawSettingsTimeDateSet(&ui_state);
            break;
#endif
        // Display settings screen
        case SETTINGS_DISPLAY:
            _ui_drawSettingsDisplay(&ui_state);
            break;
#ifdef CONFIG_GPS
        // GPS settings screen
        case SETTINGS_GPS:
            _ui_drawSettingsGPS(&ui_state);
            break;
#endif
#ifdef UI_MODE_M17
        // M17 settings screen
        case SETTINGS_M17:
            _ui_drawSettingsM17(&ui_state);
            break;
#endif
#ifdef CONFIG_TETRA_AFC
        case SETTINGS_TETRA:
            _ui_drawSettingsTetra(&ui_state);
            break;
#endif
        // FM settings screen
        case SETTINGS_FM:
            _ui_drawSettingsFM(&ui_state);
            break;
        // A mode's own settings page (its quick menu rows as a list)
        case SETTINGS_MODE:
            qm_page_draw();
            break;
        case SETTINGS_ACCESSIBILITY:
            _ui_drawSettingsAccessibility(&ui_state);
            break;
        // Screen to support resetting Settings and VFO to defaults
        case SETTINGS_RESET2DEFAULTS:
            _ui_drawSettingsReset2Defaults(&ui_state);
            break;
        // Screen to set frequency offset and step
        case SETTINGS_RADIO:
            _ui_drawSettingsRadio(&ui_state);
            break;
        // Voice prompts and phonetics
        case SETTINGS_AUDIO:
            _ui_drawSettingsAudio(&ui_state);
            break;
        // Station identity: callsign and DMR ID
        case SETTINGS_IDENTITY:
            _ui_drawSettingsIdentity(&ui_state);
            break;
        // Low battery screen
        case LOW_BAT:
            _ui_drawLowBatteryScreen();
            break;
    }

    // If MACRO menu is active draw it
    if(macro_menu)
    {
        _ui_drawDarkOverlay();
        _ui_drawMacroMenu(&ui_state);
    }
    else if(qm_state.open)
    {
        qm_draw();
    }
    else
    {
        /* Contextual key hints, in the row reserved above the screen bottom */
        ui_hints_draw_for_screen(last_state.ui_screen);
    }

#ifdef UI_CAPTURE
    gfx_capture_commit();
#endif

    redraw_needed = false;
    return true;
}

bool ui_pushEvent(const uint8_t type, const uint32_t data)
{
    uint8_t newHead = (evQueue_wrPos + 1) % MAX_NUM_EVENTS;

    // Queue is full
    if(newHead == evQueue_rdPos) return false;

    // Preserve atomicity when writing the new element into the queue.
    event_t event;
    event.type    = type;
    event.payload = data;

    evQueue[evQueue_wrPos] = event;
    evQueue_wrPos = newHead;

    return true;
}

void ui_terminate()
{
}
