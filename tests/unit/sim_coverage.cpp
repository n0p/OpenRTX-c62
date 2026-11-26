/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The coverage of the simulator's menu tour, checked against the firmware.
 *
 * tools/simulator/tour.py walks every screen of the C62 interface and declares
 * what it covers: the eleven modes of the registry with their quick menu rows,
 * whether each can transmit and which Settings page is theirs; the rows of the
 * top menu and of the Settings list; and every screen of the uiScreen enum,
 * either visited or listed as unreachable with a reason. The declaration is a
 * claim, and a claim maintained by hand goes stale the first time somebody adds
 * a mode or a screen and forgets the tour.
 *
 * This file is what stops that. It builds the same UI configuration as the
 * simulator (the same -D, the same sources) and compares the declaration with
 * what the firmware *registers* and with what its own keys can *reach*:
 *
 *  1. uiModes[] (ui_mode_count/ui_mode_at): title, TX/RX, Settings page and the
 *     labels of the quick menu rows, in order. Add a mode, add a row, flip a
 *     transmitter on -- this fails until the tour declares it.
 *  2. menu_items[] and settings_items[]: the top menu and the Settings list.
 *  3. enum uiScreen through ui_dump_screenName(): every screen the build has is
 *     either in the tour's visited set or in its unreachable set. A new screen
 *     in the enum is not either of those, so it fails.
 *  4. A breadth-first walk over the real UI state machine: from a clean radio
 *     with the stores the tour seeds, press every key from every state it
 *     reaches, and collect the screens that answer. Every screen a key can
 *     reach must be in the tour's visited set: a screen that becomes reachable
 *     -- a new menu row, a new key binding -- is a screen the tour has to walk.
 *
 * The declaration is read from the file meson generates with
 * `tour.py --coverage-manifest` (SIM_TOUR_COVERAGE). Nothing here parses C or
 * trusts a list of names written twice: the firmware is the oracle.
 */

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "core/aprs_service.h"
#include "core/backup_image.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/cps.h"
#include "core/event.h"
#include "core/graphics.h"
#include "core/input.h"
#include "core/messaging.h"
#include "core/msg_service.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "protocols/APRS/aprs.h"
#include "protocols/APRS/aprs_msg.h"
#include "protocols/APRS/ax25.h"
#include "rtx/rtx.h"
#include "ui/ui_aprs.h"
#include "ui/ui_channels.h"
#include "ui/ui_contacts.h"
#include "ui/ui_default.h"
#include "ui/ui_dump.h"
#include "ui/ui_messages.h"
#include "ui/ui_packet.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_spectrum.h"
#include "ui/ui_tools.h"
#include "ui/ui_zones.h"
}

/* The keyboard of the host platform is the emulator's */
#include "emulator.h"

extern state_t state;

namespace
{

/* ------------------------------------------------------------ the claim -- */

struct DeclaredMode
{
    std::string title;
    bool        tx;
    std::string page;
    std::vector<std::string> rows;
};

struct Declared
{
    std::vector<DeclaredMode> modes;
    std::vector<std::string>  top;
    std::vector<std::string>  settings;
    std::vector<std::string>  screens;
    std::vector<std::string>  unreachable;

    bool hasScreen(const std::string &s) const
    {
        for (const std::string &d : screens)
            if (d == s)
                return true;
        return false;
    }

