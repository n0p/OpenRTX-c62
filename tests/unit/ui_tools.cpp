/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The tools menu (P1 held down on a main screen, docs/UI.md "Herramientas"),
 * driven through the real UI FSM with key events, the way the keypad feeds it.
 *
 * What is asserted here is the part of the feature that only the interface can
 * get wrong:
 *
 *   - P1 long opens the menu from MAIN_VFO and from MAIN_MEM, and red gives the
 *     radio back exactly where it was, with the keypad answering again;
 *   - the menu claims every key while it is open (one block, one claim), the
 *     macro grid included, and the transmitter is disabled for the whole time;
 *   - the five rows are walked with UP/DOWN and with P1/P2, wrapping at both
 *     ends, and every row starts the service it names -- the scanner, the VFO
 *     carrier search, the analyser, the channel list -- with the service's own
 *     status as the witness, not a flag of the menu;
 *   - the mode submenu lists the compiled modes, opens on the mode in use,
 *     marks the receive-only ones, applies the chosen mode through
 *     state.channel.mode + the RTX synchronisation request, and returns to the
 *     main screen;
 *   - no row is a no-op, and no path leaves the UI deaf.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <vector>

extern "C" {
#include "core/channel_store.h"
#include "core/event.h"
#include "core/state.h"
#include "core/ui.h"
#include "interfaces/delays.h"
#include "core/voicePrompts.h"
#include "rtx/rtx.h"
#include "ui/ui_channels.h"
#include "ui/ui_default.h"
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
}

extern state_t state;

namespace
{

constexpr freq_t VFO_FREQ = 145500000;   /* 2 m: a band both services know */
constexpr freq_t CH_FREQ  = 430250000;

/** Enough flash for the bank: two 8 KiB slots. */
class FakeFlash
{
public:
    FakeFlash() : mem(0x4000, 0xFF) {}

    int read(uint32_t off, void *buf, size_t len)
    {
        if ((off + len) > mem.size())
            return -1;
        memcpy(buf, &mem[off], len);
        return 0;
    }

    int erase(uint32_t off, size_t len)
    {
        if ((off + len) > mem.size())
            return -1;
        memset(&mem[off], 0xFF, len);
        return 0;
    }

    int write(uint32_t off, const void *buf, size_t len)
    {
        if ((off + len) > mem.size())
            return -1;
        memcpy(&mem[off], buf, len);
        return 0;
    }

    std::vector<uint8_t> mem;
};

FakeFlash *flash;
chstorePort port;

int ffRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->read(off, buf, len);
}

int ffErase(void *ctx, uint32_t off, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->erase(off, len);
}

int ffWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->write(off, buf, len);
}

/** One key event; returns whether the UI asked for an RTX resynchronisation. */
bool pressKey(uint32_t keys, bool isLong = false)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = isLong ? 1 : 0;

    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();

    return sync;
}

/** A key press that must be claimed exactly once; returns the claiming block. */
uint8_t claimOf(uint32_t keys, bool isLong = false)
{
    pressKey(keys, isLong);

    REQUIRE(ui_lastClaimCount() == 1);
    REQUIRE(ui_lastClaim() != UI_CLAIM_NONE);

    return ui_lastClaim();
}

void tick(void)
{
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
}

/** Put every module and service back to "nothing running". */
void resetUi(uint8_t screen = MAIN_VFO)
{
    if (ui_spectrum_isOpen())
        ui_spectrum_leave();
    while (ui_tools_isOpen())
        pressKey(KEY_ESC);
    while (ui_channels_isOpen())
        pressKey(KEY_ESC);

#ifdef CONFIG_SCAN
    scan_service_stop(false);
#endif
#ifdef CONFIG_VFO_SEARCH
    vfoSearchService_stop();
#endif

    qm_close();
    qm_page_close();
    macro_latched = false;
    ui_setInputLock(false);

    state.channel.mode          = OPMODE_FM;
    state.channel.rx_frequency  = VFO_FREQ;
    state.channel.tx_frequency  = VFO_FREQ;
    state.channel.bandwidth     = BW_25;
    state.channel.power         = 1000;
    state.channel.rx_only       = false;
    state.channel.name[0]       = '\0';
    state.channel_index         = CHSTORE_NONE;
    state.vfo_channel           = state.channel;
    state.ui_screen             = screen;
    state.tuner_mode            = (screen == MAIN_MEM) ? CH : VFO;
    state.txDisable             = false;
    state.charge                = 100;
    state.emergency             = false;
    state.step_index            = 4;
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;      /* silent in the harness */
    ui_rtxSyncRequest           = false;
}

