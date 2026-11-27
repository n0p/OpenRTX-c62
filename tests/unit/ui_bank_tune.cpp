/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Green on an entry of the in-radio channel bank tunes it, and the radio says
 * so.
 *
 * The bank (P1 long, docs/CHANNELS.md) used to tune the frequency and leave the
 * screen on MAIN_VFO: state.channel and state.channel_index were updated but
 * neither the screen nor tuner_mode followed, so the main screen kept drawing a
 * VFO, the channel's name and its index were never shown, and the PC protocol
 * reported the radio as being on a VFO (it prints 0xFFFF unless tuner_mode is
 * not VFO). Tuning the same channel through Menu -> Banks, the CPS path, did
 * land on MAIN_MEM, which is how the difference was found.
 *
 * The test drives the real dispatch: a long P1 from the main screen, green on
 * the entry, then the state a screen and a protocol client read.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

extern "C" {
#include "core/channel_store.h"
#include "core/event.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/voicePrompts.h"
#include "ui/ui_channels.h"
#include "ui/ui_default.h"
#include "ui/ui_quickmenu.h"
#include "ui/ui_tools.h"
}

extern state_t state;

namespace
{

constexpr freq_t VFO_FREQ = 145500000;
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
}

/** A clean radio with one channel in the bank. */
void resetUi(void)
{
    while (ui_channels_isOpen())
        pressKey(KEY_ESC);

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
    state.ui_screen             = MAIN_VFO;
    state.tuner_mode            = VFO;
    state.txDisable             = false;
    state.unattendedTxDisable = false;
    state.charge                = 100;
    state.emergency             = false;
    state.settings              = default_settings;
    state.settings.vpLevel      = vpNone;
    ui_rtxSyncRequest           = false;
}


/**
 * The channel list, from the main screen.
 *
 * P1 long used to open it; it now opens the tools menu, whose last row is the
 * list (docs/UI.md). The row is found by name, so a build that compiles the
 * scanner out still lands on it.
 */
void openChannels(void)
{
    pressKey(KEY_F1, true);
    REQUIRE(ui_tools_isOpen());

    for (int guard = 0; guard < 8; guard++) {
        const char *label = ui_tools_rowLabel(ui_tools_selected());

        if ((label != NULL) && (strcmp(label, "Channels") == 0))
            break;
        pressKey(KEY_DOWN);
    }

    pressKey(KEY_ENTER);
    REQUIRE(ui_channels_isOpen());
}

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

    chstore_channel_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "REPEATER");
    c.rx_freq = CH_FREQ;
    c.tx_freq = CH_FREQ + 600000;
    c.power   = 1000;
    c.mode    = OPMODE_FM;
    c.bandwidth = BW_12_5;
    c.flags   = 0;

    REQUIRE(chstore_add(&c) == 0);
    REQUIRE(chstore_count() == 1);
}

} /* namespace */

TEST_CASE("green on a bank entry loads it as a memory channel", "[ui][channels]")
{
    bootBank();
    resetUi();

    /* The bank is the last row of the tools menu P1 long opens */
    openChannels();

    pressKey(KEY_ENTER);

    REQUIRE_FALSE(ui_channels_isOpen());
    REQUIRE(state.ui_screen == MAIN_MEM);
    REQUIRE(state.tuner_mode == CH);
    REQUIRE(state.channel_index == 0);
    REQUIRE(state.channel.rx_frequency == CH_FREQ);
    REQUIRE(state.channel.tx_frequency == CH_FREQ + 600000);
    REQUIRE(strcmp(state.channel.name, "REPEATER") == 0);

    /* The last used channel is remembered for the next power-on */
    REQUIRE(chstore_lastUsed() == 0);
}

TEST_CASE("a channel at 6.25 kHz is a valid record", "[store][channels]")
{
    bootBank();

    /*
     * The FM quick menu walks 12.5 / 25 / 6.25 kHz (docs/UI.md) and the channel
     * editor carries that bandwidth into the new record, but chstore_valid()
     * only accepted 12.5 and 25: a channel created right after picking 6.25 in
     * the popup could never be saved, and the editor answered "INVALID
     * CHANNEL" -- a message that names none of the reasons.
     */
    chstore_channel_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "NARROW");
    c.rx_freq = CH_FREQ;
    c.tx_freq = CH_FREQ;
    c.power   = 1000;
    c.mode    = OPMODE_FM;
    c.bandwidth = BW_6_25;

    REQUIRE(chstore_valid(&c));
    REQUIRE(chstore_add(&c) >= 0);

    chstore_channel_t saved;
    REQUIRE(chstore_get(1, &saved) == 0);
    REQUIRE(saved.bandwidth == BW_6_25);
}

