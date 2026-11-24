/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "core/scanner.h"
#include "core/settings.h"

/*
 * Scan and vigilance on the host, with a simulated receiver.
 *
 * The machine is pure (core/scanner.h), so a test can put a channel bank and a
 * squelch in front of it and run minutes of scanning in microseconds.  What is
 * asserted here is what a user notices:
 *
 *   - the walk visits every channel, in order, and comes back round;
 *   - a channel flagged SKIP is never visited, and the flag is a flag, not a
 *     deletion;
 *   - the scan stops on the squelch, stays while the carrier is there, and
 *     starts again after the configured delay, with all three resume rules
 *     (carrier, timeout, hold);
 *   - chatter on the squelch does not stop it;
 *   - the priority channel is looked at between steps, and never while a call
 *     is in progress;
 *   - PTT ends the scan and asks for the home channel from any state;
 *   - a VFO scan walks the range in the configured step, wraps or ends, and
 *     never leaves the machine in a state that does not move.
 *
 * The last test measures cost: how long a step takes on this host, and how
 * many channel reads a second of scanning needs (the firmware budget is two
 * reads per step, none while listening).
 */

namespace
{

constexpr uint32_t TICK = 5;      ///< The cadence of state_task(), ms
constexpr uint32_t START = 1000;  ///< Arbitrary non-zero origin for the clock

/* ------------------------------------------------------------ the channel bank */

struct FakeChannel
{
    uint32_t freq;
    uint8_t  mode = 1;   /* OPMODE_FM */
    bool     skip = false;
    bool     usable = true;
};

struct FakeBank
{
    std::vector<FakeChannel> channels;
    unsigned infoCalls = 0;

    static int count(void *ctx)
    {
        return static_cast<int>(static_cast<FakeBank *>(ctx)->channels.size());
    }

    static int info(void *ctx, uint16_t idx, scanEntry_t *out)
    {
        FakeBank *bank = static_cast<FakeBank *>(ctx);
        bank->infoCalls++;

        if (idx >= bank->channels.size())
            return -1;

        const FakeChannel &ch = bank->channels[idx];
        if (!ch.usable)
            return -1;

        out->frequency = ch.freq;
        out->mode      = ch.mode;
        out->skip      = ch.skip ? 1 : 0;
        return 0;
    }

    scanList_t list()
    {
        return scanList_t{ count, info, this };
    }
};

/* ---------------------------------------------------------------- the rig */

/// What the simulated receiver hears: a function of the target and the time.
using SignalModel = std::function<bool(uint16_t channel, uint32_t frequency,
                                       uint32_t now)>;

/**
 * The machine, a bank, a receiver that reports activity where the model says,
 * and the record of what happened.
 *
 * Applying an action is exactly what scan_service.c does: a channel target is
 * a channel of the bank, a frequency target leaves the channel, and HOME is
 * the target the caller had before starting.
 */
struct Rig
{
    scanner_t machine;
    FakeBank bank;
    scanList_t list;
    uint32_t now = START;
    uint16_t channel = SCAN_CHANNEL_NONE;
    uint32_t frequency = 0;
    uint16_t homeChannel = SCAN_CHANNEL_NONE;
    uint32_t homeFrequency = 0;
    SignalModel signal;
    bool ptt = false;

    std::vector<uint8_t> events;
    std::vector<uint16_t> channelTunes;   ///< Channel targets, in order
    std::vector<uint32_t> freqTunes;      ///< Frequency targets, in order
    unsigned tunes = 0;
    unsigned homeRequests = 0;

    Rig()
    {
        scan_init(&machine);
        list = scanList_t{ FakeBank::count, FakeBank::info, &bank };
        scan_setList(&machine, &list);
    }

    void configure(const scanConfig_t &cfg)
    {
        REQUIRE(scan_configure(&machine, &cfg));
    }