    bool hasUnreachable(const std::string &s) const
    {
        for (const std::string &d : unreachable)
            if (d == s)
                return true;
        return false;
    }
};

std::vector<std::string> splitCsv(const char *s)
{
    std::vector<std::string> out;
    std::string cur;

    for (const char *p = s; ; p++) {
        if ((*p == ',') || (*p == '\0') || (*p == '\n') || (*p == '\r')) {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
            if (*p == '\0')
                break;
        } else if ((*p == '\\') && (p[1] == 's')) {
            /* A space inside a label ("CELL UL"): see coverage_manifest() */
            cur.push_back(' ');
            p++;
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

/** Tokens of a manifest line, by whitespace. */
std::vector<std::string> splitWords(const std::string &line)
{
    std::vector<std::string> out;
    size_t i = 0;

    while (i < line.size()) {
        while ((i < line.size()) && isspace((unsigned char)line[i]))
            i++;
        size_t start = i;
        while ((i < line.size()) && !isspace((unsigned char)line[i]))
            i++;
        if (i > start)
            out.push_back(line.substr(start, i - start));
    }
    return out;
}

/** Everything after the first whitespace, trimmed: a label with spaces. */
std::string restOfLine(const std::string &line)
{
    size_t i = line.find(' ');

    if (i == std::string::npos)
        return std::string();

    i++;
    while ((i < line.size()) && isspace((unsigned char)line[i]))
        i++;

    size_t end = line.size();
    while ((end > i) && isspace((unsigned char)line[end - 1]))
        end--;

    return line.substr(i, end - i);
}

const char *manifestPath()
{
    const char *p = getenv("SIM_TOUR_COVERAGE");

    return (p != nullptr && p[0] != '\0') ? p : nullptr;
}

/** The declaration, read once: it is small and every case needs it. */
const Declared &declared()
{
    static Declared d;
    static bool loaded = false;

    if (loaded)
        return d;
    loaded = true;

    const char *path = manifestPath();
    REQUIRE(path != nullptr);

    FILE *f = fopen(path, "r");
    INFO("manifest: " << path);
    REQUIRE(f != nullptr);

    char buf[1024];

    while (fgets(buf, sizeof(buf), f) != nullptr) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        std::vector<std::string> w = splitWords(line);
        if (w.empty())
            continue;

        if (w[0] == "mode" && w.size() >= 5) {
            DeclaredMode m;
            m.title = w[1];
            m.tx    = (w[2] == "tx");
            m.page  = w[3];
            m.rows  = splitCsv(w[4].c_str());
            d.modes.push_back(m);
        } else if (w[0] == "top") {
            d.top.push_back(restOfLine(line));
        } else if (w[0] == "settings") {
            d.settings.push_back(restOfLine(line));
        } else if (w[0] == "screen") {
            d.screens.push_back(restOfLine(line));
        } else if (w[0] == "unreachable") {
            d.unreachable.push_back(restOfLine(line));
        }
    }

    fclose(f);
    return d;
}

/* ------------------------------------------------------------- harness --- */

/*
 * How many arrows in a row have been pressed. A list keeps its cursor in a
 * private static of its module (the channel bank, the composer, the packet
 * page do), so a state cannot see "row 4 of that list" from the outside; a run
 * of arrows is what gets the walk down a list, and it has to be part of the
 * identity or every row of every list looks like the first one.
 */
int gArrowRun = 0;

void pressKey(uint32_t keys, bool isLong = false)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = isLong ? 1 : 0;

    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();

    if (keys & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))
        gArrowRun = (gArrowRun < 24) ? (gArrowRun + 1) : 24;
    else if (keys != 0)
        gArrowRun = 0;
}

/* The channel bank and the contact book need a port: on the radio it is a
   flash partition, on the host the simulator uses a file. Here a RAM array is
   enough and keeps the walk off the disk. */

uint8_t gFlash[CHSTORE_PARTITION_SIZE];

int ramRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;
    if ((size_t)off + len > sizeof(gFlash))
        return -1;
    memcpy(buf, &gFlash[off], len);
    return 0;
}

int ramErase(void *ctx, uint32_t off, size_t len)
{
    (void)ctx;
    if ((size_t)off + len > sizeof(gFlash))
        return -1;
    memset(&gFlash[off], 0xFF, len);
    return 0;
}

int ramWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    (void)ctx;
    if ((size_t)off + len > sizeof(gFlash))
        return -1;
    memcpy(&gFlash[off], buf, len);
    return 0;
}

const chstorePort ramPort = {
    ramRead, ramErase, ramWrite, nullptr,
    (uint32_t)sizeof(gFlash), CHSTORE_SECTOR_SIZE
};