/** Two channels in the bank, so the memory scanner has something to walk. */
void bootBank(void)
{
    flash = new FakeFlash();
    port.read   = ffRead;
    port.erase  = ffErase;
    port.write  = ffWrite;
    port.ctx    = flash;
    port.size   = (uint32_t)flash->mem.size();
    port.sector = CHSTORE_SECTOR_SIZE;

    chstore_bind(&port);
    REQUIRE(chstore_init() == 0);

    for (int i = 0; i < 2; i++) {
        chstore_channel_t c;
        memset(&c, 0, sizeof(c));
        snprintf(c.name, sizeof(c.name), "CH%d", i + 1);
        c.rx_freq   = CH_FREQ + (uint32_t)i * 25000u;
        c.tx_freq   = c.rx_freq;
        c.power     = 1000;
        c.mode      = OPMODE_FM;
        c.bandwidth = BW_12_5;
        c.flags     = CHSTORE_FLAG_SIMPLEX;
        REQUIRE(chstore_add(&c) == i);
    }

    REQUIRE(chstore_count() == 2);

    /*
     * Bind the service to this bank.  Without it the scanner has no list,
     * chstore_count() still says there is something to walk, and a start goes
     * straight to the end of the range while reporting that it is running: a
     * scan test would pass without the scan ever tuning anything.
     */
    scan_service_init();
}

/** Row index of a label in the screen that is on, -1 when it is not there. */
int rowOf(const char *label)
{
    for (uint8_t i = 0; i < ui_tools_rowCount(); i++) {
        const char *l = ui_tools_rowLabel(i);

        if ((l != NULL) && (strcmp(l, label) == 0))
            return (int)i;
    }

    return -1;
}

/** Open the menu with P1 long and move the cursor to the named row. */
void openOn(const char *label)
{
    pressKey(KEY_F1, true);
    REQUIRE(ui_tools_isOpen());

    int target = rowOf(label);
    REQUIRE(target >= 0);

    for (int guard = 0; (guard < 16) && (ui_tools_selected() != target); guard++)
        pressKey(KEY_DOWN);

    REQUIRE(ui_tools_selected() == target);
}

/** Green on the row that opens the channel list: the shortcut P1 long had. */
void openChannels(void)
{
    openOn("Channels");
    pressKey(KEY_ENTER);
    REQUIRE(ui_channels_isOpen());
}

} /* namespace */

/* ============================================================ open/close == */

TEST_CASE("P1 opens the tools menu on both main screens with a tap", "[ui][tools]")
{
    for (uint8_t screen : { MAIN_VFO, MAIN_MEM }) {
        resetUi(screen);
        ui_rtxSyncRequest = false;

        INFO("from screen " << (int)screen);
        /* A tap, not a hold: the release of a short press opens it */
        REQUIRE(claimOf(KEY_F1) == UI_CLAIM_TOOLS_OPEN);

        REQUIRE(ui_tools_isOpen());
        REQUIRE_FALSE(ui_tools_inModes());
        REQUIRE(state.ui_screen == TOOLS_MENU);
        REQUIRE(ui_tools_backScreen() == screen);

        /* The menu owns the transmitter for as long as it is up */
        REQUIRE(state.txDisable);
        REQUIRE(ui_rtxSyncRequest);

        /* Red closes it and gives the screen back, with the PTT re-armed */
        REQUIRE(claimOf(KEY_ESC) == UI_CLAIM_TOOLS);
        REQUIRE_FALSE(ui_tools_isOpen());
        REQUIRE(state.ui_screen == screen);
        REQUIRE_FALSE(state.txDisable);

        tick();

        /* And the screen it went back to still answers: not deaf */
        REQUIRE(claimOf(KEY_ENTER) == UI_CLAIM_SCREEN);
        REQUIRE(state.ui_screen == MENU_TOP);
        pressKey(KEY_ESC);
        REQUIRE(state.ui_screen == screen);
    }
}