    /// A configuration that walks quickly: 40 ms settle, 100 ms dwell.
    scanConfig_t fastConfig(uint8_t mode = SCAN_MODE_MEMORY)
    {
        scanConfig_t cfg;
        scan_defaultConfig(&cfg);
        cfg.mode        = mode;
        cfg.settleMs    = 40;
        cfg.dwellMs     = 100;
        cfg.hangMs      = 200;
        cfg.holdMs      = 1000;
        cfg.debounceMs  = 20;
        cfg.priorityIntervalMs = 300;
        return cfg;
    }

    void apply(const scanAction_t &a)
    {
        if (a.event != SCAN_EVENT_NONE)
            events.push_back(a.event);

        switch (a.target)
        {
            case SCAN_TARGET_CHANNEL:
                channel   = a.channel;
                frequency = (a.channel < bank.channels.size())
                          ? bank.channels[a.channel].freq : 0;
                channelTunes.push_back(a.channel);
                tunes++;
                break;

            case SCAN_TARGET_FREQUENCY:
                channel   = SCAN_CHANNEL_NONE;
                frequency = a.frequency;
                freqTunes.push_back(a.frequency);
                tunes++;
                break;

            case SCAN_TARGET_HOME:
                channel   = homeChannel;
                frequency = homeFrequency;
                homeRequests++;
                break;

            default:
                break;
        }
    }

    void setHome(uint16_t ch, uint32_t f)
    {
        homeChannel   = ch;
        homeFrequency = f;
        channel       = ch;
        frequency     = f;
    }

    bool active() const
    {
        return signal ? signal(channel, frequency, now) : false;
    }

    void start(uint16_t ch = SCAN_CHANNEL_NONE, uint32_t f = 0)
    {
        scanAction_t a;
        REQUIRE(scan_start(&machine, ch, f, now, &a));
        apply(a);
    }

    void startVfo(uint32_t f)
    {
        start(SCAN_CHANNEL_NONE, f);
    }

    void tick(uint32_t ms = TICK)
    {
        now += ms;
        scanAction_t a;
        scan_step(&machine, active(), ptt, now, &a);
        apply(a);
    }

    void run(uint32_t ms)
    {
        uint32_t end = now + ms;
        while (now < end)
            tick();
    }

    bool sawEvent(uint8_t ev) const
    {
        for (uint8_t e : events)
            if (e == ev)
                return true;
        return false;
    }

    unsigned countEvent(uint8_t ev) const
    {
        unsigned n = 0;
        for (uint8_t e : events)
            if (e == ev)
                n++;
        return n;
    }
};

/* ------------------------------------------------------------------- helpers */

std::vector<FakeChannel> evenlySpaced(int n, uint32_t base = 145000000,
                                      uint32_t step = 25000)
{
    std::vector<FakeChannel> out;
    for (int i = 0; i < n; i++)
    {
        FakeChannel ch;
        ch.freq = base + (uint32_t)i * step;
        out.push_back(ch);
    }
    return out;
}

/// First `n` channel tunes, for order assertions.
std::vector<uint16_t> firstTunes(const Rig &rig, size_t n)
{
    std::vector<uint16_t> out(rig.channelTunes.begin(),
                              rig.channelTunes.begin() +
                              std::min(n, rig.channelTunes.size()));
    return out;
}

} // namespace

/* ======================================================== memory scan ===== */

TEST_CASE("A memory scan visits every channel, in order, and laps")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(4);
    rig.configure(rig.fastConfig());
    rig.setHome(SCAN_CHANNEL_NONE, 145000000);

    rig.start(SCAN_CHANNEL_NONE, 145000000);

    /* 4 channels, 140 ms each: one lap in ~560 ms */
    rig.run(1400);

    REQUIRE(rig.channelTunes.size() >= 9);

    /* 0,1,2,3,0,1,2,3,0: strict order, no repetition inside a lap */
    const uint16_t expected[] = { 0, 1, 2, 3, 0, 1, 2, 3, 0 };
    std::vector<uint16_t> got = firstTunes(rig, 9);
    for (size_t i = 0; i < got.size(); i++)
        REQUIRE(got[i] == expected[i]);

    /* The lap is reported once each time it wraps */
    REQUIRE(rig.countEvent(SCAN_EVENT_RANGE_END) >= 2);
    REQUIRE(rig.sawEvent(SCAN_EVENT_STARTED));
    REQUIRE(scan_state(&rig.machine) != SCAN_STATE_IDLE);
    REQUIRE(scan_channel(&rig.machine) != SCAN_CHANNEL_NONE);
}

