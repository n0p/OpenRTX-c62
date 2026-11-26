/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file sim_control.c
 * @brief Scriptable control channel of the C62 simulator. See sim_control.h.
 *
 * Three threads meet here: the OpenRTX worker (state and RTX), the UI thread
 * (ui_threadFunc, which consumes keys and draws) and the SDL thread (which owns
 * the window and the framebuffer). This file is a fourth, and it never touches
 * anything the other three own without asking them first:
 *
 *  - keys go through the emulator's key queue, one entry per kbd_getKeys()
 *    call, the same queue the readline shell uses;
 *  - a screenshot is a request to the SDL thread, which answers when the file
 *    is written (sdlEngine_capture());
 *  - a dump is taken under state_mutex, which is the lock the UI thread holds
 *    while it runs the FSM, and only after the frame counter has advanced, so
 *    it describes the screen after the key and not before it.
 *
 * Everything a command changes is what the firmware itself would change: the
 * keys of the radio's keypad, the PTT line, the volume pot. Nothing here writes
 * the UI's state directly.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "core/aprs_service.h"
#include "core/messaging.h"
#include "core/msg_service.h"
#include "core/state.h"
#include "core/ui.h"
#include "protocols/APRS/ax25.h"
#include "protocols/DSTAR/DStarDefs.h"
#include "ui/ui_default.h"
#include "ui/ui_dump.h"

#include "emulator.h"
#include "sdl_engine.h"
#include "sim_control.h"

/** Line the long press waits for, plus a margin (core/input.c). */
#define LONG_PRESS_MS 750

/** How long a command waits for the UI to catch up, milliseconds. */
#define SETTLE_TIMEOUT_MS 4000

/** Size of the dump buffers: the JSON of a full settings page is large. */
#define DUMP_BUF 16384

static bool from_stdin = false;
static int listen_fd = -1;
static int client_fd = -1;
static FILE *out = NULL;
static pthread_mutex_t outLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned shotCounter;

/* --------------------------------------------------------------- output --- */

/**
 * One line of output.
 *
 * Payload lines carry the "#| " marker and control lines (#begin, #end,
 * #ready) do not: the firmware prints its own driver log to stdout from other
 * threads, so a client cannot tell an answer line from a log line by position.
 * The marker is what makes the protocol readable while the radio keeps
 * talking (see the @file comment).
 */
static void emit(const char *prefix, const char *fmt, va_list ap)
{
    char line[1024];

    vsnprintf(line, sizeof(line), fmt, ap);

    pthread_mutex_lock(&outLock);
    if (out != NULL) {
        fputs(prefix, out);
        fputs(line, out);
        fputc('\n', out);
        fflush(out);
    }
    pthread_mutex_unlock(&outLock);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** One payload line. */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("#| ", fmt, ap);
    va_end(ap);
}

/**
 * One payload block of any size, one "#| " per line.
 *
 * The dump is bigger than the line buffer of say(), and it is multi-line: it is
 * written straight through, with the marker in front of every line.
 */
static void say_block(const char *text)
{
    pthread_mutex_lock(&outLock);

    if (out != NULL) {
        const char *p = text;
        while (*p != '\0') {
            fputs("#| ", out);
            while (*p != '\0' && *p != '\n') {
                fputc(*p, out);
                p++;
            }
            fputc('\n', out);
            if (*p == '\n')
                p++;
        }
        fflush(out);
    }

    pthread_mutex_unlock(&outLock);
}

