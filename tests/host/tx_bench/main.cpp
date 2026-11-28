/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.cpp
 * @brief The host bench where a PTT that does nothing says which call failed.
 *
 * Five modes of the C62 -- D-STAR, dPMR, NXDN, YSF and P25 -- pressed the PTT
 * and put no carrier on the air, and nothing in the firmware could say why:
 * startTx() returns a bool, and by the time it reaches the RTX layer the call
 * that said no is gone.  The Linux build could not answer either, because it
 * does not compile those modes (meson.build, "the mode itself on the radio")
 * and because the emulator had no driver behind SINK_RTX, so every one of them
 * stopped at audioStream_start() with -ENODEV before anything the mode decides
 * was even reached.
 *
 * This bench closes both gaps.  It compiles the five modes and FM -- the mode
 * that does transmit and is the control -- against the host's own audio and
 * radio drivers, drives them through the firmware's entry points
 * (rtx_init/rtx_configure/rtx_task, the PTT line as the emulator models it) and
 * presses PTT.  Then it prints, per mode, what happened:
 *
 *   - the PA keyed and samples left the mode  -> "TX", and the peak;
 *   - a call refused                          -> the call, by name and code;
 *   - a precondition the mode checked itself   -> that condition.
 *
 * The table comes from rtx/TxTrace.h, which is the same record the radio keeps
 * (a handful of pointers): a mode that fails on the radio fills it there too.
 *
 * It also validates itself.  Two injections in the emulator's transmitter
 * (emulator/txbench.h) make a call fail on purpose -- the codec refusing to
 * start a playback stream, and the PA refusing to key -- and the bench asserts
 * that each one is *named*, on every mode.  A bench that cannot tell a working
 * transmission from a broken one is not a bench.
 *
 * Exit code is 0 only if every mode keys and both injections are reported.
 */

#define _POSIX_C_SOURCE 200809L

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <vector>

extern "C" {
#include "core/utils.h"
#include "emulator/emulator.h"
#include "emulator/txbench.h"
#include "interfaces/delays.h"
#include "interfaces/radio.h"
#include "rtx/TxTrace.h"
}

#include "rtx/rtx.h"

/* Counters of the emulator's transmitter (platform/drivers/baseband/
 * radio_linux.cpp), through the same handles tests/unit/mode_switch.cpp uses */
unsigned radio_linux_keyCount(void);
unsigned radio_linux_refusedCount(void);
void radio_linux_clearCounters(void);

