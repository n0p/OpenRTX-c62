/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_DEFAULT_H
#define UI_DEFAULT_H

#include <stdbool.h>
#include "core/state.h"
#include "core/graphics.h"
#include "interfaces/keyboard.h"
#include <stdint.h>
#include "core/event.h"
#include "hwconfig.h"
#include "core/ui.h"
#include "ui/ui_modes.h"

// Maximum menu entry length
#define MAX_ENTRY_LEN 21
// Frequency digits. Eight gives 10 Hz resolution (446.00625 MHz), which the
// BK4819 supports and the 6.25 kHz step needs.
#define FREQ_DIGITS 8
// Frequency resolution of the last entered digit, in Hz
#define FREQ_DIGIT_RESOLUTION 10
// Time & Date digits
#define TIMEDATE_DIGITS 10
// Max number of UI events
#define MAX_NUM_EVENTS 16

enum uiScreen
{
    MAIN_VFO = 0,
    MAIN_VFO_INPUT,
    MAIN_MEM,
    MODE_VFO,
    MODE_MEM,
    MENU_TOP,
    MENU_BANK,
    MENU_CHANNEL,
    MENU_ZONES,
    MENU_CONTACTS,
    MENU_GPS,
    MENU_SETTINGS,
    MENU_BACKUP_RESTORE,
    MENU_BACKUP,
    MENU_RESTORE,
    MENU_INFO,
    MENU_ABOUT,
    SETTINGS_TIMEDATE,
    SETTINGS_TIMEDATE_SET,
    SETTINGS_DISPLAY,
    SETTINGS_GPS,
    SETTINGS_RADIO,
    SETTINGS_AUDIO,
    SETTINGS_M17,
#ifdef CONFIG_TETRA_AFC
    SETTINGS_TETRA,
#endif
    SETTINGS_FM,
    /*
     * A mode's own page: the mode's quick menu rows drawn as a list, so NXDN
     * and AM (and every mode registered with UI_MODE_PAGE) get one without new
     * code here. Which mode is in qm_page_state.qm (ui_quickmenu.h).
     */
    SETTINGS_MODE,
    SETTINGS_ACCESSIBILITY,
    SETTINGS_RESET2DEFAULTS,
    /*
     * APRS screens. They live in ui_aprs.c so that the rework of the menus
     * and of the hints row can proceed in parallel: this enum block and the
     * two dispatch points in ui.c are the only places the two touch.
     */
    MENU_APRS,
    APRS_DETAIL,
    APRS_MSGS,
    APRS_MSG_VIEW,
    APRS_MSG_EDIT,
    APRS_CFG,
    /*
     * Messaging screens (ui_messages.c): transport agnostic inbox, sent
     * folder, composer and delivery status. Owned by their module, the same
     * way the APRS screens are.
     */
    /*
     * Packet modem screens (ui_packet.c), same arrangement as the APRS ones:
     * the configuration page and the counters page.
     */
    PKT_CFG,
    PKT_STATUS,
    MENU_MESSAGES,
    MSG_VIEW,
    MSG_SENT,
    MSG_COMPOSE,
    MSG_STATUS,
    LOW_BAT,
    /*
     * The spectrum analyser (ui_spectrum.c). At the end of the enum on purpose:
     * the screens above are reached by number from the quick menu's QM_GOTO
     * rows and from tests, and inserting in the middle renumbers them all.
     */
    SPECTRUM
};

/**
 * Key claim trace.
 *
 * The dispatch in ui_updateFSM() is a chain of blocks that each consume the
 * key event when it belongs to them; every one of them records itself here
 * before returning. A screen that takes no claim is a key that goes nowhere
 * (the "deaf screen" defect), and two claims on one event mean two blocks are
 * eating the same key. tests/unit/ui_menu_audit.cpp walks every screen with
 * every key and requires exactly one claim, in the block docs/UI.md documents.
 */