/*
 * The internal backup needs a port too: two areas in RAM, so the walk can press
 * green on the backup screen and really write a record (the round trip itself
 * is tests/unit/backup_image.cpp's job and the tour's).
 */
uint8_t gBackup[2U * BKP_AREA_SIZE];

int bkpRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;
    if ((size_t)off + len > sizeof(gBackup))
        return -1;
    memcpy(buf, &gBackup[off], len);
    return 0;
}

int bkpErase(void *ctx, uint32_t off, size_t len)
{
    (void)ctx;
    if ((size_t)off + len > sizeof(gBackup))
        return -1;
    memset(&gBackup[off], 0xFF, len);
    return 0;
}

int bkpWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    (void)ctx;
    if ((size_t)off + len > sizeof(gBackup))
        return -1;
    memcpy(&gBackup[off], buf, len);
    return 0;
}

const bkpPort_t bkpRamPort = {
    bkpRead, bkpErase, bkpWrite, nullptr,
    (uint32_t)sizeof(gBackup), 0x1000
};

/** One APRS packet, as the deframer would hand it to the service. */
void injectAprs(const char *from, const char *info)
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
 * The stores the tour seeds, built once: the UI reads them, and rebuilding them
 * on every step of the walk would only rewrite the same bytes.
 */
void seedStores()
{
    memset(gFlash, 0xFF, sizeof(gFlash));
    chstore_bind(&ramPort);
    chstore_init();

    memset(gBackup, 0xFF, sizeof(gBackup));
    bkp_bind(&bkpRamPort);

    channel_t ch = cps_getDefaultChannel();
    ch.rx_frequency = 145500000;
    ch.tx_frequency = 145500000;
    ch.mode         = OPMODE_FM;
    snprintf(ch.name, sizeof(ch.name), "%s", "SEED");

    chstore_channel_t rec;
    memset(&rec, 0, sizeof(rec));
    chstore_fromChannel(&ch, &rec);
    REQUIRE(chstore_add(&rec) >= 0);

    cstore_forget();
    messaging_init();
    messaging_clear();
    REQUIRE(messaging_received(MSG_TRANSPORT_APRS, 1234u, 0u, "hola", false,
                               1000u) >= 0);

    aprs_service_init();
    injectAprs("EA5ABC", "!3852.20N/00004.20W-Bench station");
    injectAprs("EA5XYZ", ":N0CALL   :hola{1");
}

/**
 * A clean radio: every module closed, no popup, no latch, the mode and the
 * channel the tour starts from. The UI state is a static of ui.c and survives
 * from one test case to the next, so every case starts here.
 *
 * The stores are *not* reset: they are seeded once, and a walk that adds a
 * channel or reads a message is only adding reachable screens.
 */
void resetUi(uint8_t mode = OPMODE_FM, freq_t freq = 145500000)
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
    gArrowRun = 0;
}

/** Index of the top menu row with this name, -1 when the build has none. */
int menuRowOf(const char *name)
{
    for (uint8_t i = 0; i < menu_num; i++)
        if (strcmp(menu_items[i], name) == 0)
            return i;
    return -1;
}

/** Name of a screen of the enum, "UNKNOWN" past the end. */
std::string screenName(uint8_t screen)
{
    return std::string(ui_dump_screenName(screen));
}

/* --------------------------------------------------------------- walk ---- */

struct KeyPress
{
    uint32_t code;
    bool     longPress;
};

/*
 * Every key the radio's keypad can produce. The knobs are the same events as
 * UP/DOWN on this platform and MONI is the macro grid, so one pass of each is
 * enough; a binding that only a knob reaches would be a bug of its own.
 */
const KeyPress KEYS[] = {
    { KEY_ENTER, false }, { KEY_ESC,   false },
    { KEY_UP,    false }, { KEY_DOWN,  false },
    { KEY_F1,    false }, { KEY_F1,    true  },
    { KEY_F2,    false }, { KEY_F2,    true  },
    { KEY_MONI,  false },
    { KEY_1,     false }, { KEY_2,     false }, { KEY_3,     false },
    { KEY_4,     false }, { KEY_5,     false }, { KEY_9,     false },
    { KEY_0,     false }, { KEY_STAR,  false }, { KEY_HASH,  false },
};

