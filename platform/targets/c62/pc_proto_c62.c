/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Transport of the PC protocol on the C62: the console UART, the same cable the
 * baseband dump and the remote control use.  Built only with C62_PC_PROTO.
 *
 * Arbitration with the log, which shares this UART:
 *
 *  - The printk and stdout hooks are taken over at start-up.  Every line goes
 *    into a small ring in this file, never straight to the wire.
 *  - While no client is linked (no HELLO accepted), the hook also writes the
 *    characters to the UART as before, so the boot log is still visible on a
 *    serial terminal and nothing changes for anyone who is not using the
 *    protocol.
 *  - As soon as a frame arrives and links, the hook stops writing to the UART:
 *    log text can then only leave inside a PC_EVT_LOG frame, which the client
 *    asked for.  A log line cannot corrupt a frame because it never shares the
 *    wire with one -- the corruption is impossible, not unlikely.
 *  - On SESSION_CLOSE, or after PC_PROTO_IDLE_MS without a valid frame, the
 *    link drops and the log goes back to the UART.
 *
 * The baseband dump, when the build has it, is paused while a client is linked
 * and resumed when it goes away: its sample packets and our frames would
 * interleave otherwise.  The dump state and its trigger are reachable through
 * PC_MSG_DBG_BB, which is the point of moving it under the protocol.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <IOMuxManager.h>

#include "core/openrtx.h"
#include "core/pc_proto.h"
#include "core/state.h"
#include "rtx/rtx.h"
#include "rtx/TxGuard.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include <hwconfig.h>
#include "drivers/baseband/BK4819.h"
#include "drivers/baseband/BK1080.h"
#include "drivers/audio/baseband_dump_c62.h"
#include "drivers/audio/csk6_codec.h"

/* Raw DesignWare UART registers, as in the baseband dump and the remote. */
#define UART_BASE DT_REG_ADDR(DT_CHOSEN(zephyr_console))
#define UART_RBR 0x00
#define UART_THR 0x00
#define UART_LSR 0x14
#define LSR_DR BIT(0)
#define LSR_THRE BIT(5)

#define THREAD_PRIORITY 6
#define THREAD_STACK 4096
#define POLL_US 1000

/** A link with no valid frame for this long goes away and frees the log. */
#define PC_PROTO_IDLE_MS 15000

/** Log lines buffered here, and the longest one kept. */
#define LINE_RING 4
#define LINE_MAX 128

static pcProto_t proto;
static struct k_thread thread;
static K_THREAD_STACK_DEFINE(thread_stack, THREAD_STACK);
static bool started;
static uint32_t lastFrameMs;
static uint32_t framesSeen;

/* Log capture: the hook appends characters here, the transport thread turns
 * complete lines into pc_proto_logPush() calls.  Guarded by irq_lock() because
 * the hook runs in whatever context produced the line. */
static char lineRing[LINE_RING][LINE_MAX];
static uint8_t lineLen[LINE_RING];
static uint8_t lineHead;
static uint8_t lineTail;
static uint8_t lineCount;

static int (*prevPrintkHook)(int c);

/* ------------------------------------------------------------------------- */
/* Port                                                                      */
/* ------------------------------------------------------------------------- */

static int uartWrite(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    /* Bounded wait: a full FIFO with nobody reading must not park the thread
     * with the radio unresponsive.  What does not fit is dropped and counted
     * by the caller as txDropped. */
    for (size_t i = 0; i < len; i++) {
        uint32_t spins = 0;
        while ((sys_read32(UART_BASE + UART_LSR) & LSR_THRE) == 0) {
            if (++spins > 20000)
                return -(int)i;
            k_busy_wait(1);
        }
        sys_write32(buf[i], UART_BASE + UART_THR);
    }
    return (int)len;
}

static const struct pcProtoPort port = { .write = uartWrite, .ctx = NULL };

/* ------------------------------------------------------------------------- */
/* Log capture hook                                                          */
/* ------------------------------------------------------------------------- */

static void lineQueue(const char *line, size_t len)
{
    unsigned int key = irq_lock();
    if (lineCount == LINE_RING) {
        /* Full: the oldest line goes, and the client sees the gap in the
         * sequence numbers and the dropped counter. */
        lineTail = (uint8_t)((lineTail + 1) % LINE_RING);
        lineCount--;
        proto.logDropped++;
    }
    uint8_t slot = lineHead;
    if (len >= LINE_MAX)
        len = LINE_MAX - 1;
    memcpy(lineRing[slot], line, len);
    lineRing[slot][len] = '\0';
    lineLen[slot] = (uint8_t)len;
    lineHead = (uint8_t)((lineHead + 1) % LINE_RING);
    lineCount++;
    irq_unlock(key);
}