TEST_CASE("A channel flagged SKIP is passed over, not deleted")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(5);
    rig.bank.channels[1].skip = true;
    rig.bank.channels[3].skip = true;
    rig.configure(rig.fastConfig());
    rig.setHome(SCAN_CHANNEL_NONE, 0);

    rig.start(SCAN_CHANNEL_NONE, 0);
    rig.run(1400);

    for (uint16_t ch : rig.channelTunes)
    {
        REQUIRE(ch != 1);
        REQUIRE(ch != 3);
    }

    /* Still 0, 2, 4 and round again */
    REQUIRE(rig.channelTunes.size() >= 6);
    REQUIRE(firstTunes(rig, 6) == std::vector<uint16_t>({ 0, 2, 4, 0, 2, 4 }));
}

TEST_CASE("An index the bank refuses is walked past like a skip")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(4);
    rig.bank.channels[2].usable = false;
    rig.configure(rig.fastConfig());

    rig.start(SCAN_CHANNEL_NONE, 0);
    rig.run(1200);

    for (uint16_t ch : rig.channelTunes)
        REQUIRE(ch != 2);
    REQUIRE(rig.channelTunes.size() >= 4);
}

TEST_CASE("A bank with nothing to walk ends instead of spinning")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(3);
    for (auto &ch : rig.bank.channels)
        ch.skip = true;
    rig.configure(rig.fastConfig());

    rig.start(SCAN_CHANNEL_NONE, 0);

    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_END);
    REQUIRE(rig.sawEvent(SCAN_EVENT_RANGE_END));
    REQUIRE(rig.tunes == 0);

    /* It sits there, and the operator can still stop it cleanly */
    rig.run(500);
    REQUIRE(rig.tunes == 0);

    scanAction_t a;
    scan_stop(&rig.machine, rig.now, &a);
    rig.apply(a);
    REQUIRE(a.target == SCAN_TARGET_HOME);
    REQUIRE(rig.homeRequests == 1);
    REQUIRE_FALSE(scan_isRunning(&rig.machine));
}

/* ================================================== stopping on signal ===== */

TEST_CASE("A signal stops the scan, and it carries on after the hang")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(4);
    rig.configure(rig.fastConfig());
    rig.setHome(SCAN_CHANNEL_NONE, 0);

    /* Channel 1 carries from t=1200 to t=2400 */
    rig.signal = [](uint16_t ch, uint32_t, uint32_t now) {
        return ch == 1 && now >= 1200 && now < 2400;
    };

    rig.start(SCAN_CHANNEL_NONE, 0);
    rig.run(700);

    /* It reached channel 1 and stopped there */
    REQUIRE(rig.channel == 1);
    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);
    REQUIRE(rig.sawEvent(SCAN_EVENT_SIGNAL));
    REQUIRE(scan_stops(&rig.machine) == 1);
    REQUIRE(rig.countEvent(SCAN_EVENT_RESUME) == 0);

    /* While the carrier is there nothing else is tuned: no hunting */
    unsigned tunesAtStop = rig.tunes;
    size_t indexAtStop = rig.channelTunes.size();
    rig.run(600);                       /* still inside the carrier window */
    REQUIRE(rig.tunes == tunesAtStop);
    REQUIRE(rig.channel == 1);
    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);

    /* Carrier gone at 2400: 200 ms of hang, then the next channel */
    rig.run(600);
    REQUIRE(rig.countEvent(SCAN_EVENT_RESUME) >= 1);
    REQUIRE(rig.channel != 1);
    REQUIRE(rig.channelTunes.size() > indexAtStop);
    REQUIRE(rig.channelTunes[indexAtStop] == 2);
}

