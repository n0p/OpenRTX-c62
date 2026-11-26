/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Shared harness for the tests that walk the whole interface: the key queue as
 * the keypad feeds it, a clean radio, the stores the tour seeds, the identity
 * of a UI state and a snapshot of what a screen drew (through the simulator's
 * frame recorder, core/gfx_capture.h).
 *
 * It is a header, not a translation unit, because the walk has to be compiled
 * with the simulator's own -D (the screens of every mode, APRS, the packet
 * modem): each test that uses it is registered in meson.build with the same
 * sources and flags as openrtx_c62_sim. tests/unit/ui_keys.cpp,
 * tests/unit/ui_fit.cpp and tests/unit/ui_empty_state.cpp are the users; they
 * are the same walk with different questions asked of each screen.
 */

#ifndef UI_WALK_HPP
#define UI_WALK_HPP

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "core/aprs_service.h"
#include "core/backup_image.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/slot_store.h"
#include "core/event.h"
#include "core/gfx_capture.h"
#include "core/graphics.h"
#include "core/input.h"
#include "core/messaging.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "interfaces/cps_io.h"
#include "protocols/APRS/aprs.h"
#include "protocols/APRS/aprs_msg.h"
#include "protocols/APRS/ax25.h"
#include "rtx/rtx.h"
#include "ui/ui_aprs.h"
#include "ui/ui_channels.h"
#include "ui/ui_contacts.h"
#include "ui/ui_default.h"
#include "ui/ui_dump.h"
#include "ui/ui_hints.h"
#include "ui/ui_layout.h"
#include "ui/ui_messages.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_spectrum.h"
#include "ui/ui_tools.h"
#include "ui/ui_zones.h"
}

/* The keyboard of the host platform is the emulator's */
#include "emulator.h"

extern state_t state;