TEST_CASE("the tools menu claims every key while it is open", "[ui][tools]")
{
    resetUi();

    const uint32_t keys[] = { KEY_ENTER, KEY_ESC, KEY_UP, KEY_DOWN, KEY_F1,
                              KEY_F1 | 0, KEY_F2, KEY_MONI, KEY_1, KEY_0,
                              KEY_HASH, KEY_STAR };
    const bool longs[]    = { false, true };

    pressKey(KEY_F1, true);
    REQUIRE(ui_tools_isOpen());

    for (bool isLong : longs) {
        for (uint32_t key : keys) {
            /* Red and green have their own tests below: here the point is who
               answers, and only the menu may answer */
            if ((key == KEY_ESC) || (key == KEY_ENTER))
                continue;

            INFO("key " << key << (isLong ? " (long)" : ""));
            REQUIRE(claimOf(key, isLong) == UI_CLAIM_TOOLS);
            REQUIRE(ui_tools_isOpen());
        }
    }

    /* MONI, which elsewhere opens the macro grid, does not paint it here */
    pressKey(KEY_MONI);
    REQUIRE(ui_tools_isOpen());

    REQUIRE(claimOf(KEY_ESC) == UI_CLAIM_TOOLS);
    REQUIRE_FALSE(ui_tools_isOpen());
}

/* ============================================================== the rows == */

TEST_CASE("the list walks its rows and wraps at both ends", "[ui][tools]")
{
    resetUi();
    pressKey(KEY_F1, true);

    /* The rows of this build: the conditional ones follow their compile flag */
    std::vector<std::string> labels;
    for (uint8_t i = 0; i < ui_tools_rowCount(); i++)
        labels.push_back(ui_tools_rowLabel(i));

    REQUIRE(labels[0] == "Mode");
#ifdef CONFIG_SCAN
    REQUIRE(labels[1] == "Scan");
    REQUIRE(labels[2] == "VFO scan");
#elif defined(CONFIG_VFO_SEARCH)
    REQUIRE(labels[1] == "VFO scan");
#endif
    /* The accessory port is one row of this menu, after the two screens
       (docs/ACCESSORY.md): it used to be a row of every mode's table */
    REQUIRE(labels[labels.size() - 3] == "Spectrum");
    REQUIRE(labels[labels.size() - 2] == "Channels");
    REQUIRE(labels.back() == "Puerto");

    /* No row without the thing it starts, no row that does nothing */
    REQUIRE(ui_tools_rowCount() >= 3);

    const uint8_t count = ui_tools_rowCount();

    /* Down to the last row, then round to the first one */
    for (uint8_t i = 1; i < count; i++) {
        pressKey(KEY_DOWN);
        REQUIRE(ui_tools_selected() == i);
    }

    pressKey(KEY_DOWN);
    REQUIRE(ui_tools_selected() == 0);

    /* Up from the top wraps to the bottom */
    pressKey(KEY_UP);
    REQUIRE(ui_tools_selected() == count - 1);

    /* P1 and P2 are the same two moves (there is no arrow key... and no P1
       either: the operator has both) */
    pressKey(KEY_F1);
    REQUIRE(ui_tools_selected() == count - 2);
    pressKey(KEY_F2);
    REQUIRE(ui_tools_selected() == count - 1);

    /* Every row has a value or is an action with nothing to say: the two the
       operator can be misled by do say something */
    char buf[16];
    REQUIRE(strcmp(ui_tools_rowValue(0, buf, sizeof(buf)), "FM") == 0);
}