TEST_CASE("Squelch chatter shorter than the debounce does not stop the scan")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(3);
    scanConfig_t cfg = rig.fastConfig();
    cfg.debounceMs = 30;
    rig.configure(cfg);

    /* 10 ms spikes every 60 ms: shorter than the 30 ms debounce */
    rig.signal = [](uint16_t, uint32_t, uint32_t now) {
        return (now % 60) < 10;
    };

    rig.start(SCAN_CHANNEL_NONE, 0);
    rig.run(900);

    REQUIRE(rig.countEvent(SCAN_EVENT_SIGNAL) == 0);
    REQUIRE(scan_state(&rig.machine) != SCAN_STATE_SIGNAL);
    REQUIRE(rig.tunes > 3);
}

TEST_CASE("The carrier rule resumes when the squelch closes, the timeout rule "
          "abandons a long call")
{
    SECTION("carrier: stays as long as the signal is there")
    {
        Rig rig;
        rig.bank.channels = evenlySpaced(3);
        scanConfig_t cfg = rig.fastConfig();
        cfg.resume = SCAN_RESUME_CARRIER;
        rig.configure(cfg);
        rig.signal = [](uint16_t ch, uint32_t, uint32_t) { return ch == 0; };

        rig.start(SCAN_CHANNEL_NONE, 0);
        rig.run(3000);

        /* A permanent carrier on channel 0: the scan stops there for ever */
        REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);
        REQUIRE(rig.channel == 0);
        REQUIRE(rig.countEvent(SCAN_EVENT_RESUME) == 0);
    }

    SECTION("timeout: leaves after holdMs even if the call goes on")
    {
        Rig rig;
        rig.bank.channels = evenlySpaced(3);
        scanConfig_t cfg = rig.fastConfig();
        cfg.resume = SCAN_RESUME_TIMEOUT;
        cfg.holdMs = 500;
        rig.configure(cfg);
        rig.signal = [](uint16_t ch, uint32_t, uint32_t) { return ch == 0; };

        rig.start(SCAN_CHANNEL_NONE, 0);
        rig.run(800);

        /* Stopped on channel 0 at about 140 ms, left at about 640 ms */
        REQUIRE(rig.sawEvent(SCAN_EVENT_SIGNAL));
        REQUIRE(rig.sawEvent(SCAN_EVENT_RESUME));

        /* and it stops again when the walk returns to channel 0 */
        rig.run(1200);
        REQUIRE(rig.countEvent(SCAN_EVENT_SIGNAL) >= 2);
    }
}

TEST_CASE("The hold rule waits for the operator")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(3);
    scanConfig_t cfg = rig.fastConfig();
    cfg.resume = SCAN_RESUME_HOLD;
    rig.configure(cfg);
    rig.signal = [](uint16_t ch, uint32_t, uint32_t) { return ch == 0; };

    rig.start(SCAN_CHANNEL_NONE, 0);
    rig.run(5000);

    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);
    REQUIRE(rig.countEvent(SCAN_EVENT_RESUME) == 0);
    REQUIRE(rig.tunes == 1);

    /* The key that resumes goes on to the next channel even with the carrier */
    scanAction_t a;
    REQUIRE(scan_resume(&rig.machine, rig.now, &a));
    rig.apply(a);
    REQUIRE(rig.channel == 1);
    REQUIRE(scan_state(&rig.machine) != SCAN_STATE_SIGNAL);
}

/* ======================================================== priority ========= */