namespace ui_walk
{

/* ------------------------------------------------------------- the keys --- */

/** One key event, as the keypad queue delivers it, then one pass of the FSM. */
inline void pressKey(uint32_t keys, bool isLong = false)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = isLong ? 1 : 0;

    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

/** A digit is a press and a release, as the matrix reports it. */
inline void digit(uint32_t k)
{
    pressKey(k);
    pressKey(0);
}

/* ----------------------------------------------------------- the stores --- */

/*
 * How many arrows in a row have been pressed. The lists keep their cursor in a
 * private static of their module, so a run of arrows is part of what the next
 * key does and has to be part of the identity of a state (as in
 * sim_coverage.cpp).
 */
inline int &arrowRun()
{
    static int run = 0;
    return run;
}

/* A RAM backing store for the channel bank: no disk in a unit test. */
inline uint8_t *flash()
{
    static uint8_t g[CHSTORE_PARTITION_SIZE];
    return g;
}

inline int ramRead(void *, uint32_t off, void *buf, size_t len)
{
    if ((size_t)off + len > CHSTORE_PARTITION_SIZE)
        return -1;
    memcpy(buf, &flash()[off], len);
    return 0;
}

inline int ramErase(void *, uint32_t off, size_t len)
{
    if ((size_t)off + len > CHSTORE_PARTITION_SIZE)
        return -1;
    memset(&flash()[off], 0xFF, len);
    return 0;
}

inline int ramWrite(void *, uint32_t off, const void *buf, size_t len)
{
    if ((size_t)off + len > CHSTORE_PARTITION_SIZE)
        return -1;
    memcpy(&flash()[off], buf, len);
    return 0;
}

inline const chstorePort port()
{
    chstorePort p = { ramRead, ramErase, ramWrite, nullptr,
                      (uint32_t)CHSTORE_PARTITION_SIZE, CHSTORE_SECTOR_SIZE };
    return p;
}

inline uint8_t *backup()
{
    static uint8_t g[2U * BKP_AREA_SIZE];
    return g;
}

inline int bkpRead(void *, uint32_t off, void *buf, size_t len)
{
    if ((size_t)off + len > sizeof(uint8_t) * 2U * BKP_AREA_SIZE)
        return -1;
    memcpy(buf, &backup()[off], len);
    return 0;
}

inline int bkpErase(void *, uint32_t off, size_t len)
{
    if ((size_t)off + len > sizeof(uint8_t) * 2U * BKP_AREA_SIZE)
        return -1;
    memset(&backup()[off], 0xFF, len);
    return 0;
}

inline int bkpWrite(void *, uint32_t off, const void *buf, size_t len)
{
    if ((size_t)off + len > sizeof(uint8_t) * 2U * BKP_AREA_SIZE)
        return -1;
    memcpy(&backup()[off], buf, len);
    return 0;
}

inline const bkpPort_t bkpPort()
{
    bkpPort_t p = { bkpRead, bkpErase, bkpWrite, nullptr,
                    (uint32_t)(2U * BKP_AREA_SIZE), 0x1000 };
    return p;
}

/* A second RAM area, for the two-slot containers (the contact book): the
   slot store writes at offsets from its base, so it gets its own buffer. */
inline uint8_t *slotFlash()
{
    static uint8_t g[4U * 0x2000];
    return g;
}

inline int slotRead(void *, uint32_t off, void *buf, size_t len)
{
    if ((size_t)off + len > 4U * 0x2000)
        return -1;
    memcpy(buf, &slotFlash()[off], len);
    return 0;
}

inline int slotErase(void *, uint32_t off, size_t len)
{
    if ((size_t)off + len > 4U * 0x2000)
        return -1;
    memset(&slotFlash()[off], 0xFF, len);
    return 0;
}

inline int slotWrite(void *, uint32_t off, const void *buf, size_t len)
{
    if ((size_t)off + len > 4U * 0x2000)
        return -1;
    memcpy(&slotFlash()[off], buf, len);
    return 0;
}

inline const slot_ops_t *slotPort()
{
    static slot_ops_t ops;
    ops.read = slotRead;
    ops.erase = slotErase;
    ops.write = slotWrite;
    ops.ctx = nullptr;
    ops.slot_size = 0x2000;
    ops.sector = 0x1000;
    return &ops;
}

/** One APRS packet, as the deframer would hand it to the service. */
inline void injectAprs(const char *from, const char *info)
{
    ax25_frame_t f;
    memset(&f, 0, sizeof(f));
    ax25_addr_set(&f.dst, "APRS");
    ax25_addr_set(&f.src, from);
    f.control = 0x03;
    f.pid     = 0xF0;
    f.info    = (const uint8_t *)info;
    f.infoLen = strlen(info);

    uint8_t frame[AX25_MAX_INFO + 64];
    size_t len = ax25_build_ui(frame, sizeof(frame), &f);

    REQUIRE(len > 2);
    REQUIRE(aprs_sim_rxFrame(frame, len - 2u, 1000u));
}

/**
 * The stores the tour seeds: one channel, one received message and two APRS
 * packets. A screen that needs data to be reachable, or to have something to
 * act on, gets it from here.
 */
inline void seedStores()
{
    memset(flash(), 0xFF, CHSTORE_PARTITION_SIZE);
    static chstorePort portCopy;
    portCopy = port();
    chstore_bind(&portCopy);
    chstore_init();

    memset(backup(), 0xFF, 2U * BKP_AREA_SIZE);
    static bkpPort_t bkpCopy;
    bkpCopy = bkpPort();
    bkp_bind(&bkpCopy);

    channel_t ch = cps_getDefaultChannel();
    ch.rx_frequency = 145500000;
    ch.tx_frequency = 145500000;
    ch.mode         = OPMODE_FM;
    snprintf(ch.name, sizeof(ch.name), "%s", "SEED");

    chstore_channel_t rec;
    memset(&rec, 0, sizeof(rec));
    chstore_fromChannel(&ch, &rec);
    REQUIRE(chstore_add(&rec) >= 0);

    /* One contact, so the contact editor and the messaging composer have a
       record to open and to save: with an empty book, green on the editor is
       refused ("name empty") and the bar's SAVE would look like a dead key. */
    memset(slotFlash(), 0xFF, 4U * 0x2000);
    cstore_bind(slotPort());
    cstore_forget();
    REQUIRE(cstore_init() == 0);
    cstore_contact_t person;
    memset(&person, 0, sizeof(person));
    snprintf(person.call, sizeof(person.call), "%s", "EA5ABC");
    snprintf(person.name, sizeof(person.name), "%s", "BENCH");
    person.mode = OPMODE_FM;
    REQUIRE(cstore_add(&person) >= 0);

    messaging_init();
    messaging_clear();
    REQUIRE(messaging_received(MSG_TRANSPORT_APRS, 1234u, 0u, "hola", false,
                               1000u) >= 0);

    /* The two packets the simulator's `inject` builds (sim_control.c): a
       position, and a message addressed to this station's own callsign --
       which is "N0CALL-0", not "N0CALL", so the addressee is built by the same
       function the radio uses instead of being spelled out here. */
    /* One message in the outbox, so the sent folder has a row to show its
       status and to cancel: with an empty outbox, green ("STATUS") and P2
       ("CANC") on MSG_SENT have nothing to act on. */
    REQUIRE(messaging_send(MSG_TRANSPORT_APRS, 0u, "saludo", false, 1000u) > 0);

    aprs_service_init();
    /* Two stations: with one, the "next station" key of the detail screen has
       nowhere to go and would look like a dead key. */
    injectAprs("EA5ABC", "!3852.20N/00004.20W-Bench station");
    injectAprs("EA5DEF", "!3851.00N/00003.00W-Second");

    /* Two messages in the inbox, so "next message" on the view screen has a
       second message to move to */
    char own[12];
    char text[APRS_COMMENT_MAX + 40];
    aprs_ownCallsign(own, sizeof(own));
    REQUIRE(aprs_build_message(text, sizeof(text), own, "hola", "1") > 0);
    injectAprs("EA5XYZ", text);
    REQUIRE(aprs_build_message(text, sizeof(text), own, "adios", "2") > 0);
    injectAprs("EA5XYZ", text);
}

/** A clean radio: every module closed, no popup, a known screen and mode. */
inline void resetUi(uint8_t mode = OPMODE_FM, freq_t freq = 145500000)
{
    if (ui_spectrum_isOpen())
        ui_spectrum_leave();

    qm_close();
    qm_page_close();

    for (int i = 0; (i < 10) && ui_tools_isOpen(); i++)
        pressKey(KEY_ESC);
    for (int i = 0; (i < 10) && ui_channels_isOpen(); i++)
        pressKey(KEY_ESC);
    for (int i = 0; (i < 10) && ui_contacts_isOpen(); i++)
        pressKey(KEY_ESC);
    for (int i = 0; (i < 10) && ui_zones_isOpen(); i++)
        pressKey(KEY_ESC);

    state.channel.mode          = mode;
    state.channel.rx_frequency  = freq;
    state.channel.tx_frequency  = freq;
    state.channel.bandwidth     = BW_25;
    state.channel.power         = 1000;
    state.channel.rx_only       = false;
    state.channel.name[0]       = '\0';
    state.channel.dmr.rxColorCode = 1;
    state.channel.dmr.txColorCode = 1;
    state.channel.dmr.dmr_timeslot = 1;
    state.ui_screen             = MAIN_VFO;
    state.tuner_mode            = VFO;
    state.txDisable             = false;
    state.charge                = 100;
    state.emergency             = false;
    state.step_index            = 4;            /* 12.5 kHz */
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;       /* silent in the harness */
    ui_rtxSyncRequest           = false;

    ui_setInputLock(false);
    ui_menu_select(0);
    macro_latched = false;
    emulator_state.PTTstatus = false;
    arrowRun() = 0;
}

/* ---------------------------------------------------------- observation --- */

/** One string, as the screen drew it. */
struct Line
{
    int16_t     x;
    int16_t     y;
    uint8_t     font;
    uint8_t     align;
    uint16_t    width;
    uint32_t    rgb;
    std::string text;
};

/**
 * Everything the next key can depend on, plus everything the screen shows.
 *
 * The first half is stateId() of sim_coverage.cpp with the edit and lock
 * flags; the second half is the frame recorder's copy of the strings, in
 * drawing order, with the position and the selection rectangle. A key that
 * changes either has done something visible; one that changes neither has not.
 */
struct Snapshot
{
    std::string digest;
    std::vector<Line> lines;
};

inline Snapshot observe()
{
    Snapshot s;
    char buf[256];

    /* Draw the screen the FSM has just reached, then read it back. A state
       written by hand (resetUi, an injected packet) has not asked for a
       redraw, and ui_updateGUI() would leave the previous frame in place. */
    ui_requestRedraw();
    /* The screens draw from `last_state`, the copy ui_updateFSM() publishes at
       the end of every pass: a state written by hand has to be published too,
       or the frequency and the mode on the screen are the previous ones. */
    ui_saveState();
    ui_updateGUI();

    gfx_capture_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    gfx_capture_last(&frame);

    snprintf(buf, sizeof(buf),
             "%u|%u|%u|%d|%u|%u|%d|%u|%u|%d|%d|%u|%d%d%d%d%d%d|%d|%d",
             state.ui_screen, state.channel.mode, ui_menu_selected(),
             qm_state.open ? 1 : 0,
             (qm_state.qm != nullptr) ? qm_state.qm->mode : 0xFFu,
             qm_state.selected,
             qm_page_isOpen() ? 1 : 0,
             (qm_page_state.qm != nullptr) ? qm_page_state.qm->mode : 0xFFu,
             qm_page_state.selected,
             ui_tools_isOpen() ? 1 : 0,
             ui_tools_inModes() ? 1 : 0,
             ui_tools_selected(),
             ui_channels_isOpen() ? 1 : 0,
             ui_contacts_isOpen() ? 1 : 0,
             ui_zones_isOpen() ? 1 : 0,
             ui_spectrum_isOpen() ? 1 : 0,
             ui_msgs_isScreen(state.ui_screen) ? 1 : 0,
             ui_aprs_isScreen(state.ui_screen) ? 1 : 0,
             ui_edit_mode() ? 1 : 0,
             ui_getInputLock() ? 1 : 0);

    /* The APRS cursor and draft, which the keys act on (see stateId()) */
    if (ui_aprs_isScreen(state.ui_screen)) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "|%u|%u|%u",
                 (unsigned)ui_aprs_selectedStation(),
                 (unsigned)ui_aprs_selectedMessage(),
                 (unsigned)ui_aprs_editLength());
    }

    s.digest = std::string(buf) + "|" + std::string(frame.title) + "|"
             + std::string(frame.right) + "|"
             + (frame.selection ? std::to_string(frame.sel_top) + "," +
                                  std::to_string(frame.sel_h) + "," +
                                  std::to_string(frame.sel_filled ? 1 : 0)
                                : std::string("-"))
             + (frame.caret ? "|caret " + std::to_string(frame.caret_x) + ","
                              + std::to_string(frame.caret_y) + ","
                              + std::to_string(frame.caret_w)
                            : std::string("|-"));

    for (uint16_t i = 0; i < frame.lines; i++) {
        const gfx_capture_line_t *l = &frame.line[i];

        Line line;
        line.x     = l->x;
        line.y     = l->y;
        line.font  = l->font;
        line.align = l->align;
        line.width = l->width;
        line.rgb   = l->rgb;
        line.text  = std::string(l->text);

        s.lines.push_back(line);
        s.digest += "|" + std::to_string(l->x) + "," + std::to_string(l->y) + ","
                  + std::to_string(l->rgb & 0xFFFFFF) + "," + line.text;
    }

    return s;
}