/**
 * Split a Zephyr log line into level, module and text.
 *
 * The console prints "[00:00:01.234,567] <inf> net: text" when the log backend
 * is on, and plain text otherwise.  A line that does not match leaves the
 * level at INFO and the module empty, which is the safe assumption: it is
 * shown unless the client asked for errors only.
 */
static void lineFeed(const char *line)
{
    const char *p = line;
    uint8_t level = PC_LOG_INFO;
    char module[8] = "";

    if (p[0] == '[') {
        const char *close = strchr(p, ']');
        if (close != NULL)
            p = close + 1;
    }
    while (*p == ' ')
        p++;
    if (p[0] == '<' && p[3] == '>') {
        if (!strncmp(p + 1, "err", 3))
            level = PC_LOG_ERROR;
        else if (!strncmp(p + 1, "wrn", 3))
            level = PC_LOG_WARN;
        else if (!strncmp(p + 1, "inf", 3))
            level = PC_LOG_INFO;
        else
            level = PC_LOG_DEBUG;
        p += 4;
    }
    while (*p == ' ')
        p++;

    /* "module: text" */
    const char *colon = strchr(p, ':');
    if (colon != NULL && (size_t)(colon - p) < sizeof(module)) {
        size_t n = (size_t)(colon - p);
        memcpy(module, p, n);
        module[n] = '\0';
        p = colon + 1;
        while (*p == ' ')
            p++;
    }
    pc_proto_logPush(&proto, level, module[0] ? module : "fw", p);
}

static int logHook(int c)
{
    static char line[LINE_MAX];
    static size_t len;

    if (c == '\n' || c == '\r') {
        if (len > 0) {
            line[len] = '\0';
            lineFeed(line);
            len = 0;
        }
    } else if (len < sizeof(line) - 1) {
        line[len++] = (char)c;
    }

    /* Before a client links, the console is still the console. */
    if (!pc_proto_linked(&proto)) {
        if (prevPrintkHook != NULL)
            return prevPrintkHook(c);
        return c;
    }
    return c; /* swallowed: it is in the ring, it leaves inside a frame */
}

extern void __printk_hook_install(int (*fn)(int c));
extern void __stdout_hook_install(int (*hook)(int c));
extern int (*__printk_hook_get(void))(int c);

/* ------------------------------------------------------------------------- */
/* Platform hooks of the protocol                                            */
/* ------------------------------------------------------------------------- */

int pc_proto_platformStatus(pcProtoStatus_t *out)
{
    pthread_mutex_lock(&state_mutex);
    out->rxFreq = state.channel.rx_frequency;
    out->txFreq = state.channel.tx_frequency;
    out->mode = state.channel.mode;
    out->channelIdx = (state.tuner_mode == VFO) ? 0xFFFF : state.channel_index;
    out->batteryMv = state.v_bat;
    out->txState = (uint8_t)state.rtxStatus;
    if (state.tuner_mode != VFO) {
        size_t n = 0;
        while (n + 1 < sizeof(out->channelName) &&
               state.channel.name[n] != '\0') {
            out->channelName[n] = state.channel.name[n];
            n++;
        }
        out->channelName[n] = '\0';
    }
    pthread_mutex_unlock(&state_mutex);

    out->rssi = rtx_getRssi();
    out->squelchOpen = rtx_rxSquelchOpen() ? 1 : 0;
    return 0;
}

const char *pc_proto_platformModel(void)
{
    static char name[16];
    if (name[0] == '\0') {
        const hwInfo_t *hw = platform_getHwInfo();
        snprintf(name, sizeof(name), "%s", hw != NULL ? hw->name : "c62");
    }
    return name;
}

const char *pc_proto_platformFirmware(void)
{
    /* The build does not stamp a version: the protocol and the model are what
     * a client gets.  A release that does stamp one changes this line. */
    return "OpenRTX c62";
}

const char *pc_proto_platformHardware(void)
{
    static char hw[16];
    const hwInfo_t *info = platform_getHwInfo();
    if (hw[0] == '\0')
        snprintf(hw, sizeof(hw), "rev %u",
                 (unsigned)(info != NULL ? info->hw_version : 0));
    return hw;
}

#ifdef CONFIG_C62_DEBUG_PROTO

/* The linker knows these when the board script provides them; the weak
 * references let the same file build on a script that does not. */
extern char __rom_region_start[] __attribute__((weak));
extern char __rom_region_end[] __attribute__((weak));
extern char _image_ram_start[] __attribute__((weak));
extern char _image_ram_end[] __attribute__((weak));