TEST_CASE("every row starts the service it names", "[ui][tools]")
{
    /* ---- Scan: the memory scanner of core/scan_service.c ---- */
#ifdef CONFIG_SCAN
    SECTION("Scan starts the memory scanner") {
        bootBank();
        resetUi();
        openOn("Scan");

        REQUIRE(pressKey(KEY_ENTER));
        REQUIRE_FALSE(ui_tools_isOpen());
        REQUIRE(state.ui_screen == MAIN_VFO);

        struct scanStatus st;
        REQUIRE(scan_service_status(&st));
        REQUIRE(st.running);
        REQUIRE(st.mode == SCAN_MODE_MEMORY);
        REQUIRE(state.tuner_mode == CHSCAN);
        /* The PTT is armed again: a scan is stopped by transmitting */
        REQUIRE_FALSE(state.txDisable);

        scan_service_stop(false);
    }

    SECTION("opening the menu stops a running memory scan") {
        bootBank();
        resetUi();

        REQUIRE(scan_service_start(SCAN_MODE_MEMORY, false));
        struct scanStatus st;
        REQUIRE(scan_service_status(&st));
        REQUIRE(st.running);

        /* The menu is the door to a manual change: the scan stops first */
        pressKey(KEY_F1, true);
        REQUIRE(ui_tools_isOpen());
        REQUIRE(scan_service_status(&st));
        REQUIRE_FALSE(st.running);

        scan_service_stop(false);
    }

    SECTION("Scan with an empty bank beeps and leaves the radio alone") {
        resetUi();
        openOn("Scan");

        /* No bank: the service refuses, the menu is already closed and the
           radio is where it was, not in a scan of nothing */
        while (chstore_count() > 0)
            chstore_delete(0);
        pressKey(KEY_ENTER);

        struct scanStatus st;
        REQUIRE(scan_service_status(&st));
        REQUIRE_FALSE(st.running);
        REQUIRE(state.tuner_mode == VFO);
    }
#endif

    /* ---- VFO scan: the carrier search of core/vfo_search_service.c ---- */
#ifdef CONFIG_VFO_SEARCH
    SECTION("VFO scan starts the carrier search") {
        resetUi();
        openOn("VFO scan");

        REQUIRE(pressKey(KEY_ENTER));
        REQUIRE_FALSE(ui_tools_isOpen());
        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE(vfoSearchService_active());

        struct vfoSearchStatus st;
        REQUIRE(vfoSearchService_status(&st));
        REQUIRE(st.direction == VFOS_UP);

        vfoSearchService_stop();
    }

    SECTION("VFO scan from the memory screen gives the VFO back and sweeps") {
        bootBank();
        resetUi(MAIN_MEM);

        /* The memory screen is on a channel; the search only runs on the VFO */
        state.channel_index = 0;
        REQUIRE(chstore_toChannel(0, &state.channel) == 0);
        state.channel.rx_frequency = CH_FREQ;
        state.channel.tx_frequency = CH_FREQ;
        state.vfo_channel.rx_frequency = VFO_FREQ;
        state.vfo_channel.tx_frequency = VFO_FREQ;

        openOn("VFO scan");
        REQUIRE(pressKey(KEY_ENTER));

        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE(state.channel.rx_frequency == VFO_FREQ);
        REQUIRE(vfoSearchService_active());

        vfoSearchService_stop();
    }
#endif

    /* ---- Spectrum: the analyser's sweep ---- */
    SECTION("Spectrum opens the analyser") {
        resetUi();
        openOn("Spectrum");

        /* The analyser refuses when the receiver cannot sweep (a digital mode
           owns the discriminator); FM accepts, so this section sweeps */
        pressKey(KEY_ENTER);

        REQUIRE(ui_spectrum_isOpen());
        REQUIRE(state.ui_screen == SPECTRUM);
        REQUIRE(state.txDisable);

        ui_spectrum_leave();
    }

    /* ---- Channels: the shortcut P1 long used to be ---- */
    SECTION("Channels opens the channel list") {
        resetUi();
        openOn("Channels");

        pressKey(KEY_ENTER);

        REQUIRE(ui_channels_isOpen());
        /* The list owns the keypad, so the transmitter stays disabled */
        REQUIRE(state.txDisable);

        pressKey(KEY_ESC);
        REQUIRE_FALSE(ui_channels_isOpen());
        REQUIRE_FALSE(state.txDisable);
        REQUIRE(state.ui_screen == MAIN_VFO);

        /* And the radio answers again */
        REQUIRE(claimOf(KEY_ENTER) == UI_CLAIM_SCREEN);
    }
}