static void ctl(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** One control line: #begin, #end, #ready. */
static void ctl(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("", fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- timing -- */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/**
 * Wait until the UI thread has consumed every queued key and drawn at least
 * `frames` passes of its loop since the call started.
 */
static bool settle(int frames)
{
    long long deadline = now_ms() + SETTLE_TIMEOUT_MS;
    uint32_t base = ui_frames();

    /*
     * Three conditions, and all of them matter: the keypad queue is drained,
     * the FSM's event queue is empty (it dispatches one event per pass, so a
     * press and its release are two passes), and the drawing loop has run the
     * requested number of times since the wait started.
     */
    for (;;) {
        bool idle = (emulator_keyPending() == 0) && (ui_pendingEvents() == 0);
        bool drawn = (int32_t)(ui_frames() - base) >= frames;

        if (idle && drawn)
            break;

        if (now_ms() > deadline || emulator_state.powerOff)
            return false;

        sleep_ms(2);
    }

    /*
     * The framebuffer reaches the SDL thread through a rendezvous channel, and
     * gfx_render() returns once it has been copied: one more millisecond is
     * enough for the renderer to present it, and a screenshot taken after this
     * point shows the frame the dump describes.
     */
    sleep_ms(3);
    return true;
}

/* ----------------------------------------------------------------- keys --- */

static bool press(keyboard_t key, bool isLong)
{
    if (!emulator_keyEnqueue(key))
        return false;

    if (isLong) {
        /*
         * The keypad emulation is a queue consumed one entry per scan, so a key
         * that is not re-queued is released after one scan and the firmware
         * never sees it held. To hold it, keep feeding the queue for longer
         * than input_longPressTimeout (700 ms, core/input.c): the FSM emits the
         * long press itself once the key has been down that long.
         */
        long long end = now_ms() + LONG_PRESS_MS + 250;

        while (now_ms() < end && !emulator_state.powerOff) {
            emulator_keyEnqueue(key);
            sleep_ms(5);
        }

        emulator_keyEnqueue(0);
    } else {
        /* A press and a release: two entries, two scans (emulator.c) */
        emulator_keyEnqueue(0);
    }

    return settle(2);
}

/* ------------------------------------------------------------ commands ---- */

static void cmd_help(void)
{
    say("key <NAME> [long]     press a key (GREEN RED P1 P2 UP DOWN LEFT RIGHT "
        "SIDE 0..9 STAR HASH)");
    say("keys <NAME>...       several presses in a row");
    say("keycombo <NAME>...   press them together, then release");
    say("keydown <NAME>       hold a key (no release)");
    say("keyup <NAME>         release the held key");
    say("ptt on|off|toggle    the PTT line");
    say("volume <N|+N|-N>     the analog volume pot, 0..255");
    say("knob left|right      KNOB_LEFT / KNOB_RIGHT events");
    say("screenshot [file]    PNG of the screen (auto-numbered without a name)");
    say("dump [text|json]     the screen as text and the state behind it");
    say("sync                 wait for the UI to catch up");
    say("sleep <ms>           wait");
    say("rssi <f>, vbat <f>, mic <f>, channel <f>   the emulated front end");
    say("inject aprs-station <CALL> <LAT> <LON> <COMMENT>");
    say("                     one received APRS position (lat/lon: 38.87 or 3852.00N)");
    say("inject aprs-message <FROM> <TEXT>");
    say("                     one APRS message addressed to this station");
    say("inject message <SRC> <TEXT>");
    say("                     one message in the shared inbox (MSG_VIEW)");
    say("inject tetra-cell <MCC> <MNC> <CC> <DL_HZ> <UL_HZ>");
    say("                     a TETRA cell the host never decoded; UL_HZ 0 = none");
    say("inject tetra-cell clear");
    say("                     back to \"nothing decoded\"");
    say("inject dmr <CC> <TS> [evm <N>] [voice|data <TYPE>] [call <SRC> <DST> [P]]");
    say("                     a DMR burst decoded: colour code, slot, call, quality");
    say("inject dmr clear     nothing decoded (\"DMR searching\")");
    say("inject nxdn <RAN> [evm <N>] [frames <N>] [call <SRC> <DST> <TYPE>]");
    say("inject nxdn clear");
    say("inject p25 <NAC> [duid <N>] [frames <N>] [bad <N>] [talkgroup <SRC> <TG>]");
    say("inject p25 clear");
    say("inject dstar <MY> <UR> <RPT> [viterbi <N>] [text <WORDS...>]");
    say("inject dstar clear");
    say("inject tetra-registration <0|1> <GROUPS> <CAUSE>");
    say("inject tetra-call <0|1> <GROUP> <DST> <TALKER> <ID> <VOICE> [<ENC>]");
    say("inject identity <DMR_ID>     the station's own DMR ID");
    say("inject mode-state clear   every mode above, back to nothing decoded");
    say("quit                 power the radio off (the shutdown sequence runs)");
    say("powercut             cut the power: no shutdown, no write on the way out");
}

/*
 * Injection: what the receiver would have heard, handed to the services that
 * act on it. Nothing is written into the UI: the station list and the mailbox
 * are the service's, and they are filled through the same call the decoder
 * uses (core/aprs_service.c, aprs_sim_rxFrame), so a screen reached this way
 * shows what it would show on the air. docs/SIMULATOR.md.
 */

/** Join argv[from..argc) with single spaces: a message, not a command. */
static void joinFrom(char *out, size_t cap, int from, int argc, char **argv)
{
    size_t pos = 0;
    out[0] = '\0';
    for (int i = from; i < argc; i++) {
        int n = snprintf(out + pos, cap - pos, "%s%s", (pos > 0) ? " " : "",
                         argv[i]);
        if (n < 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
    }
}

/** Build an AX.25 UI frame from `call` and `info` and give it to the service. */
static bool injectFrame(const char *call, const char *info)
{
    ax25_frame_t f;
    memset(&f, 0, sizeof(f));
    ax25_addr_set(&f.dst, "APRS");
    ax25_addr_set(&f.src, call);
    f.control = 0x03;
    f.pid     = 0xF0;
    f.info    = (const uint8_t *)info;
    f.infoLen = strlen(info);

    uint8_t frame[AX25_MAX_INFO + 64];
    size_t len = ax25_build_ui(frame, sizeof(frame), &f);
    if (len < 2) {
        say("#error the frame does not fit");
        return false;
    }

    /* The deframer strips the FCS before the service sees a frame */
    if (!aprs_sim_rxFrame(frame, len - 2, (uint32_t)now_ms())) {
        say("#error the service refused the frame");
        return false;
    }

    say("#inject %s %s", call, info);
    return true;
}


/*
 * The digital modes' receiver state, for the state block of the main screen
 * (ui_mode_state.h). Each command fills what the mode's own decoder would have
 * written: they exist because a screen that only ever shows "SRCH" is a screen
 * whose layout nobody has looked at.
 */

static bool injectDmr(int argc, char **argv)
{
    if (argc < 4)
        return false;

    int cc = atoi(argv[2]);
    int ts = atoi(argv[3]);
    unsigned evm = 0;
    int lastType = -1;
    bool voice = true;
    bool ids = false, group = true;
    unsigned long src = 0, dst = 0;

    for (int i = 4; i < argc; i++) {
        if ((strcmp(argv[i], "evm") == 0) && (i + 1 < argc))
            evm = (unsigned)strtoul(argv[++i], NULL, 0);
        else if ((strcmp(argv[i], "voice") == 0) && (i + 1 < argc)) {
            voice = true;
            lastType = -1;
            i++;
        } else if ((strcmp(argv[i], "data") == 0) && (i + 1 < argc)) {
            voice = false;
            lastType = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "call") == 0) && (i + 2 < argc)) {
            src = strtoul(argv[++i], NULL, 0);
            dst = strtoul(argv[++i], NULL, 0);
            ids = true;
            group = true;
            if ((i + 1 < argc) && (strcmp(argv[i + 1], "P") == 0)) {
                group = false;
                i++;
            }
        }
    }

    dmr_sim_setStatus(true, cc, ts, voice, evm, ids, group, (uint32_t)src,
                      (uint32_t)dst, lastType);
    say("#inject dmr CC%d TS%d evm=%u call=%s", cc, ts, evm,
        ids ? "yes" : "no");
    return true;
}

static bool injectNxdn(int argc, char **argv)
{
    if (argc < 3)
        return false;

    int ran = atoi(argv[2]);
    unsigned evm = 0, frames = 0, type = 1;
    unsigned long src = 0, dst = 0;
    bool msg = false;

    for (int i = 3; i < argc; i++) {
        if ((strcmp(argv[i], "evm") == 0) && (i + 1 < argc))
            evm = (unsigned)strtoul(argv[++i], NULL, 0);
        else if ((strcmp(argv[i], "frames") == 0) && (i + 1 < argc))
            frames = (unsigned)strtoul(argv[++i], NULL, 0);
        else if ((strcmp(argv[i], "call") == 0) && (i + 3 < argc)) {
            src = strtoul(argv[++i], NULL, 0);
            dst = strtoul(argv[++i], NULL, 0);
            type = (unsigned)strtoul(argv[++i], NULL, 0);
            msg = true;
        }
    }

    nxdnStatus_simSet(true, ran, true, evm, msg, (unsigned)src,
                      (unsigned)dst, type, frames);
    say("#inject nxdn RAN%d evm=%u frames=%u", ran, evm, frames);
    return true;
}

static bool injectDpmr(int argc, char **argv)
{
    if (argc < 3)
        return false;

    /* "inject dpmr -" is a receiver that is locked and has no code yet */
    int cc = (strcmp(argv[2], "-") == 0) ? -1 : (int)strtol(argv[2], NULL, 0);
    unsigned evm = 0, frames = 0, type = 0;
    unsigned long src = 0, dst = 0;
    int endStatus = -1;
    bool ids = false, receiving = true;

    for (int i = 3; i < argc; i++) {
        if ((strcmp(argv[i], "evm") == 0) && (i + 1 < argc))
            evm = (unsigned)strtoul(argv[++i], NULL, 0);
        else if ((strcmp(argv[i], "frames") == 0) && (i + 1 < argc))
            frames = (unsigned)strtoul(argv[++i], NULL, 0);
        else if ((strcmp(argv[i], "rx") == 0) && (i + 1 < argc))
            receiving = atoi(argv[++i]) != 0;
        else if ((strcmp(argv[i], "end") == 0) && (i + 1 < argc)) {
            /* An END frame is what closes a call: TX_REL, with its status */
            endStatus = (int)strtol(argv[++i], NULL, 0);
            type = 2;
        } else if ((strcmp(argv[i], "call") == 0) && (i + 3 < argc)) {
            src = strtoul(argv[++i], NULL, 0);
            dst = strtoul(argv[++i], NULL, 0);
            type = (unsigned)strtoul(argv[++i], NULL, 0);
            ids = true;
        }
    }

    dpmrStatus_simSet(true, receiving, cc, evm, frames, ids, src, dst, type,
                      endStatus);
    say("#inject dpmr CC%d call=%s type=%u rx=%d evm=%u frames=%u", cc,
        ids ? "yes" : "no", type, receiving ? 1 : 0, evm, frames);
    return true;
}

static bool injectP25(int argc, char **argv)
{
    if (argc < 3)
        return false;

    int nac = (int)strtol(argv[2], NULL, 0);
    int duid = -1, tg = -1;
    long src = -1;
    unsigned frames = 0, bad = 0;

    for (int i = 3; i < argc; i++) {
        if ((strcmp(argv[i], "duid") == 0) && (i + 1 < argc))
            duid = (int)strtol(argv[++i], NULL, 0);
        else if ((strcmp(argv[i], "frames") == 0) && (i + 1 < argc))
            frames = (unsigned)strtoul(argv[++i], NULL, 0);
        else if ((strcmp(argv[i], "bad") == 0) && (i + 1 < argc))
            bad = (unsigned)strtoul(argv[++i], NULL, 0);
        else if ((strcmp(argv[i], "talkgroup") == 0) && (i + 2 < argc)) {
            src = strtol(argv[++i], NULL, 0);
            tg = (int)strtol(argv[++i], NULL, 0);
        }
    }

    p25Status_simSet(true, true, nac, duid, src, tg, frames ? frames : 1,
                     bad, frames);
    say("#inject p25 NAC%03X", (unsigned)nac);
    return true;
}

static bool injectDstar(int argc, char **argv)
{
    if (argc < 5)
        return false;

    int viterbi = 0;
    char text[DSTAR_TEXT_LEN + 1] = "";

    for (int i = 5; i < argc; i++) {
        if ((strcmp(argv[i], "viterbi") == 0) && (i + 1 < argc))
            viterbi = atoi(argv[++i]);
        else if (strcmp(argv[i], "text") == 0) {
            joinFrom(text, sizeof(text), i + 1, argc, argv);
            break;
        }
    }

    dstarStatus_simSet(true, true, argv[2], argv[3], argv[4], viterbi, text);
    say("#inject dstar %s>%s rpt=%s", argv[2], argv[3], argv[4]);
    return true;
}

static bool cmd_inject(int argc, char **argv)
{
    char text[APRS_COMMENT_MAX + 64];

    /*
     * Build the message store before the first message lands in it: on the
     * radio state_init() does that at boot, and the screens' own entry point
     * (msg_service_init) loads the record only if nobody has yet -- a message
     * injected before that would be cleared by the reload.
     */
    msg_service_init();

    if (argc < 2) {
        say("#error inject aprs-station|aprs-message|message ...");
        return false;
    }

    if (strcmp(argv[1], "aprs-station") == 0 && argc > 3) {
        const char *call = argv[2];
        aprs_pos_t pos;
        memset(&pos, 0, sizeof(pos));
        if (!aprs_parse_lat(argv[3], &pos.lat) ||
            !aprs_parse_lon(argv[4], &pos.lon)) {
            say("#error lat/lon not understood");
            return false;
        }
        pos.table  = '/';
        pos.symbol = '-';
        joinFrom(pos.comment, sizeof(pos.comment), 5, argc, argv);

        if (aprs_build_position(text, sizeof(text), &pos, true) == 0) {
            say("#error the position does not fit");
            return false;
        }
        return injectFrame(call, text);
    }

    if (strcmp(argv[1], "aprs-message") == 0 && argc > 3) {
        char own[12];
        char msg[APRS_COMMENT_MAX + 32];
        aprs_ownCallsign(own, sizeof(own));
        joinFrom(msg, sizeof(msg), 3, argc, argv);
        if (aprs_build_message(text, sizeof(text), own, msg, "1") == 0) {
            say("#error the message does not fit");
            return false;
        }
        return injectFrame(argv[2], text);
    }

    if (strcmp(argv[1], "message") == 0 && argc > 3) {
        long src = strtol(argv[2], NULL, 0);
        joinFrom(text, sizeof(text), 3, argc, argv);
        int id = messaging_received(MSG_TRANSPORT_APRS, (uint32_t)src, 0,
                                    text, false, (uint32_t)now_ms());
        if (id < 0) {
            say("#error the inbox refused the message");
            return false;
        }
        say("#inject message id=%d", id);
        return true;
    }

    /*
     * A TETRA cell: what a receiver locked to a downlink would hold, so the
     * "Cell uplink" row of the TETRA quick menu can be walked with a known
     * uplink frequency (UL_HZ != 0), with a cell whose uplink is unknown
     * (UL_HZ 0), and with no cell at all (clear).  Nothing is decoded here:
     * this is the receiver's state, filled the way TetraStatus.c fills it.
     */
    if (strcmp(argv[1], "tetra-cell") == 0 && argc > 2) {
        if (strcmp(argv[2], "clear") == 0) {
            tetra_sim_clearCell();
            say("#inject tetra-cell clear");
            return true;
        }
        if (argc > 6) {
            unsigned long mcc = strtoul(argv[2], NULL, 0);
            unsigned long mnc = strtoul(argv[3], NULL, 0);
            unsigned long cc = strtoul(argv[4], NULL, 0);
            unsigned long dl = strtoul(argv[5], NULL, 0);
            unsigned long ul = strtoul(argv[6], NULL, 0);

            tetra_sim_setCell((uint16_t)mcc, (uint16_t)mnc, (uint8_t)cc,
                              (uint32_t)dl, (uint32_t)ul);
            say("#inject tetra-cell %lu-%lu CC%lu dl=%lu ul=%lu", mcc, mnc, cc,
                dl, ul);
            return true;
        }
    }

    if (strcmp(argv[1], "tetra-registration") == 0 && argc > 4) {
        bool accepted = atoi(argv[2]) != 0;
        uint8_t groups = (uint8_t)strtoul(argv[3], NULL, 0);
        uint8_t cause = (uint8_t)strtoul(argv[4], NULL, 0);

        tetra_sim_setRegistration(accepted, groups, cause);
        say("#inject tetra-registration %s G%u cause=%u",
            accepted ? "accepted" : "rejected", (unsigned)groups,
            (unsigned)cause);
        return true;
    }

    if (strcmp(argv[1], "tetra-call") == 0 && argc > 7) {
        bool call = atoi(argv[2]) != 0;
        bool group = atoi(argv[3]) != 0;
        uint32_t dst = (uint32_t)strtoul(argv[4], NULL, 0);
        uint32_t talker = (uint32_t)strtoul(argv[5], NULL, 0);
        uint16_t id = (uint16_t)strtoul(argv[6], NULL, 0);
        bool voice = atoi(argv[7]) != 0;
        bool enc = (argc > 8) && (atoi(argv[8]) != 0);

        tetra_sim_setCall(call, group, dst, talker, id, voice, enc);
        say("#inject tetra-call %s %s%lu %lu id=%u", call ? "on" : "off",
            group ? "G" : "", (unsigned long)dst, (unsigned long)talker,
            (unsigned)id);
        return true;
    }

    /*
     * The station's own identifiers, so a screen that refuses to transmit for
     * lack of one (DMR: "NO TX: DMR ID 0") can be looked at with one set. It
     * writes the settings record the Identity page writes, nothing else.
     */
    if (strcmp(argv[1], "identity") == 0 && argc > 2) {
#ifdef UI_MODE_DMR
        state.settings.dmr_id = (uint32_t)strtoul(argv[2], NULL, 0);
        say("#inject identity dmr_id=%lu",
            (unsigned long)state.settings.dmr_id);
#else
        say("#error this build has no DMR identity");
#endif
        return true;
    }

    if (strcmp(argv[1], "dmr") == 0) {
        if ((argc == 3) && (strcmp(argv[2], "clear") == 0)) {
            dmr_sim_clear();
            say("#inject dmr clear");
            return true;
        }
        return injectDmr(argc, argv);
    }

    if (strcmp(argv[1], "nxdn") == 0) {
        if ((argc == 3) && (strcmp(argv[2], "clear") == 0)) {
            nxdnStatus_simSet(false, -1, false, 0, false, 0, 0, 0, 0);
            say("#inject nxdn clear");
            return true;
        }
        return injectNxdn(argc, argv);
    }

    if (strcmp(argv[1], "dpmr") == 0) {
        if ((argc == 3) && (strcmp(argv[2], "clear") == 0)) {
            dpmrStatus_simSet(false, false, -1, 0, 0, false, 0, 0, 0, -1);
            say("#inject dpmr clear");
            return true;
        }
        return injectDpmr(argc, argv);
    }

    if (strcmp(argv[1], "p25") == 0) {
        if ((argc == 3) && (strcmp(argv[2], "clear") == 0)) {
            p25Status_simSet(false, false, -1, -1, -1, -1, 0, 0, 0);
            say("#inject p25 clear");
            return true;
        }
        return injectP25(argc, argv);
    }

    if (strcmp(argv[1], "dstar") == 0) {
        if ((argc == 3) && (strcmp(argv[2], "clear") == 0)) {
            dstarStatus_simSet(false, false, "", "", "", 0, "");
            say("#inject dstar clear");
            return true;
        }
        return injectDstar(argc, argv);
    }

    if (strcmp(argv[1], "mode-state") == 0) {
        dmr_sim_clear();
        nxdnStatus_simSet(false, -1, false, 0, false, 0, 0, 0, 0);
        dpmrStatus_simSet(false, false, -1, 0, 0, false, 0, 0, 0, -1);
        p25Status_simSet(false, false, -1, -1, -1, -1, 0, 0, 0);
        dstarStatus_simSet(false, false, "", "", "", 0, "");
        tetra_sim_clearCell();
        say("#inject mode-state clear");
        return true;
    }

    say("#error inject needs a station, a message and their arguments");
    return false;
}

static bool cmd_key(int argc, char **argv, bool combo)
{
    if (argc < 2) {
        say("#error key needs a name");
        return false;
    }

    if (combo) {
        keyboard_t keys = 0;
        for (int i = 1; i < argc; i++) {
            keyboard_t k = emulator_keyFromName(argv[i]);
            if (k == 0) {
                say("#error unknown key \"%s\"", argv[i]);
                return false;
            }
            keys |= k;
        }
        emulator_keyEnqueue(keys);
        emulator_keyEnqueue(0);
        return settle(2);
    }

    for (int i = 1; i < argc; i++) {
        /* "long" qualifies the key written before it: `key P2 long` */
        if (strcmp(argv[i], "long") == 0)
            continue;

        bool isLong = (i + 1 < argc) && (strcmp(argv[i + 1], "long") == 0);

        keyboard_t k = emulator_keyFromName(argv[i]);
        if (k == 0) {
            say("#error unknown key \"%s\"", argv[i]);
            return false;
        }

        if (!press(k, isLong))
            return false;
    }

    return true;
}

static void cmd_dump(const char *format)
{
    static char buf[DUMP_BUF];
    bool json = (format != NULL) && (strcmp(format, "json") == 0);

    if (!settle(1))
        say("#warning the UI did not advance: the dump may be stale");

    bool locked = state_tryLock(500);
    size_t n = json ? ui_dump_json(buf, sizeof(buf)) :
                      ui_dump_text(buf, sizeof(buf));
    if (locked)
        state_unlock();

    if (n > 0)
        say_block(buf);

    if (!locked)
        say("#warning state_mutex was busy: the dump was taken unlocked");
}

static void cmd_screenshot(const char *path)
{
    char name[512];

    if (path == NULL) {
        const char *dir = getenv("OPENRTX_SIM_SHOTS");
        if (dir == NULL || dir[0] == '\0')
            dir = ".";
        snprintf(name, sizeof(name), "%s/shot-%04u.png", dir, ++shotCounter);
    } else {
        snprintf(name, sizeof(name), "%s", path);
    }

    if (!settle(2)) {
        say("#error the UI did not advance before the screenshot");
        return;
    }

    if (sdlEngine_capture(name, true, 5000)) {
        struct stat st;
        long size = (stat(name, &st) == 0) ? (long)st.st_size : -1;
        say("#shot %s %ld", name, size);
    } else {
        say("#error cannot write %s", name);
    }
}

static void cmd_volume(const char *arg)
{
    float *pot = &emulator_state.volumeLevel;
    float value = *pot;

    if (arg == NULL) {
        say("#volume %d", (int)value);
        return;
    }

    if (arg[0] == '+' || arg[0] == '-')
        value += atof(arg);
    else
        value = atof(arg);

    if (value < 0.0f)
        value = 0.0f;
    if (value > 255.0f)
        value = 255.0f;

    *pot = value;

    if (state_tryLock(200)) {
        state.volume = (uint8_t)value;
        state_unlock();
    }

    say("#volume %d", (int)value);
}

static void cmd_knob(const char *arg)
{
    if (arg == NULL) {
        say("#error knob needs left or right");
        return;
    }

    if (strcasecmp(arg, "left") == 0)
        say("#knob %d", press(KNOB_LEFT, false) ? -1 : 0);
    else if (strcasecmp(arg, "right") == 0)
        say("#knob %d", press(KNOB_RIGHT, false) ? 1 : 0);
    else
        say("#error knob needs left or right");
}

static void cmd_ptt(const char *arg)
{
    if (arg == NULL || strcasecmp(arg, "toggle") == 0)
        emulator_state.PTTstatus = !emulator_state.PTTstatus;
    else if (strcasecmp(arg, "on") == 0 || strcasecmp(arg, "down") == 0)
        emulator_state.PTTstatus = true;
    else if (strcasecmp(arg, "off") == 0 || strcasecmp(arg, "up") == 0)
        emulator_state.PTTstatus = false;
    else {
        say("#error ptt on|off|toggle");
        return;
    }

    settle(2);
    say("#ptt %s", emulator_state.PTTstatus ? "on" : "off");
}

/** Result of one command line. */
enum cmd_result { CMD_OK = 0, CMD_ERR, CMD_QUIT };

/*
 * One command is at most this many words. It is 16 and not 12 because an
 * `inject` with every option of a mode (`inject dpmr CC call S D TYPE frames N
 * evm N rx 0`) is 13, and the day the tokeniser hit its ceiling it dropped the
 * tail in silence: `rx 0` never arrived and the screen kept showing "RX". A
 * command that does not fit is answered with an error instead.
 */
#define CMD_MAX_WORDS 16

static enum cmd_result execute(char *line)
{
    char *argv[CMD_MAX_WORDS];
    int argc = 0;
    bool overflow = false;

    for (char *tok = strtok(line, " \t\r\n"); tok != NULL;
         tok = strtok(NULL, " \t\r\n")) {
        if (argc >= CMD_MAX_WORDS) {
            overflow = true;
            break;
        }
        argv[argc++] = tok;
    }

    if (argc == 0)
        return CMD_OK;

    if (overflow) {
        say("#error too many words: a command is at most %d", CMD_MAX_WORDS);
        return CMD_ERR;
    }

    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "key") == 0 || strcmp(cmd, "keys") == 0
               || strcmp(cmd, "keyseq") == 0) {
        if (!cmd_key(argc, argv, false))
            return CMD_ERR;
    } else if (strcmp(cmd, "keycombo") == 0) {
        if (!cmd_key(argc, argv, true))
            return CMD_ERR;
    } else if (strcmp(cmd, "hold") == 0) {
        keyboard_t k = (argc > 1) ? emulator_keyFromName(argv[1]) : 0;
        int ms = (argc > 2) ? atoi(argv[2]) : LONG_PRESS_MS + 100;
        if (k == 0) {
            say("#error unknown key");
            return CMD_ERR;
        }
        long long end = now_ms() + ms;
        int fed = 0;
        while (now_ms() < end) {
            if (emulator_keyEnqueue(k))
                fed++;
            sleep_ms(5);
        }
        emulator_keyEnqueue(0);
        settle(2);
        say("#hold %dms fed=%d", ms, fed);
    } else if (strcmp(cmd, "keydown") == 0) {
        keyboard_t k = (argc > 1) ? emulator_keyFromName(argv[1]) : 0;
        if (k == 0) {
            say("#error unknown key");
            return CMD_ERR;
        }
        emulator_keyEnqueue(k);
        settle(1);
    } else if (strcmp(cmd, "keyup") == 0) {
        emulator_keyEnqueue(0);
        settle(1);
    } else if (strcmp(cmd, "ptt") == 0) {
        cmd_ptt((argc > 1) ? argv[1] : NULL);
    } else if (strcmp(cmd, "volume") == 0) {
        cmd_volume((argc > 1) ? argv[1] : NULL);
    } else if (strcmp(cmd, "knob") == 0) {
        cmd_knob((argc > 1) ? argv[1] : NULL);
    } else if (strcmp(cmd, "rssi") == 0 && argc > 1) {
        emulator_state.RSSI = atof(argv[1]);
    } else if (strcmp(cmd, "vbat") == 0 && argc > 1) {
        emulator_state.vbat = atof(argv[1]);
    } else if (strcmp(cmd, "mic") == 0 && argc > 1) {
        emulator_state.micLevel = atof(argv[1]);
    } else if (strcmp(cmd, "channel") == 0 && argc > 1) {
        emulator_state.chSelector = atof(argv[1]);
    } else if (strcmp(cmd, "inject") == 0) {
        if (!cmd_inject(argc, argv))
            return CMD_ERR;
    } else if (strcmp(cmd, "screenshot") == 0) {
        cmd_screenshot((argc > 1) ? argv[1] : NULL);
    } else if (strcmp(cmd, "dump") == 0) {
        cmd_dump((argc > 1) ? argv[1] : "text");
    } else if (strcmp(cmd, "probe") == 0) {
        say("#probe frames=%lu keys_pending=%d events=%u last_key=%lu",
            (unsigned long)ui_frames(), emulator_keyPending(),
            ui_pendingEvents(), (unsigned long)ui_lastKeyEvent());
    } else if (strcmp(cmd, "sync") == 0) {
        if (!settle(2))
            say("#warning the UI did not advance");
    } else if (strcmp(cmd, "sleep") == 0) {
        sleep_ms((argc > 1) ? atoi(argv[1]) : 0);
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "poweroff") == 0) {
        emulator_state.powerOff = true;
        return CMD_QUIT;
    } else if (strcmp(cmd, "powercut") == 0) {
        /*
         * The radio's own way off: the volume knob cuts the rail, so neither
         * state_terminate() nor platform_terminate() runs and whatever the
         * settings debouncer had not written yet is lost (core/state.c).  A
         * test needs it to tell "the change survived the power cut" from "the
         * shutdown wrote it on the way out".  _exit() and not exit(): no
         * atexit handlers, no stdio flush beyond what the answers already
         * flushed, nothing of ours can sneak a write in.
         */
        ctl("#end ok");
        fflush(NULL);
        _exit(0);
    } else {
        say("#error unknown command \"%s\" (try help)", cmd);
        return CMD_ERR;
    }

    return CMD_OK;
}