enum uiKeyClaim
{
    UI_CLAIM_NONE = 0,   /**< Nobody took the key: a defect                */
    UI_CLAIM_STANDBY,    /**< Coming out of standby swallowed it           */
    UI_CLAIM_SEARCH,     /**< The carrier search stopped on it             */
    UI_CLAIM_QM,         /**< The quick menu owns the keypad               */
    UI_CLAIM_SPECTRUM,   /**< The analyser's screen                        */
    UI_CLAIM_QM_OPEN,    /**< P2 short on a main screen opened the popup    */
    UI_CLAIM_APRS,       /**< One of the APRS screens                      */
    UI_CLAIM_PACKET,     /**< The packet modem screens                     */
    UI_CLAIM_MSGS,       /**< One of the messaging screens                 */
    UI_CLAIM_CONTACTS,   /**< The contact book                             */
    UI_CLAIM_ZONES,      /**< The zone browser                             */
    UI_CLAIM_APRS_ENTER, /**< P2 long in FM opened the APRS screens        */
    UI_CLAIM_MACRO,      /**< The macro grid (MONI or P2 long)             */
    UI_CLAIM_CHANNELS_OPEN, /**< P1 long opened the channel list           */
    UI_CLAIM_CHANNELS,   /**< The channel list or editor                   */
    UI_CLAIM_SCREEN,     /**< The case of the screen in the big switch     */
    UI_CLAIM_UNLOCK      /**< Red held down while the keypad was locked    */
};

/**
 * What the UI thinks is selected, and whether it is editing a field.
 *
 * `ui_state` is a static of ui.c; these two accessors are what a test (or the
 * remote control) needs to see the state that the screens act on, without
 * opening the struct.
 */
uint8_t ui_menu_selected(void);
bool ui_edit_mode(void);

/** Block that consumed the last key event (enum uiKeyClaim). */
uint8_t ui_lastClaim(void);

/** How many blocks consumed the last key event: 1 when it is correct. */
uint8_t ui_lastClaimCount(void);

enum SetRxTx
{
    SET_RX = 0,
    SET_TX
};

// This enum is needed to have item numbers that match
// menu elements even if some elements may be missing (GPS)
enum menuItems
{
    M_BANK = 0,
    M_CHANNEL,
    M_ZONES,
    M_CONTACTS,
    M_MESSAGES,
#ifdef CONFIG_GPS
    M_GPS,
#endif
    M_SETTINGS,
    M_INFO,
    M_ABOUT
};

enum settingsItems
{
    S_DISPLAY = 0,
#ifdef CONFIG_RTC
    S_TIMEDATE,
#endif
#ifdef CONFIG_GPS
    S_GPS,
#endif
    S_RADIO,
    S_AUDIO,
#ifdef UI_MODE_M17
    S_M17,
#endif
#ifdef CONFIG_TETRA_AFC
    S_TETRA,
#endif
    S_FM,
    /*
     * One page per mode that has its own settings (UI_MODE_PAGES in
     * ui/ui_modes.h). The list is generated, in the same order, into
     * settings_items[] in ui.c: registering a mode there adds its page here.
     */
#define UI_MODE_PAGE(id, title) S_PAGE_##id,
    UI_MODE_PAGES(UI_MODE_PAGE)
#undef UI_MODE_PAGE
    S_ACCESSIBILITY,
    S_RESET2DEFAULTS,
};

enum backupRestoreItems
{
    BR_BACKUP = 0,
    BR_RESTORE
};

enum displayItems
{
#ifdef CONFIG_SCREEN_BRIGHTNESS
    D_BRIGHTNESS = 0,
#endif
#ifdef CONFIG_SCREEN_CONTRAST
    D_CONTRAST,
#endif
    D_TIMER,
    D_BATTERY,
#ifdef CONFIG_KEY_BACKLIGHT
    D_KEYLIGHT,
#endif
};

#ifdef CONFIG_GPS
enum settingsGPSItems
{
    G_ENABLED = 0,
#ifdef CONFIG_RTC
    G_SET_TIME,
    G_TIMEZONE
#endif
};
#endif

enum settingsAccessibilityItems
{
    A_MACRO_LATCH = 0,
};

enum settingsRadioItems
{
    R_OFFSET,
    R_DIRECTION,
    R_STEP,
    R_SQUELCH,
    R_POWER,
};

enum settingsAudioItems
{
    AU_VOICE = 0,
    AU_PHONETIC,
};

enum settingsM17Items
{
    M17_CALLSIGN = 0,
    M17_METATEXT,
    M17_CAN,
    M17_CAN_RX
};

#ifdef CONFIG_TETRA_AFC
enum settingsTetraItems
{
    T_AFC = 0,
#ifdef CONFIG_RX_FINE_OFFSET
    T_FINE,
#endif
};
#endif

enum settingsFMItems
{
    CTCSS_Tone,
    CTCSS_Enabled,
    CTCSS_Tx_En,
    CTCSS_Rx_En,
    CTCSS_Cancel,
    FM_BANDWIDTH,
};

/**
 * Struct containing a set of positions and sizes that get
 * calculated for the selected display size.
 * Using these parameters make the UI automatically adapt
 * To displays of different sizes
 */