constexpr int MAX_NODES  = 40000;
constexpr size_t MAX_DEPTH = 28;

/**
 * Identity of a UI state for the walk: what the next key does depends on which
 * screen is up, which overlay owns the keypad, which table the popup shows and
 * where its cursor is. Two states with the same key are the same state for the
 * purpose of finding screens.
 */
std::string stateId()
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
             gArrowRun);

    return std::string(buf);
}

} /* namespace */

/* =========================================== 1. the registry of modes ==== */

TEST_CASE("The mode registry is the one the tour declares", "[sim][coverage]")
{
    const Declared &d = declared();

    REQUIRE(d.modes.size() == (size_t)ui_mode_count());

    for (uint8_t i = 0; i < ui_mode_count(); i++) {
        const ui_mode_t *m = ui_mode_at(i);
        const DeclaredMode &want = d.modes[i];

        INFO("mode " << (int)i << ": firmware \"" << m->title
                     << "\", tour \"" << want.title << "\"");
        REQUIRE(std::string(m->title) == want.title);

        /* TX/RX: the arrow the tools menu prints, from the same query */
        REQUIRE(ui_mode_canTx(m->mode) == want.tx);

        /* The rows of the quick menu, in order */
        REQUIRE((size_t)m->count == want.rows.size());
        for (uint8_t r = 0; r < m->count; r++) {
            INFO("mode " << m->title << ", row " << (int)r << ": firmware \""
                         << m->items[r].label << "\", tour \""
                         << want.rows[r] << "\"");
            REQUIRE(std::string(m->items[r].label) == want.rows[r]);
        }

        /* And the Settings page it opens, walked like the operator does */
        resetUi(m->mode);
        pressKey(KEY_ENTER);
        REQUIRE(state.ui_screen == MENU_TOP);
        int row = menuRowOf("Settings");
        REQUIRE(row >= 0);
        ui_menu_select((uint8_t)row);
        pressKey(KEY_ENTER);
        REQUIRE(state.ui_screen == MENU_SETTINGS);

        int page = -1;
        for (uint8_t s = 0; s < settings_num; s++)
            if (strcmp(settings_items[s], m->title) == 0)
                page = s;

        INFO("mode " << m->title << " is not in the Settings list");
        REQUIRE(page >= 0);
        ui_menu_select((uint8_t)page);
        pressKey(KEY_ENTER);

        INFO("mode " << m->title << ": page opened as "
                     << screenName(state.ui_screen) << ", tour says "
                     << want.page);
        REQUIRE(screenName(state.ui_screen) == want.page);

        pressKey(KEY_ESC);
        pressKey(KEY_ESC);
        REQUIRE(state.ui_screen == MENU_TOP);
        pressKey(KEY_ESC);
        REQUIRE(state.ui_screen == MAIN_VFO);
    }
}

/* ================================== 2. the top menu and Settings list ==== */

TEST_CASE("The top menu and the Settings list are the ones the tour declares",
          "[sim][coverage]")
{
    const Declared &d = declared();

    REQUIRE(d.top.size() == (size_t)menu_num);
    for (uint8_t i = 0; i < menu_num; i++) {
        INFO("top menu row " << i);
        REQUIRE(std::string(menu_items[i]) == d.top[i]);
    }

    REQUIRE(d.settings.size() == (size_t)settings_num);
    for (uint8_t i = 0; i < settings_num; i++) {
        INFO("settings row " << i);
        REQUIRE(std::string(settings_items[i]) == d.settings[i]);
    }
}

/* ============================ 3. every screen is visited or unreachable === */

