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

#include "core/state.h"
#include "core/ui.h"
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
static int  listen_fd  = -1;
static int  client_fd  = -1;
static FILE *out       = NULL;
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
    ts.tv_sec  = ms / 1000;
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
    say("quit                 power the radio off");
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
    size_t n = json ? ui_dump_json(buf, sizeof(buf))
                    : ui_dump_text(buf, sizeof(buf));
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
enum cmd_result {
    CMD_OK = 0,
    CMD_ERR,
    CMD_QUIT
};

static enum cmd_result execute(char *line)
{
    char *argv[12];
    int argc = 0;

    for (char *tok = strtok(line, " \t\r\n"); tok != NULL && argc < 12;
         tok = strtok(NULL, " \t\r\n"))
        argv[argc++] = tok;

    if (argc == 0)
        return CMD_OK;

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
    const char *sock     = getenv("OPENRTX_SIM_SOCKET");

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