typedef struct layout_t
{
    uint16_t hline_h;
    uint16_t top_h;
    uint16_t line1_h;
    uint16_t line2_h;
    uint16_t line3_h;
    uint16_t line3_large_h;
    uint16_t line4_h;
    uint16_t menu_h;
    uint16_t bottom_h;
    uint16_t bottom_pad;
    uint16_t status_v_pad;
    uint16_t horizontal_pad;
    uint16_t text_v_offset;
    point_t top_pos;
    point_t line1_pos;
    point_t line2_pos;
    point_t line3_pos;
    point_t line3_large_pos;
    point_t line4_pos;
    point_t bottom_pos;
    fontSize_t top_font;
    symbolSize_t top_symbol_size;
    fontSize_t line1_font;
    symbolSize_t line1_symbol_size;
    fontSize_t line2_font;
    symbolSize_t line2_symbol_size;
    fontSize_t line3_font;
    symbolSize_t line3_symbol_size;
    fontSize_t line3_large_font;
    fontSize_t line4_font;
    symbolSize_t line4_symbol_size;
    fontSize_t bottom_font;
    fontSize_t input_font;
    fontSize_t menu_font;
    fontSize_t message_font;
} layout_t;

/**
 * This structs contains state variables internal to the
 * UI that need to be kept between executions of the UI
 * This state does not need to be saved on device poweroff
 */
typedef struct ui_state_t
{
    // Index of the currently selected menu entry
    uint8_t menu_selected;
    // If true we can change a menu entry value with UP/DOWN
    bool edit_mode;
    bool input_locked;
    // Variables used for VFO input
    uint8_t input_number;
    uint8_t input_position;
    uint8_t input_set;
    long long last_keypress;
    freq_t new_rx_frequency;
    freq_t new_tx_frequency;
    char new_rx_freq_buf[16];
    char new_tx_freq_buf[16];
    size_t m17_meta_text_scroll_position;
    long long m17_meta_text_last_scroll_tick;
    char new_meta_text[53];
    bool edit_meta_text;
#ifdef CONFIG_RTC
    // Variables used for Time & Date input
    datetime_t new_timedate;
    char new_date_buf[9];
    char new_time_buf[9];
#endif
    char new_callsign[10];
    freq_t new_offset;
    // Which state to return to when we exit menu
    uint8_t last_main_state;
#if defined(CONFIG_UI_NO_KEYBOARD)
    uint8_t macro_menu_selected;
#endif // UI_NO_KEYBOARD
}
ui_state_t;

extern layout_t layout;
extern state_t last_state;
extern bool    macro_latched;
extern const char *menu_items[];
extern const char *settings_items[];
extern const char *display_items[];
extern const char *settings_gps_items[];
extern const char *settings_radio_items[];
extern const char *settings_audio_items[];
extern const char *settings_m17_items[];
extern const char *settings_fm_items[];
#ifdef CONFIG_TETRA_AFC
extern const char *settings_tetra_items[];
extern const uint8_t settings_tetra_num;
#endif
extern const char * settings_accessibility_items[];
extern const char *backup_restore_items[];
extern const char *info_items[];
extern const char *authors[];
extern const uint8_t menu_num;
extern const uint8_t settings_num;
extern const uint8_t display_num;
extern const uint8_t settings_gps_num;
extern const uint8_t settings_radio_num;
extern const uint8_t settings_audio_num;
#ifdef UI_MODE_M17
extern const uint8_t settings_m17_num;
#endif
extern const uint8_t settings_fm_num;
extern const uint8_t settings_accessibility_num;
extern const uint8_t backup_restore_num;
extern const uint8_t info_num;
extern const uint8_t author_num;
extern const color_t color_black;
extern const color_t color_grey;
extern const color_t color_white;
extern const color_t yellow_fab413;

/**
 * Move the cursor of the screen that is being shown (the menus keep it in
 * ui_state.menu_selected). Used by the quick menu when it opens a Settings
 * page on a given row, so that "META" lands on the meta text field and not on
 * the first entry of the page.
 */
void ui_menu_select(uint8_t index);

/**
 * Switch screen from an overlay (the quick menu, the macro grid).
 *
 * Everything a menu entry does by hand - remember where to come back to, set
 * the cursor, stop the radio from transmitting while a screen that owns the
 * keypad is open, and call the module's own open() - happens in one place, so
 * the quick menu and the menus can never disagree about what opening the
 * message list means.
 *
 * @param screen: destination, one of enum uiScreen.
 * @param item: row to select on the destination screen, 0 for the first.
 */
void ui_open_screen(uint8_t screen, uint8_t item);

#endif /* UI_DEFAULT_H */