/* -------------------------------------------------------------- identity --- */

/** The identity of a UI state for the walk (as in sim_coverage.cpp). */
inline std::string stateId()
{
    char buf[192];

    snprintf(buf, sizeof(buf),
             "%u|%u|%u|%d|%u|%u|%d|%u|%u|%d|%d|%u|%d%d%d%d%d%d|%d|%d|%d",
             state.ui_screen, state.channel.mode, ui_menu_selected(),
             qm_state.open ? 1 : 0,
             (qm_state.qm != nullptr) ? qm_state.qm->mode : 0xFFu,
             qm_state.selected,
             qm_page_isOpen() ? 1 : 0,
             (qm_page_state.qm != nullptr) ? qm_page_state.qm->mode : 0xFFu,
             qm_page_state.selected,
             ui_tools_isOpen() ? 1 : 0,
             ui_tools_inModes() ? 1 : 0,
             ui_tools_selected(),
             ui_channels_isOpen() ? 1 : 0,
             ui_contacts_isOpen() ? 1 : 0,
             ui_zones_isOpen() ? 1 : 0,
             ui_spectrum_isOpen() ? 1 : 0,
             ui_msgs_isScreen(state.ui_screen) ? 1 : 0,
             ui_aprs_isScreen(state.ui_screen) ? 1 : 0,
             ui_edit_mode() ? 1 : 0,
             ui_getInputLock() ? 1 : 0,
             arrowRun());

    /*
     * The APRS screens keep their cursor and what has been typed in their own
     * file, and the keys behave differently there: SEND with an empty draft is
     * inert, REPLY with no message selected is inert. Without these the walk
     * would see one "editor" state and test the same empty screen for ever.
     */
    if (ui_aprs_isScreen(state.ui_screen)) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "|%u|%u|%u",
                 (unsigned)ui_aprs_selectedStation(),
                 (unsigned)ui_aprs_selectedMessage(),
                 (unsigned)ui_aprs_editLength());
    }

    return std::string(buf);
}