int pc_proto_platformDiag(pcProtoDiag_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Reset cause.  The CSK6 port does not implement the hwinfo driver, so
     * what the SoC latched is not readable from here: 0 means unknown, and the
     * client says so instead of inventing a reason. */
    out->resetCause = 0;

#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
    {
        extern struct sys_heap _system_heap;
        struct sys_heap_runtime_stats st;
        if (sys_heap_runtime_stats_get(&_system_heap, &st) == 0) {
            out->heapFree = (uint32_t)st.free_bytes;
            out->heapTotal = (uint32_t)(st.free_bytes + st.allocated_bytes);
        }
    }
#else
    out->heapTotal = CONFIG_HEAP_MEM_POOL_SIZE;
#endif

    if (__rom_region_start != NULL && __rom_region_end != NULL) {
        out->flashUsed = (uint32_t)((uintptr_t)__rom_region_end -
                                    (uintptr_t)__rom_region_start);
        out->flashTotal = CONFIG_FLASH_SIZE * 1024u;
    }
    if (_image_ram_start != NULL && _image_ram_end != NULL) {
        out->ramUsed = (uint32_t)((uintptr_t)_image_ram_end -
                                  (uintptr_t)_image_ram_start);
        out->ramTotal = CONFIG_SRAM_SIZE * 1024u;
    }

    /* Stack high-water marks.  k_thread_stack_space_get() needs
     * CONFIG_INIT_STACKS to know what was never touched; without it the number
     * is the free space at this instant, which is still the field a bench
     * session reads.  Only the two threads this file can name are reported:
     * enumerating the rest needs CONFIG_THREAD_MONITOR. */
#if defined(CONFIG_THREAD_STACK_INFO)
    {
        extern struct k_thread z_main_thread;
        const struct k_thread *list[2] = { &z_main_thread, &thread };
        const char *names[2] = { "main", "pcproto" };
        uint16_t sizes[2] = { (uint16_t)CONFIG_MAIN_STACK_SIZE,
                              (uint16_t)K_THREAD_STACK_SIZEOF(thread_stack) };

        for (int i = 0; i < 2; i++) {
            size_t unused = 0;
            if (k_thread_stack_space_get(list[i], &unused) != 0)
                continue;
            pcProtoTaskStack_t *t = &out->tasks[out->taskCount++];
            snprintf(t->name, sizeof(t->name), "%s", names[i]);
            t->unused = (uint16_t)unused;
            t->size = sizes[i];
        }
    }
#endif
    out->agc = 0xFF;
    return 0;
}

int pc_proto_platformReg(int chip, uint8_t reg, uint16_t *value)
{
    if (chip == 0) {
        *value = bk4819_readReg(&c62_bk4819, reg);
        return 0;
    }
    if (chip == 1) {
        *value = BK1080_ReadRegister(&c62_bk1080, reg);
        return 0;
    }
    return -1;
}

int pc_proto_platformRegWrite(int chip, uint8_t reg, uint16_t value)
{
    if (chip == 0) {
        bk4819_writeReg(&c62_bk4819, reg, value);
        return 0;
    }
    if (chip == 1) {
        BK1080_WriteRegister(&c62_bk1080, reg, value);
        return 0;
    }
    return -1;
}

int pc_proto_platformMem(uint32_t addr, uint8_t *buf, uint16_t len)
{
    /* Only the SRAM and the PSRAM window: the flash and the peripherals are
     * not handed over, so a bad address cannot stall a bus. */
    const uint32_t sramStart = 0x20000000u, sramEnd = 0x20040000u;
    const uint32_t psramStart = 0x30000000u, psramEnd = 0x30400000u;
    uint32_t end = addr + len;

    bool ok = (addr >= sramStart && end <= sramEnd &&
               end >= addr) ||
              (addr >= psramStart && end <= psramEnd && end >= addr);
    if (!ok)
        return -1;
    memcpy(buf, (const void *)(uintptr_t)addr, len);
    return 0;
}

/*
 * RX audio capture.
 *
 * The audio the codec worker receives is 8 kHz s16 in blocks; the capture keeps
 * a copy in PSRAM so a 2 s window costs no internal RAM, which this radio does
 * not have to spare.  The read path copies out of it, chunk by chunk.
 */
#define CAP_MAX 16000
static int16_t capBuffer[CAP_MAX] __attribute__((section(".psram_section")));
static pcProtoCapture_t cap;

int pc_proto_platformCapture(int op, pcProtoCapture_t *st, uint8_t *buf,
                             uint32_t off, uint32_t count)
{
    switch (op) {
        case PC_CAP_OP_START:
            cap.capacity = st->capacity > CAP_MAX ? CAP_MAX : st->capacity;
            cap.written = 0;
            cap.read = 0;
            cap.rate = 48000; /* the rate the codec worker delivers */
            cap.overflow = 0;
            cap.running = 1;
            break;
        case PC_CAP_OP_STOP:
            cap.running = 0;
            break;
        case PC_CAP_OP_STATUS:
            *st = cap;
            return 0;
        case PC_CAP_OP_READ:
            if (off >= cap.written || off + count > cap.written)
                return -1;
            memcpy(buf, &capBuffer[off], (size_t)count * 2);
            cap.read = off + count;
            return (int)count;
        default:
            return -1;
    }
    *st = cap;
    return 0;
}