TEST_CASE("The priority channel is checked between steps")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(4);
    scanConfig_t cfg = rig.fastConfig();
    cfg.priorityEnabled    = 1;
    cfg.priority           = 2;
    cfg.priorityIntervalMs = 0;    /* every step, so the test is deterministic */
    rig.configure(cfg);
    rig.setHome(SCAN_CHANNEL_NONE, 0);

    rig.start(SCAN_CHANNEL_NONE, 0);
    rig.run(700);

    /* The priority channel appears between the walk's channels, and the walk
     * does not lose its place: after each check it goes where it was going. */
    REQUIRE(rig.channelTunes.size() >= 6);
    REQUIRE(rig.channelTunes[1] == 2);   /* 0 (start), 2 (priority), 1 ... */
    REQUIRE(rig.channelTunes[2] == 1);
    REQUIRE(rig.channelTunes[3] == 2);
    REQUIRE(rig.channelTunes[4] == 2);   /* pending 2 == priority: no extra check */
    REQUIRE(scan_state(&rig.machine) != SCAN_STATE_IDLE);
}

TEST_CASE("A call on the priority channel stops the scan, and resuming goes "
          "back to where the walk was")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(4);
    scanConfig_t cfg = rig.fastConfig();
    cfg.priorityEnabled    = 1;
    cfg.priority           = 2;
    cfg.priorityIntervalMs = 0;
    rig.configure(cfg);

    rig.signal = [](uint16_t ch, uint32_t, uint32_t) { return ch == 2; };

    rig.start(SCAN_CHANNEL_NONE, 0);
    rig.run(400);

    REQUIRE(rig.sawEvent(SCAN_EVENT_PRIORITY));
    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);
    REQUIRE(rig.channel == 2);

    scanAction_t a;
    REQUIRE(scan_resume(&rig.machine, rig.now, &a));
    rig.apply(a);

    /* Back on the channel the walk was going to, not on the priority one */
    REQUIRE(rig.channel == 1);
}

TEST_CASE("The priority check never interrupts a call in progress")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(4);
    scanConfig_t cfg = rig.fastConfig();
    cfg.priorityEnabled    = 1;
    cfg.priority           = 0;
    cfg.priorityIntervalMs = 50;   /* would fire 60 times in three seconds */
    rig.configure(cfg);

    /* A call on channel 1, which is where the scan stops */
    rig.signal = [](uint16_t ch, uint32_t, uint32_t) { return ch == 1; };

    rig.start(SCAN_CHANNEL_NONE, 0);

    /* Walk until it stops on the call */
    uint32_t guard = 0;
    while ((scan_state(&rig.machine) != SCAN_STATE_SIGNAL) && (guard < 3000))
    {
        rig.tick();
        guard += TICK;
    }
    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);
    REQUIRE(rig.channel == 1);

    /* Three seconds into the call, nothing has been retuned: the priority
     * channel was never checked while the call was up. */
    unsigned tunesAtStop = rig.tunes;
    rig.run(3000);

    REQUIRE(rig.channel == 1);
    REQUIRE(rig.tunes == tunesAtStop);
    REQUIRE(rig.countEvent(SCAN_EVENT_PRIORITY) == 0);
    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);
}

/* ============================================================ PTT ========= */