TEST_CASE("Every screen of the build is visited by the tour or declared "
          "unreachable", "[sim][coverage]")
{
    const Declared &d = declared();

    /*
     * Every value the name table knows, not just up to the last enumerator the
     * test was written with: a screen appended to enum uiScreen is exactly the
     * case this is here to catch, and it sorts after the ones the tour knows.
     */
    std::vector<std::string> enumScreens;
    for (int s = 0; s <= 255; s++) {
        std::string n = screenName((uint8_t)s);
        if (n == "UNKNOWN")
            continue;
        enumScreens.push_back(n);
    }

    REQUIRE(enumScreens.size() > 30);

    for (const std::string &n : enumScreens) {
        INFO("screen " << n << " is in the enum but neither visited nor "
                       << "declared unreachable by the tour");
        REQUIRE((d.hasScreen(n) || d.hasUnreachable(n)));
    }

    /* And the other way: a name in the declaration that the enum does not have
       is a typo or a stale entry, and would silently stop checking anything */
    for (const std::string &n : d.screens) {
        bool found = false;
        for (const std::string &e : enumScreens)
            if (e == n)
                found = true;
        INFO("the tour declares screen " << n << ", the enum has no such name");
        REQUIRE(found);
        REQUIRE_FALSE(d.hasUnreachable(n));
    }

    for (const std::string &n : d.unreachable) {
        bool found = false;
        for (const std::string &e : enumScreens)
            if (e == n)
                found = true;
        INFO("the tour declares " << n << " unreachable, the enum has no such "
                                  << "name");
        REQUIRE(found);
        REQUIRE_FALSE(d.hasScreen(n));
    }
}

/* ================== 4. every screen the keys reach is in the tour ======== */

TEST_CASE("Every screen the keys can reach is visited by the tour",
          "[sim][coverage][walk]")
{
    const Declared &d = declared();

    seedStores();

    struct Node
    {
        std::vector<KeyPress> path;
        std::string id;
        uint8_t screen;
    };

    std::map<std::string, bool> seen;
    std::deque<Node> queue;
    std::vector<std::string> reached;      /* for the failure message */
    std::set<std::string> reachedSet;

    resetUi();
    Node start;
    start.id     = stateId();
    start.screen = state.ui_screen;
    seen[start.id] = true;
    queue.push_back(start);

    int nodes = 0;

    while (!queue.empty() && (nodes < MAX_NODES)) {
        Node n = queue.front();
        queue.pop_front();
        nodes++;

        for (const KeyPress &k : KEYS) {
            resetUi();

            for (const KeyPress &p : n.path)
                pressKey(p.code, p.longPress);

            pressKey(k.code, k.longPress);

            std::string id = stateId();
            std::string name = screenName(state.ui_screen);

            if (!reachedSet.count(name)) {
                reachedSet.insert(name);
                reached.push_back(name);
            }

            if (!seen.count(id) && (n.path.size() + 1 < MAX_DEPTH)) {
                Node next;
                next.path   = n.path;
                next.path.push_back(k);
                next.id     = id;
                next.screen = state.ui_screen;
                seen[id]    = true;
                queue.push_back(next);
            }
        }
    }

    std::string list;
    for (const std::string &r : reached)
        list += r + " ";

    printf("[coverage] key walk: %d states%s, %d screens reached: %s\n",
           nodes, queue.empty() ? "" : " (truncated at the node cap)",
           (int)reached.size(), list.c_str());
    fflush(stdout);

    INFO("walked " << nodes << " states");
    INFO("reached screens: " << list);

    for (const std::string &r : reached) {
        INFO("the keys reach " << r << " and the tour does not visit it");
        REQUIRE(d.hasScreen(r));
    }

    /* The walk has to be able to reach the modes' screens at all: if it cannot
       see a single one of them it is measuring nothing */
    REQUIRE(reachedSet.count("MAIN_VFO"));
    REQUIRE(reachedSet.count("MENU_TOP"));
    REQUIRE(reachedSet.count("TOOLS_MENU"));
    REQUIRE(reachedSet.count("SETTINGS_MODE"));
    REQUIRE(reached.size() >= 30);
}