/* --------------------------------------------------------------- channel -- */

static void serve(FILE *in)
{
    char line[1024];

    while (fgets(line, sizeof(line), in) != NULL) {
        /*
         * Framed answer: "#begin", the payload, "#end ok|err". The firmware
         * prints to stdout as well (the radio's own driver log), so a client
         * cannot assume that every line between two answers is an answer;
         * these two lines are what makes it possible to tell them apart.
         */
        ctl("#begin");

        enum cmd_result rc = execute(line);

        if (rc == CMD_QUIT) {
            say("QUIT: 73!");
            ctl("#end ok");
            return;
        }

        ctl((rc == CMD_OK) ? "#end ok" : "#end err");

        if (emulator_state.powerOff)
            return;
    }
}

static int open_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
        || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void *controlThread(void *arg)
{
    (void)arg;

    /* Wait for the UI to be up before the first command is answered: a key
       pressed before ui_threadFunc exists is a key nobody consumes. */
    long long deadline = now_ms() + 10000;
    while (!sdlEngine_ready() || ui_frames() < 2) {
        if (now_ms() > deadline || emulator_state.powerOff)
            break;
        sleep_ms(5);
    }

    out = stdout;
    ctl("#ready %s %dx%d", "openrtx-simulator", CONFIG_SCREEN_WIDTH,
        CONFIG_SCREEN_HEIGHT);

    if (listen_fd >= 0) {
        struct pollfd pfd[2];
        int nfds = 0;

        if (from_stdin) {
            pfd[nfds].fd = STDIN_FILENO;
            pfd[nfds].events = POLLIN;
            nfds++;
        }
        pfd[nfds].fd = listen_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds + 1].fd = -1;
        nfds++;

        for (;;) {
            if (poll(pfd, (nfds_t)nfds, 200) <= 0)
                continue;

            if (from_stdin && (pfd[0].revents & POLLIN)) {
                serve(stdin);
                if (emulator_state.powerOff)
                    break;
                from_stdin = false; /* the pipe is done if it closed */
            }

            if (pfd[nfds - 1].revents & POLLIN) {
                client_fd = accept(listen_fd, NULL, NULL);
                if (client_fd >= 0) {
                    FILE *f = fdopen(client_fd, "r+");
                    if (f != NULL) {
                        pthread_mutex_lock(&outLock);
                        out = f;
                        pthread_mutex_unlock(&outLock);
                        ctl("#ready %s %dx%d", "openrtx-simulator",
                            CONFIG_SCREEN_WIDTH, CONFIG_SCREEN_HEIGHT);
                        serve(f);
                        pthread_mutex_lock(&outLock);
                        out = stdout;
                        pthread_mutex_unlock(&outLock);
                        fclose(f);
                        client_fd = -1;
                    }
                }
            }

            if (emulator_state.powerOff)
                break;
        }
    } else {
        serve(stdin);
    }

    if (!emulator_state.powerOff) {
        emulator_state.powerOff = true;
        printf("Terminating the simulator, goodbye!\n");
    }

    return NULL;
}

bool sim_control_wanted(void)
{
    const char *headless = getenv("OPENRTX_SIM_HEADLESS");
    const char *sock = getenv("OPENRTX_SIM_SOCKET");

    if (headless != NULL && headless[0] != '\0' && strcmp(headless, "0") != 0)
        from_stdin = true;

    return from_stdin || (sock != NULL && sock[0] != '\0');
}

void sim_control_start(void)
{
    const char *sock = getenv("OPENRTX_SIM_SOCKET");

    if (sock != NULL && sock[0] != '\0') {
        listen_fd = open_socket(sock);

        if (listen_fd < 0)
            fprintf(stderr, "simulator: cannot create socket %s: %s\n", sock,
                    strerror(errno));
        else
            printf("simulator: control socket at %s\n", sock);
    }

    pthread_t thread;
    int err = pthread_create(&thread, NULL, controlThread, NULL);

    if (err != 0) {
        fprintf(stderr, "simulator: cannot start the control thread: %d\n",
                err);
        emulator_state.powerOff = true;
    }
}