TEST_CASE("PTT ends the scan from any state and asks for the home channel")
{
    const uint8_t states[] = { SCAN_STATE_TUNE, SCAN_STATE_LISTEN,
                               SCAN_STATE_SIGNAL, SCAN_STATE_HANG,
                               SCAN_STATE_PRIORITY };

    for (uint8_t state : states)
    {
        DYNAMIC_SECTION("from state " << scan_stateName(state))
        {
            Rig rig;
            rig.bank.channels = evenlySpaced(4);
            scanConfig_t cfg = rig.fastConfig();
            cfg.priorityEnabled    = 1;
            cfg.priority           = 3;
            cfg.priorityIntervalMs = 0;
            cfg.hangMs             = 5000;   /* so HANG lasts long enough */
            rig.configure(cfg);
            rig.setHome(SCAN_CHANNEL_NONE, 145000000);

            /*
             * A carrier on channel 1 that goes away shortly after the start:
             * with the priority check diverting to channel 3 and coming back,
             * that reaches every state of the table within half a second.
             */
            uint32_t carrierUntil = rig.now + 400;
            rig.signal = [carrierUntil](uint16_t ch, uint32_t, uint32_t now) {
                return (ch == 1) && (now < carrierUntil);
            };

            rig.start(SCAN_CHANNEL_NONE, 0);

            uint32_t guard = 0;
            while ((scan_state(&rig.machine) != state) && (guard < 3000))
            {
                rig.tick();
                guard += TICK;
            }
            REQUIRE(scan_state(&rig.machine) == state);

            rig.ptt = true;
            rig.tick();

            REQUIRE(rig.sawEvent(SCAN_EVENT_STOPPED));
            REQUIRE_FALSE(scan_isRunning(&rig.machine));
            REQUIRE(scan_state(&rig.machine) == SCAN_STATE_IDLE);
            REQUIRE(rig.homeRequests == 1);

            /* Keyed or not, the machine is idle and stays quiet */
            unsigned tunes = rig.tunes;
            rig.run(200);
            REQUIRE(rig.tunes == tunes);

            /* Releasing PTT: the scan can be started again */
            rig.ptt = false;
            rig.tick();
            REQUIRE(scan_state(&rig.machine) == SCAN_STATE_IDLE);
        }
    }
}

/* ============================================================ VFO ========= */

TEST_CASE("A VFO scan walks the range in the configured step and wraps")
{
    Rig rig;
    scanConfig_t cfg = rig.fastConfig(SCAN_MODE_VFO);
    cfg.vfoLow  = 145000000;
    cfg.vfoHigh = 145050000;
    cfg.vfoStep = 12500;
    cfg.loop    = 1;
    rig.configure(cfg);

    rig.startVfo(145000000);
    rig.run(1200);

    REQUIRE(rig.freqTunes.size() >= 5);
    REQUIRE(rig.freqTunes[0] == 145012500);
    REQUIRE(rig.freqTunes[1] == 145025000);
    REQUIRE(rig.freqTunes[2] == 145037500);
    REQUIRE(rig.freqTunes[3] == 145050000);
    REQUIRE(rig.freqTunes[4] == 145000000);   /* wrapped */
    REQUIRE(rig.sawEvent(SCAN_EVENT_RANGE_END));
}

TEST_CASE("A VFO scan without loop ends at the limit instead of wrapping")
{
    Rig rig;
    scanConfig_t cfg = rig.fastConfig(SCAN_MODE_VFO);
    cfg.vfoLow  = 145000000;
    cfg.vfoHigh = 145025000;
    cfg.vfoStep = 12500;
    cfg.loop    = 0;
    rig.configure(cfg);

    rig.startVfo(145000000);
    rig.run(900);

    REQUIRE(rig.freqTunes.size() == 2);
    REQUIRE(rig.freqTunes[1] == 145025000);
    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_END);
    REQUIRE(rig.sawEvent(SCAN_EVENT_RANGE_END));

    /* It stays on the last frequency, and it is still stoppable */
    uint32_t last = rig.frequency;
    rig.run(500);
    REQUIRE(rig.frequency == last);
    REQUIRE(rig.freqTunes.size() == 2);

    scanAction_t a;
    scan_stop(&rig.machine, rig.now, &a);
    rig.apply(a);
    REQUIRE(a.target == SCAN_TARGET_HOME);
}

TEST_CASE("A VFO scan goes down and wraps at the bottom")
{
    Rig rig;
    scanConfig_t cfg = rig.fastConfig(SCAN_MODE_VFO);
    cfg.vfoLow     = 145000000;
    cfg.vfoHigh    = 145050000;
    cfg.vfoStep    = 25000;
    cfg.direction  = SCAN_DOWN;
    cfg.loop       = 1;
    rig.configure(cfg);

    rig.startVfo(145025000);
    rig.run(700);

    REQUIRE(rig.freqTunes.size() >= 4);
    REQUIRE(rig.freqTunes[0] == 145000000);
    REQUIRE(rig.freqTunes[1] == 145050000);   /* wrapped from the bottom */
    REQUIRE(rig.freqTunes[2] == 145025000);
}