/* ================================================================= locking == */

#ifdef CONFIG_SCAN
/*
 * The UI thread calls ui_updateFSM() with state_mutex held (threads.c), so
 * every service a key starts runs inside that mutex.  state_mutex is created
 * with default attributes -- PTHREAD_MUTEX_NORMAL -- and a second acquisition
 * by its owner does not fail: in Zephyr the owner that re-locks the mutex it
 * holds sleeps for ever (zephyr-listenai/lib/posix/pthread_mutex.c), and on
 * glibc it blocks just the same.  The UI task then stops with the mutex in its
 * hand, state_task() waits on it for the battery read, and the black box
 * reboots the radio two seconds later.  That was Tools > Scan memory with one
 * channel in the bank; core/vfo_search_service.c documents the same trap.
 *
 * These cases reproduce the thread, not just the call.  A regression leaves the
 * worker parked on the mutex; the deadline fails the case, the mutex is then
 * released so the worker can finish, and the rest of the binary is not left
 * waiting on a lock a dead case took.
 */
template <typename F>
bool runUnderStateLock(F fn, uint32_t timeoutMs = 2000)
{
    std::atomic<bool> done{false};

    pthread_mutex_lock(&state_mutex);
    std::thread worker([&]() {
        fn();
        done = true;
    });

    uint32_t waited = 0;
    while (!done && (waited < timeoutMs)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        waited += 2;
    }

    /* Read the verdict before the unlock: the worker finishes as soon as it is
     * released, and that is exactly the case this has to fail. */
    bool finished = done.load();

    pthread_mutex_unlock(&state_mutex);
    worker.join();

    return finished;
}

TEST_CASE("Scan memory starts and stops from a key without re-locking the state",
          "[ui][tools][scan]")
{
    bootBank();
    resetUi();
    openOn("Scan");

    /* Green on the row, from the UI thread's pass: start with the mutex held */
    REQUIRE(runUnderStateLock([]() { pressKey(KEY_ENTER); }));

    struct scanStatus st;
    REQUIRE(scan_service_status(&st));
    REQUIRE(st.running);
    REQUIRE_FALSE(ui_tools_isOpen());
    REQUIRE(state.tuner_mode == CHSCAN);

    /* P1 long opens the menu and stops the scan -- also inside the pass */
    REQUIRE(runUnderStateLock([]() { pressKey(KEY_F1, true); }));
    REQUIRE(ui_tools_isOpen());

    REQUIRE(scan_service_status(&st));
    REQUIRE_FALSE(st.running);
    REQUIRE(state.tuner_mode == VFO);

    /* Out of the menu for the next case, as the harness expects */
    while (ui_tools_isOpen())
        pressKey(KEY_ESC);
}

TEST_CASE("The scan poll hands the state back instead of waiting for the UI",
          "[ui][tools][scan]")
{
    /*
     * state_task() polls with the mutex free, and it is the thread that feeds
     * the application watchdog and the TX supervisor: it may wait for the UI
     * for a moment, never for as long as a screen takes.  A retune it cannot
     * take is skipped and retried on the next tick; a scan is not stopped by a
     * busy UI.
     *
     * The first loop proves the path under test is the one that retunes -- with
     * a free mutex the scan walks the bank; the second one repeats the same
     * polls with the mutex held by the UI thread and only asks that they come
     * back.
     */
    bootBank();
    resetUi();
    REQUIRE(scan_service_start(SCAN_MODE_MEMORY, false));

    struct scanStatus st;
    REQUIRE(scan_service_status(&st));
    REQUIRE(st.running);
    uint16_t first = state.channel_index;
    bool walked = false;
    for (int i = 0; (i < 100) && !walked; i++) {
        delayMs(5);
        scan_service_poll();
        walked = (state.channel_index != first);
    }
    REQUIRE(walked);

    REQUIRE(runUnderStateLock([]() {
        /* Longer than one settle plus one dwell, so a retune is due inside */
        for (int i = 0; i < 60; i++) {
            delayMs(5);
            scan_service_poll();
        }
    }, 5000));

    REQUIRE(scan_service_status(&st));
    REQUIRE(st.running);

    scan_service_stop(false);
}
#endif