namespace
{

pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;

/** What one PTT did. */
struct Result
{
    std::string mode;
    bool        entered;    ///< The mode came up in receive.
    bool        tx;         ///< The mode reported TX.
    bool        keyed;      ///< The PA was keyed (the driver's own counter).
    unsigned    blocks;     ///< Halves the transmitter played.
    unsigned    samples;    ///< Samples they carried.
    int         peak;       ///< Loudest sample played.
    unsigned    micSamples; ///< Samples the microphone handed out.
    txTraceReport trace;
};

constexpr uint8_t kTxFrequency = 430'000'000u;
constexpr uint32_t kEntryTimeoutMs = 2000;
/* P25 and YSF only look at the PTT at the end of their receive pass, and P25
 * drops 500 ms of samples on entry before it gets there: a window shorter than
 * that would report "startTx() was never reached" for a mode that is fine. */
constexpr uint32_t kKeyTimeoutMs = 2000;
/* Passes of transmit to let the mode render, so the samples it puts out can be
 * counted: reaching TX is the start of the over, not the over. */
constexpr uint32_t kRenderMs = 120;
constexpr uint32_t kReleaseTimeoutMs = 1000;

void sleepMs(unsigned ms)
{
    sleepFor(0, ms);
}

/** The radio's status as the UI would read it. */
rtxStatus_t readStatus()
{
    return rtx_getCurrentStatus();
}

/** Run the RTX thread's pass until `done` says so, or the deadline passes. */
template <typename Done>
bool spinUntil(uint32_t timeoutMs, Done done)
{
    const uint32_t start = (uint32_t)getTick();

    for (;;)
    {
        rtx_task();
        if (done())
            return true;
        if (((uint32_t)getTick() - start) >= timeoutMs)
            return false;
        sleepMs(5);
    }
}

const char *modeName(uint8_t mode);

/**
 * Enter a mode and leave it receiving.
 *
 * The configuration carries what a channel would: a frequency in the PA's band,
 * a power the gate will key with, and a source and destination address, because
 * NXDN and dPMR refuse to transmit without one (dmrSourceId is 0 after
 * rtx_init(), which is a real refusal and would mask everything below it).
 */
bool enterMode(uint8_t mode)
{
    rtxStatus_t cfg = readStatus();

    cfg.opMode        = static_cast<opmode>(mode);
    cfg.rxFrequency   = kTxFrequency;
    cfg.txFrequency   = kTxFrequency;
    cfg.txPower       = 500;
    cfg.txDisable     = 0;
    cfg.bandwidth     = BW_12_5;
    cfg.dmrSourceId   = 1234567;
    cfg.dmrDestId     = 9;
    cfg.dmrDestType   = DMR_DEST_GROUP;
    cfg.dmrColourCode = 1;
    cfg.dmrTimeslot   = 1;
    snprintf(cfg.source_address, sizeof(cfg.source_address), "N0CALL");
    snprintf(cfg.destination_address, sizeof(cfg.destination_address), "CQCQCQ");

    rtx_configure(&cfg);

    const bool ok = spinUntil(kEntryTimeoutMs, [mode]() {
        const rtxStatus_t s = readStatus();
        return (s.opMode == mode) && (s.opStatus == RX);
    });
    if (!ok)
    {
        const rtxStatus_t s = readStatus();
        printf("  (mode %s did not come up: published mode=%u status=%u "
               "driver=%d)\n", modeName(mode), (unsigned)s.opMode,
               (unsigned)s.opStatus, (int)radio_getStatus());
    }

    return ok;
}

/**
 * Press the PTT and hold it until the mode keys or the deadline passes.
 *
 * The gate wants a release seen since the mode change and a press of
 * KEYGATE_HOLD_MS, so the line is sampled released first: that is exactly what
 * the operator does, and it is not something the bench may skip -- a press the
 * gate never saw start is a press that never keys, which would be a false
 * negative for every mode.
 */
void pressPtt()
{
    emulator_state.PTTstatus = false;
    sleepMs(20);
    rtx_task();

    emulator_state.PTTstatus = true;
    sleepMs(40);
}

void releasePtt()
{
    emulator_state.PTTstatus = false;
    spinUntil(kReleaseTimeoutMs, []() {
        return readStatus().opStatus != TX;
    });
    sleepMs(20);
}

/** One mode: enter, press, read the trace, release. */
Result runMode(uint8_t mode, const char *name)
{
    Result r;
    r.mode    = name;
    r.entered = false;
    r.tx      = false;
    r.keyed   = false;
    r.blocks  = 0;
    r.samples = 0;
    r.peak    = 0;
    r.micSamples = 0;
    memset(&r.trace, 0, sizeof(r.trace));

    txbench_clear();
    radio_linux_clearCounters();

    r.entered = enterMode(mode);
    if (!r.entered)
        return r;

    pressPtt();

    const bool keyed = spinUntil(kKeyTimeoutMs, []() {
        return readStatus().opStatus == TX;
    });

    /* Let the over run for a few passes: a mode that keyed and rendered
     * nothing is the other way of transmitting nothing. */
    if (keyed)
        spinUntil(kRenderMs, []() { return readStatus().opStatus != TX; });

    r.tx      = keyed;
    r.keyed   = (radio_linux_keyCount() != 0);
    r.blocks  = txbench_blocks();
    r.samples = txbench_samples();
    r.peak    = txbench_peak();
    r.micSamples = txbench_micSamples();

    txTrace_report(&r.trace);

    releasePtt();

    return r;
}

const char *modeName(uint8_t mode)
{
    switch (mode) {
        case OPMODE_FM: return "FM";
        case OPMODE_NXDN: return "NXDN";
        case OPMODE_DPMR: return "dPMR";
        case OPMODE_DSTAR: return "D-STAR";
        case OPMODE_YSF: return "YSF";
        case OPMODE_P25: return "P25";
        default: return "?";
    }
}

std::string describe(const Result &r)
{
    if (!r.entered)
        return "did not enter receive";

    if (r.tx && r.keyed)
        return "TX";

    std::string why;

    if (r.trace.failedCall != nullptr)
    {
        why = std::string(r.trace.failedCall) + " = " +
              std::to_string(r.trace.failedRc);
    }
    else if (r.trace.blockedBy != nullptr)
    {
        why = r.trace.blockedBy;
    }
    else
    {
        why = "no trace: startTx() was never reached";
    }

    return why;
}

/**
 * The bench's own control: with a call broken on purpose, the mode has to name
 * it.  Two injections, because they are at the two ends of the chain -- the
 * codec that will not start a stream, and the PA that will not come up -- and a
 * trace that only ever reports the first would look right and be useless.
 */
bool mutationNamesTheCall(uint8_t mode, const char *name, const char *expected,
                          const char *streamCall, int streamRc, bool refuseKey)
{
    bool ok;

    txbench_clear();
    radio_linux_clearCounters();

    if (!enterMode(mode))
    {
        printf("  %-8s MUTATION: the mode did not enter receive\n", name);
        return false;
    }

    if (streamCall != nullptr)
        txbench_failStreamStart(streamRc);
    txbench_refuseKeying(refuseKey);

    pressPtt();
    spinUntil(kKeyTimeoutMs, []() { return readStatus().opStatus == TX; });

    txTraceReport trace;
    txTrace_report(&trace);

    releasePtt();

    const char *got = (trace.failedCall != nullptr) ? trace.failedCall
                                                    : trace.blockedBy;

    ok = (got != nullptr) && (strstr(got, expected) != nullptr);

    printf("  %-8s %-46s %s\n", name, expected,
           ok ? "named" : (got ? got : "(nothing)"));

    txbench_failStreamStart(0);
    txbench_refuseKeying(false);

    return ok;
}

/**
 * The receiver's stream is dead and the PTT still has to work.
 *
 * This is the failure that reads as "the radio transmits nothing": the codec
 * route did not apply, the baseband stream never started, and a mode that looks
 * at the PTT at the *end* of its receive pass never looks at it at all.  Two of
 * the five (P25, YSF) were written that way; the fix moved the check in front
 * of the receiver, and this is what keeps it there.
 */
bool deadReceiverStillKeys(uint8_t mode, const char *name)
{
    txbench_clear();
    radio_linux_clearCounters();
    txbench_failReceiveStream(true);

    const bool entered = enterMode(mode);

    pressPtt();
    const bool tx = spinUntil(kKeyTimeoutMs, []() {
        return readStatus().opStatus == TX;
    });
    if (tx)
        spinUntil(kRenderMs, []() { return readStatus().opStatus != TX; });

    const bool keyed = (radio_linux_keyCount() != 0);

    releasePtt();
    txbench_failReceiveStream(false);

    const bool ok = entered && tx && keyed;

    printf("  %-8s %-46s %s\n", name, "PTT keys with the receiver dead",
           ok ? "yes" : (entered ? "no: the PTT never looked" : "no: no entry"));

    return ok;
}

} /* namespace */