TEST_CASE("A VFO parked outside the range enters it from the edge")
{
    Rig rig;
    scanConfig_t cfg = rig.fastConfig(SCAN_MODE_VFO);
    cfg.vfoLow  = 145000000;
    cfg.vfoHigh = 145025000;
    cfg.vfoStep = 12500;
    rig.configure(cfg);

    rig.startVfo(430000000);   /* nowhere near: the 70 cm band */
    rig.run(200);

    REQUIRE(rig.freqTunes.size() >= 1);
    REQUIRE(rig.freqTunes[0] == 145000000);

    /* Down from below the range starts at the top edge */
    Rig down;
    cfg.direction = SCAN_DOWN;
    down.configure(cfg);
    down.startVfo(100000000);
    down.run(200);

    REQUIRE(down.freqTunes.size() >= 1);
    REQUIRE(down.freqTunes[0] == 145025000);
}

TEST_CASE("Every step of the table walks its own grid")
{
    /* The table is the settings', and the scanner takes the step from there:
     * check that each entry produces exactly that spacing on the air. */
    const uint32_t expected[SETTINGS_SCAN_STEPS] = {
        5000, 6250, 8330, 10000, 12500, 25000
    };

    for (uint8_t i = 0; i < SETTINGS_SCAN_STEPS; i++)
    {
        REQUIRE(scan_steps[i] == expected[i]);

        Rig rig;
        scanConfig_t cfg = rig.fastConfig(SCAN_MODE_VFO);
        cfg.vfoLow  = 145000000;
        cfg.vfoHigh = 145100000;
        cfg.vfoStep = scan_steps[i];
        rig.configure(cfg);

        rig.startVfo(145000000);
        rig.run(400);

        REQUIRE(rig.freqTunes.size() >= 2);
        REQUIRE(rig.freqTunes[0] == 145000000U + scan_steps[i]);
        REQUIRE(rig.freqTunes[1] == 145000000U + 2U * scan_steps[i]);

        /* And the name the menu shows is the one the operator expects */
        REQUIRE(settings_scanStepName(i)[0] != '\0');
        REQUIRE(settings_scanStepName(i)[0] != '-');
    }
}

/* ==================================================== invariants ========== */

TEST_CASE("No state is a trap: a carrier that goes away is always followed by a "
          "step")
{
    /* On/off patterns whose silence is longer than the hang, so the squelch
     * really closes and the scan must resume */
    struct Pattern { uint32_t on; uint32_t off; };
    const Pattern patterns[] = { { 200, 500 }, { 100, 300 }, { 700, 250 },
                                 { 40, 400 },  { 250, 120 } };

    for (const Pattern &p : patterns)
    {
        DYNAMIC_SECTION("carrier " << p.on << " ms on, " << p.off << " off")
        {
            Rig rig;
            rig.bank.channels = evenlySpaced(5);
            scanConfig_t cfg = rig.fastConfig();
            cfg.resume = SCAN_RESUME_CARRIER;
            cfg.hangMs = 100;
            rig.configure(cfg);

            uint32_t period = p.on + p.off;
            rig.signal = [p, period](uint16_t, uint32_t, uint32_t now) {
                return (now % period) < p.on;
            };

            rig.start(SCAN_CHANNEL_NONE, 0);

            /*
             * After the carrier closes: debounce, hang, settle and dwell before
             * the step.  Anything above that is a state the machine cannot
             * leave, which is what this test is for.
             */
            uint32_t budget = p.on + cfg.debounceMs + cfg.hangMs +
                              cfg.settleMs + cfg.dwellMs + 3 * TICK;
            unsigned startTunes = rig.tunes;
            uint32_t ticksWithoutTune = 0;

            for (int i = 0; i < 4000; i++)
            {
                unsigned before = rig.tunes;
                rig.tick();
                if (rig.tunes == before)
                    ticksWithoutTune += TICK;
                else
                    ticksWithoutTune = 0;

                REQUIRE(ticksWithoutTune <= budget);
            }

            REQUIRE(rig.tunes > startTunes);
        }
    }
}