inline std::string screenName(uint8_t screen = state.ui_screen)
{
    return std::string(ui_dump_screenName(screen));
}

/* ------------------------------------------------------------- the walk --- */

struct KeyPress
{
    uint32_t code;
    bool     longPress;
};

/* Every key the radio's keypad can produce that a screen might need. */
inline const KeyPress *keys()
{
    static const KeyPress k[] = {
        { KEY_ENTER, false }, { KEY_ESC,   false },
        { KEY_UP,    false }, { KEY_DOWN,  false },
        { KEY_F1,    false }, { KEY_F1,    true  },
        { KEY_F2,    false }, { KEY_F2,    true  },
        { KEY_MONI,  false },
        { KEY_1,     false }, { KEY_4,     false }, { KEY_0,    false },
    };
    return k;
}

inline int keyCount()
{
    return 12;
}

/**
 * Walk the interface from the main screen and call `visit` on every distinct
 * state, with the keys that lead to it.
 *
 * A breadth-first search over the real ui_updateFSM(): every state reached is
 * rebuilt from a clean radio (the path is replayed) and passed to the visitor,
 * which draws it if it needs to. The identity of a state is what the next key
 * depends on (see stateId()), so a list cursor reached by three arrows is its
 * own state.
 *
 * @return the number of states walked.
 */