TEST_CASE("a channel created with 6.25 kHz selected is saved", "[ui][channels]")
{
    bootBank();
    resetUi();

    state.channel.bandwidth = BW_6_25;

    openChannels();
    pressKey(KEY_F1);                 /* new channel from the VFO */
    pressKey(KEY_F2);                 /* P2: save */

    /* Saved, not refused: the bank has the new channel */
    REQUIRE(chstore_count() == 2);
}

TEST_CASE("tuning a bank entry keeps the VFO it was on", "[ui][channels]")
{
    bootBank();
    resetUi();

    openChannels();
    pressKey(KEY_ENTER);

    REQUIRE(state.vfo_channel.rx_frequency == VFO_FREQ);

    /* And red on the memory screen does not lose the channel either */
    REQUIRE(state.ui_screen == MAIN_MEM);
    REQUIRE(state.channel.rx_frequency == CH_FREQ);
}

/*
 * Power-on has to come back on the channel the operator left the radio on, and
 * on its index.
 *
 * state_init() loads the bank, takes chstore_lastUsed() and fills state.channel
 * with that record, index included -- and then the inherited "initialise
 * remaining fields" block assigned 0 to state.channel_index, fifty lines later,
 * throwing the index away while keeping the channel.  The screen drew "CH 1"
 * next to the other channel's name, the voice prompt announced number 1, the
 * first knob turn loaded channel 2 (skipping 1) and chstore_setLastUsed()
 * stored an index the operator never had.
 */
TEST_CASE("power-on comes back on the stored channel and its index",
          "[ui][channels]")
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

    chstore_channel_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "FIRST");
    c.rx_freq = CH_FREQ;
    c.tx_freq = CH_FREQ;
    c.power   = 1000;
    c.mode    = OPMODE_FM;
    c.bandwidth = BW_12_5;
    REQUIRE(chstore_add(&c) == 0);

    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "SECOND");
    c.rx_freq = CH_FREQ + 12500;
    c.tx_freq = CH_FREQ + 12500;
    c.power   = 1000;
    c.mode    = OPMODE_FM;
    c.bandwidth = BW_12_5;
    REQUIRE(chstore_add(&c) == 1);

    /* The operator tuned channel 2 and switched the radio off there */
    chstore_setLastUsed(1);
    REQUIRE(chstore_commit() == 0);

    state_init();

    REQUIRE(state.channel_index == 1);
    REQUIRE(strcmp(state.channel.name, "SECOND") == 0);
    REQUIRE(state.channel.rx_frequency == CH_FREQ + 12500);
}

/*
 * A stored channel's own DMR call destination has to reach the transmitter
 * when the operator selects it.
 *
 * channel_t has no room for it, so it lives in the settings and only
 * chstore_applySettings() knows how to copy it.  The scanner called it; the
 * channel list did not, so a channel opened here transmitted to the talkgroup
 * of the channel before it, while the screen showed the new one.
 */
TEST_CASE("a stored DMR channel brings its call destination", "[ui][channels]")
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

    chstore_channel_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "LOCAL");
    c.rx_freq = CH_FREQ;
    c.tx_freq = CH_FREQ;
    c.power   = 1000;
    c.mode    = OPMODE_FM;
    c.bandwidth = BW_12_5;
    REQUIRE(chstore_add(&c) == 0);

    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "TG91");
    c.rx_freq = CH_FREQ + 25000;
    c.tx_freq = CH_FREQ + 25000;
    c.power   = 1000;
    c.mode    = OPMODE_DMR;
    c.bandwidth = BW_12_5;
    c.dmr_cc  = 1;
    c.dmr_dest_type = DMR_DEST_GROUP;
    c.dmr_dest_id   = 91;
    REQUIRE(chstore_add(&c) == 1);

    resetUi();

    /* The default destination is the settings' own, not this channel's */
    REQUIRE(state.settings.dmr_dest_id != 91);

    openChannels();
    pressKey(KEY_DOWN);
    pressKey(KEY_ENTER);

    REQUIRE(strcmp(state.channel.name, "TG91") == 0);
    REQUIRE(state.settings.dmr_dest_type == DMR_DEST_GROUP);
    REQUIRE(state.settings.dmr_dest_id == 91);
}