TEST_CASE("Gaps shorter than the hang keep the scan parked: that is what the "
          "hang is for")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(4);
    scanConfig_t cfg = rig.fastConfig();
    cfg.resume = SCAN_RESUME_CARRIER;
    cfg.hangMs = 300;
    cfg.debounceMs = 20;
    rig.configure(cfg);

    /* 200 ms on, 50 ms off: the silence never outlives the hang */
    rig.signal = [](uint16_t, uint32_t, uint32_t now) {
        return (now % 250) < 200;
    };

    rig.start(SCAN_CHANNEL_NONE, 0);

    uint32_t guard = 0;
    while ((scan_state(&rig.machine) != SCAN_STATE_SIGNAL) && (guard < 3000))
    {
        rig.tick();
        guard += TICK;
    }
    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);

    unsigned tunesAtStop = rig.tunes;
    rig.run(3000);

    /* Still there: a conversation with short gaps is not chopped up */
    REQUIRE(rig.tunes == tunesAtStop);
    REQUIRE(scan_state(&rig.machine) == SCAN_STATE_SIGNAL);
    REQUIRE(rig.countEvent(SCAN_EVENT_RESUME) == 0);
}

TEST_CASE("A stopped machine costs nothing and a scan costs two bank reads a step")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(40);
    scanConfig_t cfg = rig.fastConfig();
    cfg.dwellMs = 150;
    rig.configure(cfg);

    /* While idle: no bank reads at all */
    rig.bank.infoCalls = 0;
    rig.run(500);
    REQUIRE(rig.bank.infoCalls == 0);

    /* While scanning: a step reads the bank once per candidate, never more
     * than one lap of it, and nothing at all while listening */
    rig.bank.infoCalls = 0;
    rig.start(SCAN_CHANNEL_NONE, 0);
    uint32_t infoAfterStart = rig.bank.infoCalls;
    REQUIRE(infoAfterStart <= 1);          /* the start reads one channel */

    rig.bank.infoCalls = 0;
    unsigned tunes = rig.tunes;
    rig.run(1000);                          /* ~5 steps of 190 ms */

    unsigned steps = rig.tunes - tunes;
    REQUIRE(steps >= 3);
    INFO("bank reads in one second: " << rig.bank.infoCalls
         << " over " << steps << " steps");
    REQUIRE(rig.bank.infoCalls <= steps + 1);   /* one read per step, no more */
}

TEST_CASE("The cost of a sample on this host")
{
    Rig rig;
    rig.bank.channels = evenlySpaced(128);   /* a full bank, worst case */
    rig.configure(rig.fastConfig());

    /* Listening is the overwhelmingly common case: measure that */
    rig.start(SCAN_CHANNEL_NONE, 0);

    constexpr unsigned SAMPLES = 200000;
    auto begin = std::chrono::steady_clock::now();

    for (unsigned i = 0; i < SAMPLES; i++)
    {
        rig.tick();          /* no signal: LISTEN, the dwell timer only */
    }

    auto end = std::chrono::steady_clock::now();
    double nsPerCall = std::chrono::duration<double, std::nano>(end - begin)
                       .count() / SAMPLES;

    std::printf("\nscanner: %.1f ns per 5 ms sample (host), "
                "%u tunes, %u stops\n", nsPerCall, rig.tunes,
                scan_stops(&rig.machine));

    /*
     * The firmware calls this 200 times a second, so what matters is that one
     * call is far below one millisecond: the whole point of the state machine
     * is that scanning costs nothing while listening.  The bound is loose on
     * purpose (it is a host, and a CI machine may be loaded).
     */
    REQUIRE(nsPerCall < 20000.0);
    REQUIRE(rig.tunes > 0);
}