template<typename Visit>
int walk(Visit visit, int maxNodes = 20000, size_t maxDepth = 22)
{
    struct Node
    {
        std::vector<KeyPress> path;
    };

    std::vector<std::pair<std::string, std::vector<KeyPress>>> queue;
    std::map<std::string, bool> seen;

    seedStores();
    resetUi();

    seen[stateId()] = true;
    queue.push_back(std::make_pair(std::string(), std::vector<KeyPress>()));

    size_t head = 0;
    int nodes = 0;

    while ((head < queue.size()) && (nodes < maxNodes)) {
        std::vector<KeyPress> path = queue[head].second;
        head++;
        nodes++;

        resetUi();
        for (const KeyPress &p : path)
            pressKey(p.code, p.longPress);

        visit(path);

        for (int i = 0; i < keyCount(); i++) {
            const KeyPress &k = keys()[i];

            resetUi();
            for (const KeyPress &p : path)
                pressKey(p.code, p.longPress);
            pressKey(k.code, k.longPress);

            std::string id = stateId();

            if (!seen.count(id) && (path.size() + 1 < maxDepth)) {
                std::vector<KeyPress> next = path;
                next.push_back(k);
                seen[id] = true;
                queue.push_back(std::make_pair(id, next));
            }
        }
    }

    return nodes;
}

/** Replay a path from a clean radio: the state the walk found, rebuilt. */
inline void replay(const std::vector<KeyPress> &path)
{
    resetUi();
    for (const KeyPress &p : path)
        pressKey(p.code, p.longPress);
}

} /* namespace ui_walk */

#endif /* UI_WALK_HPP */