/* ============================================================ mode submenu == */

TEST_CASE("the mode submenu opens on the mode in use and marks RX only modes",
          "[ui][tools]")
{
    resetUi();

    /* FM: the first entry of the registry, and the one in use */
    openOn("Mode");
    pressKey(KEY_ENTER);

    REQUIRE(ui_tools_inModes());
    REQUIRE(ui_tools_rowCount() == ui_mode_count());
    REQUIRE(ui_tools_rowCount() > 1);

    const ui_mode_t *current = ui_mode_at(ui_tools_selected());
    REQUIRE(current != nullptr);
    REQUIRE(current->mode == OPMODE_FM);
    REQUIRE(strcmp(ui_tools_rowLabel(ui_tools_selected()), current->title) == 0);

    /* Every mode of the registry is offered, with what it can do */
    char buf[16];
    for (uint8_t i = 0; i < ui_tools_rowCount(); i++) {
        const ui_mode_t *m = ui_mode_at(i);

        REQUIRE(m != nullptr);
        REQUIRE(strcmp(ui_tools_rowLabel(i), m->title) == 0);
        REQUIRE(strcmp(ui_tools_rowValue(i, buf, sizeof(buf)),
                       ui_mode_canTx(m->mode) ? "TX" : "RX") == 0);
    }

    /* A receive-only mode is marked as such: the operator cannot be misled */
    int tetra = rowOf("TETRA");
    if (tetra >= 0)
        REQUIRE(strcmp(ui_tools_rowValue((uint8_t)tetra, buf, sizeof(buf)),
                       "RX") == 0);
    REQUIRE(strcmp(ui_tools_rowValue(0, buf, sizeof(buf)), "TX") == 0);

    /* Red in the submenu goes back to the list, not out of the menu */
    REQUIRE(claimOf(KEY_ESC) == UI_CLAIM_TOOLS);
    REQUIRE(ui_tools_isOpen());
    REQUIRE_FALSE(ui_tools_inModes());
    REQUIRE(strcmp(ui_tools_rowLabel(ui_tools_selected()), "Mode") == 0);
}

TEST_CASE("choosing a mode applies it and returns to the main screen",
          "[ui][tools]")
{
    resetUi();

    /* Open the submenu with the radio in another mode: the cursor must be on
       it, not on the first entry */
    state.channel.mode = OPMODE_DMR;
    openOn("Mode");
    pressKey(KEY_ENTER);
    REQUIRE(ui_tools_inModes());

    const ui_mode_t *inUse = ui_mode_at(ui_tools_selected());
    REQUIRE(inUse != nullptr);
    REQUIRE(inUse->mode == OPMODE_DMR);

    /* Pick FM, which is the first entry of the registry */
    int fm = rowOf("FM");
    REQUIRE(fm == 0);
    while (ui_tools_selected() != fm)
        pressKey(KEY_DOWN);

    REQUIRE(pressKey(KEY_ENTER));

    REQUIRE_FALSE(ui_tools_isOpen());
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.channel.mode == OPMODE_FM);
    REQUIRE_FALSE(state.txDisable);

    /* A receive-only mode keeps one frequency: TX follows RX, so nothing
       downstream is left with the transmit frequency of the mode before */
    state.channel.rx_frequency = 430000000;
    state.channel.tx_frequency = 145500000;

    openOn("Mode");
    pressKey(KEY_ENTER);

    int tetra = rowOf("TETRA");
    REQUIRE(tetra >= 0);
    while (ui_tools_selected() != tetra)
        pressKey(KEY_DOWN);
    REQUIRE(pressKey(KEY_ENTER));

    REQUIRE(state.channel.mode == OPMODE_TETRA);
    REQUIRE_FALSE(opmode_canTransmit(OPMODE_TETRA));
    REQUIRE(state.channel.tx_frequency == state.channel.rx_frequency);

    /* The UI is not deaf after the change */
    REQUIRE(claimOf(KEY_ENTER) == UI_CLAIM_SCREEN);
    REQUIRE(state.ui_screen == MENU_TOP);
}