int main()
{
    struct { uint8_t id; const char *name; } modes[] = {
        { OPMODE_FM,    "FM"    },   /* the control: it transmits, and on the
                                      * host it does so without a stream */
        { OPMODE_NXDN,  "NXDN"  },
        { OPMODE_DPMR,  "dPMR"  },
        { OPMODE_DSTAR, "D-STAR" },
        { OPMODE_YSF,   "YSF"   },
        { OPMODE_P25,   "P25"   },
    };

    /* The microphone is a driver of its own: its absence would show as an
     * -ENODEV three calls into a mode's startTx() and read like a mode bug. */
    rtx_init(&cfgMutex);
    emulator_state.PTTstatus = false;

    std::vector<Result> results;
    bool green = true;

    /* The microphone column is what tells the two families apart: NXDN, dPMR
     * and YSF encode the operator's voice, D-STAR and P25 send a null voice
     * source, and all of them key the same PA. */
    printf("\n"
           "mode     result                                         keyed  "
           "blocks  peak   mic\n"
           "----------------------------------------------------------------"
           "--------------------\n");

    for (const auto &m : modes)
    {
        const Result r = runMode(m.id, m.name);
        results.push_back(r);

        const std::string what = describe(r);

        printf("%-8s %-46s %-6s %-7u %-6d %u\n", m.name, what.c_str(),
               r.keyed ? "yes" : "no", r.blocks, r.peak, r.micSamples);

        /*
         * The pass condition, per mode: the PA keyed, the mode reported TX and
         * what it rendered reached the transmitter.  FM has no stream of its
         * own on the host -- the codec worker moves the microphone into the
         * transmitter and that worker is not here -- so for FM the keying is
         * the whole evidence.
         */
        const bool rendered = (m.id == OPMODE_FM) || (r.blocks > 0);
        const bool audible  = (m.id == OPMODE_FM) || (r.peak != 0);

        if (!(r.tx && r.keyed && rendered && audible))
            green = false;
    }

    /* The working modes must have put real samples out, not silence: a mode
     * that keys and renders zeros is the other way to transmit nothing. */
    printf("\nmutation: the same bench with a call broken on purpose\n");

    bool mutated = true;
    for (const auto &m : modes)
    {
        /* FM has no stream of its own, so only the PA injection applies to
         * it; every other mode has to name both. */
        if (m.id != OPMODE_FM)
        {
            mutated = mutationNamesTheCall(
                          m.id, m.name,
                          "audioStream_start(OUT",
                          "audioStream_start", -5 /* -EIO */, false) && mutated;
        }

        mutated = mutationNamesTheCall(
                      m.id, m.name,
                      "radio_enableTx(): the PA did not key",
                      nullptr, 0, true) && mutated;
    }

    printf("\nreceiver dead: the PTT may not depend on it\n");

    bool independent = true;
    for (const auto &m : modes)
        independent = deadReceiverStillKeys(m.id, m.name) && independent;

    /* Release the mode and let the radio go: nothing is left keyed. */
    rtxStatus_t cfg = readStatus();
    cfg.opMode = OPMODE_NONE;
    rtx_configure(&cfg);
    rtx_task();
    rtx_terminate();

    const bool pass = green && mutated && independent;

    printf("\n%s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}