/** Copy received audio into the capture, if one is running. Called by the
 *  codec worker with the interleaved frame, hence the stride. */
void pc_proto_capturePush(const int16_t *samples, size_t stride, size_t count)
{
    if (!cap.running)
        return;
    for (size_t i = 0; i < count && cap.written < cap.capacity; i++)
        capBuffer[cap.written++] = samples[i * stride];
    if (cap.written >= cap.capacity) {
        cap.overflow = 1;
        cap.running = 0;
    }
}

int pc_proto_platformBbDump(int op, uint8_t *state)
{
#ifdef C62_BASEBAND_DUMP
    bool active = baseband_dump_active();

    switch (op) {
        case PC_BB_OP_ARM:
            baseband_dump_init();
            baseband_dump_pause(false);
            break;
        case PC_BB_OP_DISARM:
            baseband_dump_pause(true);
            break;
        case PC_BB_OP_PAUSE:
            baseband_dump_pause(true);
            break;
        case PC_BB_OP_RESUME:
            baseband_dump_pause(false);
            break;
        default:
            break;
    }
    if (state != NULL) {
        if (!active && op != PC_BB_OP_ARM)
            *state = 0x80; /* this build has no dump */
        else
            *state = baseband_dump_paused() ? 2 : 1;
    }
#else
    (void)op;
    if (state != NULL)
        *state = 0x80; /* built without the dump */
#endif
    return 0;
}

int pc_proto_platformTxTrace(char *buf, size_t cap)
{
    struct txGuard *guard = rtx_getTxGuard();
    char trace[110];
    char summary[130];

    txg_formatTrace(guard, trace, sizeof(trace));
    txg_format(guard, summary, sizeof(summary));
    snprintf(buf, cap, "%s | %s | pk=%u wa=%d", trace, summary,
             (unsigned int)csk6codec_getTxPeak(),
             (int)csk6codec_getWorkerAge());
    return 0;
}

#endif /* CONFIG_C62_DEBUG_PROTO */

/* ------------------------------------------------------------------------- */
/* Thread                                                                    */
/* ------------------------------------------------------------------------- */

static void pcProtoMain(void *a, void *b, void *c)
{
    (void)a;
    (void)b;
    (void)c;

    uint8_t chunk[64];

    for (;;) {
        size_t n = 0;
        while (n < sizeof(chunk) &&
               (sys_read32(UART_BASE + UART_LSR) & LSR_DR)) {
            chunk[n++] = (uint8_t)sys_read32(UART_BASE + UART_RBR);
        }
        if (n > 0) {
            uint32_t frames = pc_proto_rx(&proto, chunk, n);
            if (frames > 0) {
                framesSeen += frames;
                lastFrameMs = k_uptime_get_32();
            }
            /* A client that stops talking gives the console back. */
            if (pc_proto_linked(&proto) &&
                (k_uptime_get_32() - lastFrameMs) > PC_PROTO_IDLE_MS) {
                pc_proto_reset(&proto);
            }
        }

        /* Lines captured by the hook, in order, from the thread that owns the
         * protocol instance: no lock needed on the protocol side. */
        for (;;) {
            char line[LINE_MAX];
            unsigned int key = irq_lock();
            if (lineCount == 0) {
                irq_unlock(key);
                break;
            }
            size_t len = lineLen[lineTail];
            memcpy(line, lineRing[lineTail], len);
            line[lineLen[lineTail]] = '\0';
            lineTail = (uint8_t)((lineTail + 1) % LINE_RING);
            lineCount--;
            irq_unlock(key);

            lineFeed(line);
        }

        pc_proto_tick(&proto, k_uptime_get_32());
        k_usleep(POLL_US);
    }
}

void pc_proto_c62_init(void)
{
    if (started)
        return;
    started = true;

    pc_proto_init(&proto, &port);
    lastFrameMs = k_uptime_get_32();

    /* GPIOA_15 carries the PTT button and UART2 RXD: the programming cable
     * uses it as RX, so PTT is not available in this build (as with the
     * remote control). */
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, 15, CSK_IOMUX_FUNC_ALTER3);
    while (sys_read32(UART_BASE + UART_LSR) & LSR_DR)
        (void)sys_read32(UART_BASE + UART_RBR);

    prevPrintkHook = __printk_hook_get();
    __printk_hook_install(logHook);
    __stdout_hook_install(logHook);

    k_thread_create(&thread, thread_stack, K_THREAD_STACK_SIZEOF(thread_stack),
                    pcProtoMain, NULL, NULL, NULL, THREAD_PRIORITY, 0,
                    K_NO_WAIT);
    k_thread_name_set(&thread, "pcproto");
}
