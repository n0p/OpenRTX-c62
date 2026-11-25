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
#include "core/port_ptt.h"
#include "core/psram_state.h"
#include "core/settings.h"
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
#define LSR_OE BIT(1)
#define LSR_PE BIT(2)
#define LSR_FE BIT(3)
#define LSR_BI BIT(4)
#define LSR_THRE BIT(5)

#define THREAD_PRIORITY 6
#define THREAD_STACK 4096
#define POLL_US 1000

/** No pass of the loop for this long is a stall, not a slow frame. */
#define PROTO_STALL_MS 2000

/** One stall line every this long: the log shares the cable with the client. */
#define PROTO_STALL_REPORT_MS 5000

/** A link with no valid frame for this long goes away and frees the log. */
#define PC_PROTO_IDLE_MS 15000

/** Log lines buffered here, and the longest one kept. */
#define LINE_RING 4
#define LINE_MAX 128

static pcProto_t proto PSRAM_STATE;
static struct k_thread thread;
static K_THREAD_STACK_DEFINE(thread_stack, THREAD_STACK);
static bool started;
static uint32_t lastFrameMs;

/*
 * Liveness of the transport thread, in ordinary .bss.
 *
 * The thread is the only thing that can answer a frame, so "is it still
 * running?" is a question the rest of the firmware has to be able to ask: a
 * radio whose screen and keys work while the CPS gets silence is
 * indistinguishable from a broken cable, and the last time it happened (a
 * stored channel applied at boot) there was no fault, no log line and no way
 * to tell where it had stopped.  loopSeq advances once per pass of the loop;
 * pc_proto_c62_supervise(), called from the main thread's watchdog feed,
 * reports a stall and dumps what the transport last saw, which is also the
 * first thing a hardware session needs.  rxBytes is the other half of it: if
 * bytes arrive and no frame comes out of them the problem is the framing, and
 * if nothing arrives at all it is the pad, the cable or the client.
 */
static volatile uint32_t loopSeq;
static volatile uint32_t loopMs;
static volatile uint32_t rxBytes;
static volatile bool     stallReported;
static uint32_t          stallLastMs;

/* Log capture: the hook appends characters here, the transport thread turns
 * complete lines into pc_proto_logPush() calls.  Guarded by irq_lock() because
 * the hook runs in whatever context produced the line. */
static char lineRing[LINE_RING][LINE_MAX] PSRAM_STATE;
static uint8_t lineLen[LINE_RING];
static uint8_t lineHead;
static uint8_t lineTail;
static uint8_t lineCount;

static int (*prevPrintkHook)(int c);
#ifdef C62_BASEBAND_DUMP
static bool dumpPaused;
#endif

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
extern void *__printk_get_hook(void);

/* ------------------------------------------------------------------------- */
/* Platform hooks of the protocol                                            */
/* ------------------------------------------------------------------------- */

int pc_proto_platformStatus(pcProtoStatus_t *out)
{
    /*
     * No lock here, on purpose (docs/PC_PROTO.md, "The transport never waits").
     *
     * This runs in the transport thread, and it used to take state_mutex.  The
     * UI thread holds that same mutex across ui_updateFSM() and the state task
     * across the battery read, so a slow holder -- or one that blocks, which is
     * what applying a stored channel at boot made possible -- parked the only
     * thread that can answer a frame: the screen kept working, the keys kept
     * working, the log kept coming out, and the CPS got nothing, ever, with no
     * fault to show for it.  A status is a set of aligned words read to make a
     * picture; it does not need a transaction, and a value that is one tick old
     * is still the right answer.  vfo_search_service.c reads state the same way
     * and for the same reason.
     */
    out->rxFreq = state.channel.rx_frequency;
    out->txFreq = state_txFrequency();
    out->mode = state.channel.mode;
    out->channelIdx = (state.tuner_mode == VFO) ? 0xFFFF : state.channel_index;
    out->batteryMv = state.v_bat;
    out->txState = (uint8_t)state.rtxStatus;

    /*
     * The power the radio would key with and, when it is none or the driver
     * refused, why.  A PTT that does nothing must be explainable from the PC
     * (docs/TX_SAFETY.md).
     */
    out->txPower = (uint16_t)state_txPowerMw();
    const char *reason;
    if (out->txPower == 0) {
        reason = "no tx power";
    } else {
        /* What the gate said about the last press, and then what the driver
         * said about the last keying: the first is why it did not try, the
         * second is why the try came to nothing. */
        reason = rtx_keyingVetoReason();
        if (reason == NULL || strcmp(reason, "none") == 0)
            reason = radio_txRefusedReason();
        if (reason == NULL)
            reason = "none";
    }
    snprintf(out->txReason, sizeof(out->txReason), "%s", reason);

    if (state.tuner_mode != VFO) {
        size_t n = 0;
        while (n + 1 < sizeof(out->channelName) &&
               state.channel.name[n] != '\0') {
            out->channelName[n] = state.channel.name[n];
            n++;
        }
        out->channelName[n] = '\0';
    }

    out->rssi = rtx_getRssi();
    out->squelchOpen = rtx_rxSquelchOpen() ? 1 : 0;

    /*
     * The accessory port, docs/ACCESSORY.md.  The mode is what the operator
     * chose, and portLost is the byte count the transport has thrown away
     * because the speaker-mic PTT was holding the very line it reads: a client
     * whose requests went unanswered while somebody keyed has the number here.
     */
    out->portMode = c62_portMode();
    out->portLost = c62_portPttState()->lost;
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

/*
 * The transmit clock is reported in both builds: a production image has no
 * debug namespace, but the answer is three loads and it is what a bench asks
 * for when a radio comes back hot (docs/TX_SAFETY.md).
 */
int pc_proto_platformTxWatch(uint16_t *seconds, const char **reason)
{
    struct txWatch *watch = rtx_getTxWatch();

    if (seconds != NULL)
        *seconds = txw_airSeconds(watch, k_uptime_get_32());
    if (reason != NULL)
        *reason = rtx_txWatchReason();
    return 0;
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
     * enumerating the rest needs CONFIG_THREAD_MONITOR, and the call itself
     * only exists when CONFIG_INIT_STACKS fills the stacks at boot (that is
     * what gives the high-water mark).  Without it the field is absent and the
     * client shows nothing, which is honest: there is no number to show. */
#if defined(CONFIG_THREAD_STACK_INFO)
    {
        /*
         * Bytes never touched, measured from the top of the stack down to the
         * last non-zero word.  The stacks live in .bss, which the kernel
         * zeroes, so this is the same high-water mark
         * k_thread_stack_space_get() gives -- and it does not need
         * CONFIG_INIT_STACKS, which this build does not set.  The number is an
         * estimate: a frame that wrote zeroes over zeroes is invisible, which
         * is the standard limitation of the technique.
         */
        extern struct k_thread z_main_thread;
        struct k_thread *list[2] = { &z_main_thread, &thread };
        const char *names[2] = { "main", "pcproto" };

        for (int i = 0; i < 2 && out->taskCount < 8; i++) {
            uintptr_t start = list[i]->stack_info.start;
            size_t size = list[i]->stack_info.size;
            const uint8_t *base = (const uint8_t *)start;
            size_t unused = size;

            if (start == 0 || size == 0 || size > 65535)
                continue;
            /* Skip the 8-byte sentinel Zephyr reserves at the very top. */
            for (size_t n = size; n > 8; n--) {
                if (base[n - 1] != 0) {
                    unused = size - n;
                    break;
                }
            }
            pcProtoTaskStack_t *t = &out->tasks[out->taskCount++];
            snprintf(t->name, sizeof(t->name), "%s", names[i]);
            t->unused = (uint16_t)unused;
            t->size = (uint16_t)size;
        }
    }
#endif
    out->agc = 0xFF;

    /* The transmit side of the same picture (docs/TX_SAFETY.md). */
    out->txPower = (uint16_t)state_txPowerMw();
    {
        const char *reason = out->txPower == 0 ? "no tx power"
                                              : radio_txRefusedReason();
        if (reason == NULL || strcmp(reason, "none") == 0) {
            const char *veto = rtx_keyingVetoReason();
            reason = (veto != NULL && strcmp(veto, "none") != 0) ? veto
                                                                : "none";
        }
        snprintf(out->txReason, sizeof(out->txReason), "%s", reason);
    }

    /* And the accessory port (docs/ACCESSORY.md): who owns the pad, whether the
     * switch is down at this instant and what that has cost the serial line. */
    {
        const portPttState_t *ptt = c62_portPttState();

        out->portMode = c62_portMode();
        out->portPtt = port_ptt_pressed(ptt) ? 1 : 0;
        out->portLost = ptt->lost;
    }
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
        case PC_CAP_OP_START_LIVE: /* the worker fills it either way */
            cap.capacity = st->capacity > CAP_MAX ? CAP_MAX : st->capacity;
            cap.written = 0;
            cap.read = 0;
            cap.rate = 48000; /* the rate the codec worker delivers */
            cap.overflow = 0;
            /* The bench needs the capture on a time line: the tick of sample
             * zero and the shape of the blocks that filled it.  A capture
             * whose blocks are not all the same length cannot claim to be
             * contiguous, and blockLen says so with 0. */
            cap.t0Ms = (uint32_t)k_uptime_get();
            cap.blocks = 0;
            cap.blockLen = 0;
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
    if (cap.blocks == 0)
        cap.blockLen = (uint16_t)count;
    else if (cap.blockLen != (uint16_t)count)
        cap.blockLen = 0; /* a gap of unknown size: not contiguous */
    cap.blocks++;
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
    struct txWatch *watch = rtx_getTxWatch();
    struct txWatch *barrier = radio_txWatch();
    char trace[110];
    char summary[130];

    txg_formatTrace(guard, trace, sizeof(trace));
    txg_format(guard, summary, sizeof(summary));
    /*
     * The live line carries both clocks: the one that ends the transmission
     * (air, at the RTX limit) and the driver's own, which is what actually
     * keeps the PA from staying keyed if this one never runs. "cut=%u" of the
     * barrier is the number that must stay at zero.
     */
    snprintf(buf, cap,
             "%s | %s | tx=%us/%s air=%lums cut=%lu/%lu | pk=%u wa=%d",
             trace, summary,
             (unsigned int)txw_airSeconds(watch, k_uptime_get_32()),
             rtx_txWatchReason(),
             (unsigned long)txw_airMs(watch, k_uptime_get_32()),
             (unsigned long)watch->cuts, (unsigned long)barrier->cuts,
             (unsigned int)csk6codec_getTxPeak(),
             (int)csk6codec_getWorkerAge());
    return 0;
}

#else /* !CONFIG_C62_DEBUG_PROTO */

/*
 * A production build has no capture, but the codec worker calls the hook
 * unconditionally (it is compiled with C62_PC_PROTO): the stub keeps the call
 * and costs four instructions, and the 32 kB of PSRAM the buffer would take
 * are not there at all.
 */
void pc_proto_capturePush(const int16_t *samples, size_t stride, size_t count)
{
    (void)samples;
    (void)stride;
    (void)count;
}

#endif /* CONFIG_C62_DEBUG_PROTO */

/* ------------------------------------------------------------------------- */
/* Thread                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * The accessory port (docs/ACCESSORY.md).
 *
 * The pad at the jack is UART2 RXD and the speaker-mic PTT at once, and the
 * operator chooses who owns it.  The mode lives in the settings, so it is read
 * here -- once per pass, 1 ms -- instead of being pushed in from the menu: a
 * change applies on the next pass without the UI having to know that this file
 * exists, and a value that arrives from the CPS lands the same way.
 *
 * In "Auto" the pad stays muxed to the UART and its input is also sampled as a
 * GPIO.  A low level that lasts more than PORT_PTT_HOLD_MS is a finger on the
 * switch (a 0x00 byte holds the line 87 us, a short break a couple of
 * milliseconds); while that lasts, the bytes and the framing errors the UART
 * collects are drained, counted and thrown away, and the half frame the parser
 * had is dropped.  On release the parser waits for the next end delimiter and
 * everything is normal again.  The cost, and it is real, is that the frames the
 * client sends while the operator keys are lost -- the counter in the status
 * says how many.
 */
static volatile uint32_t rxLost;
static uint32_t lostAtPress;
static int appliedMode = -1;

static uint32_t uartTakeLineErrors(void)
{
    /*
     * Reading the line status clears the error bits on this UART, which is the
     * only way to acknowledge them: overrun, parity, framing and break are
     * counted as one event per read, which is what "the switch was holding the
     * line when a byte was on its way" looks like from the outside.
     */
    uint32_t lsr = sys_read32(UART_BASE + UART_LSR);
    uint32_t errors = 0;

    if (lsr & LSR_OE)
        errors++;
    if (lsr & LSR_PE)
        errors++;
    if (lsr & LSR_FE)
        errors++;
    if (lsr & LSR_BI)
        errors++;
    return errors;
}

static void portApplyMode(uint8_t mode)
{
    const portPttPlan_t plan = port_ptt_plan(mode);

    /* The owner of the pad is the point of the setting: UART2 RXD or a plain
     * GPIO with its pull-up.  This also resets the detector. */
    c62_portPadToUart(plan.padIsUart);

    if (!plan.useProtocol) {
        /*
         * Nothing can arrive any more.  Close the session instead of leaving a
         * client that thinks it is linked: it will time out either way, but the
         * log says why, and the console gets the log back.
         */
        pc_proto_reset(&proto);
        rxLost = 0;
        lostAtPress = 0;
    }

    /* After the reset, so the line reaches the console even when a client was
     * linked a moment ago. */
    printk("port: mode %s, pad %s, PTT %s\n", settings_portModeName(mode),
           plan.padIsUart ? "UART RX" : "GPIO",
           plan.useDetector ? "sustained low > 30 ms"
                            : (plan.useButton ? "button" : "not read"));
}

static void pcProtoMain(void *a, void *b, void *c)
{
    (void)a;
    (void)b;
    (void)c;

    uint8_t chunk[64];

    for (;;) {
        loopSeq++;
        loopMs = k_uptime_get_32();

        /* Who owns the pad on this pass, and did that just change? */
        const uint8_t mode = c62_portMode();
        if ((int)mode != appliedMode) {
            portApplyMode(mode);
            appliedMode = (int)mode;
        }
        const portPttPlan_t plan = port_ptt_plan(mode);
        portPttState_t *ptt = c62_portPttState();

        /*
         * Sample the accessory switch every millisecond -- that is what makes
         * the 30 ms rule work -- and act on its edges exactly once: the frame
         * the parser was building at the moment of the press is garbage, and
         * the counters of the press are reported on release.
         */
        bool held = false;
        if (plan.useDetector) {
            const bool changed = c62_portPttPoll();

            held = port_ptt_pressed(ptt);

            if (changed && held) {
                lostAtPress = ptt->lost;
                /* The line went low under the parser: drop the half frame and
                 * wait for the next end delimiter (pc_proto_rxResync). */
                pc_proto_rxResync(&proto);
                printk("port: accessory PTT down, UART discarded until "
                       "release\n");
            } else if (changed) {
                printk("port: accessory PTT up, held %lums, %lu bytes/%lu "
                       "errors dropped (%lu since boot)\n",
                       (unsigned long)ptt->lastPressMs,
                       (unsigned long)(ptt->lost - lostAtPress),
                       (unsigned long)ptt->glitches,
                       (unsigned long)ptt->lost);
                /* Whatever arrived as the key was released is already drained
                 * below; the parser starts from a delimiter again. */
                pc_proto_rxResync(&proto);
            }
        }

        size_t n = 0;
        if (plan.useProtocol && held) {
            /*
             * The pad is grounded by the switch: everything the UART sees is
             * break and zeros.  Drain it so the FIFO cannot overflow into the
             * first frame after the release, count it and parse none of it.
             */
            uint32_t dropped = 0;
            while (n < sizeof(chunk) &&
                   (sys_read32(UART_BASE + UART_LSR) & LSR_DR)) {
                (void)sys_read32(UART_BASE + UART_RBR);
                n++;
                dropped++;
            }
            dropped += uartTakeLineErrors();
            if (dropped > 0) {
                rxLost += dropped;
                rxBytes += (uint32_t)n;
                port_ptt_noteLost(ptt, dropped);
            }
        } else if (plan.useProtocol) {
            /* Framing errors with nobody pressing are the cable's business:
             * count them so the supervisor can tell an idle line from a noisy
             * one, and let the parser decide about the bytes. */
            uint32_t errors = uartTakeLineErrors();
            if (errors > 0) {
                rxLost += errors;
                port_ptt_noteLost(ptt, errors);
            }

            while (n < sizeof(chunk) &&
                   (sys_read32(UART_BASE + UART_LSR) & LSR_DR)) {
                chunk[n++] = (uint8_t)sys_read32(UART_BASE + UART_RBR);
            }
            if (n > 0) {
                rxBytes += (uint32_t)n;
                uint32_t frames = pc_proto_rx(&proto, chunk, n);
                /* Anything valid keeps the link alive; the protocol instance
                 * counts the frames itself. */
                if (frames > 0)
                    lastFrameMs = k_uptime_get_32();
                /* A client that stops talking gives the console back. */
                if (pc_proto_linked(&proto) &&
                    (k_uptime_get_32() - lastFrameMs) > PC_PROTO_IDLE_MS) {
                    pc_proto_reset(&proto);
                }
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

        /*
         * The baseband dump and the protocol share the UART: while a client is
         * linked the dump is paused, and it comes back when the client goes
         * away.  Its own trigger (PC_MSG_DBG_BB) is how a bench session starts
         * it now, instead of holding a key at power on.
         */
#ifdef C62_BASEBAND_DUMP
        bool up = pc_proto_linked(&proto);
        if (up && !dumpPaused) {
            baseband_dump_pause(true);
            dumpPaused = true;
        } else if (!up && dumpPaused) {
            baseband_dump_pause(false);
            dumpPaused = false;
        }
#endif

        pc_proto_tick(&proto, k_uptime_get_32());
        k_usleep(POLL_US);
    }
}

/**
 * Report a transport thread that has stopped passing through its loop.
 *
 * Called from the main thread, next to the watchdog feed, every 5 ms: it is
 * the one context that is known to run (it feeds the hardware watchdog), so it
 * is the right place to notice that the thread which answers the cable no
 * longer runs.  Two seconds without a pass is not a slow frame -- the loop
 * sleeps 1 ms between passes -- it is a thread parked somewhere it should never
 * park, and the numbers printed are what says where: bytes received (the pad
 * and the client are fine), frames parsed (the framing is fine), requests
 * dispatched (the handler is where it stopped) and the last error sent.
 *
 * Throttled to one line every five seconds, because the log shares the cable.
 */
void pc_proto_c62_supervise(void)
{
    if (!started)
        return;

    const uint32_t now = k_uptime_get_32();
    const uint32_t seen = loopMs;

    if ((now - seen) < PROTO_STALL_MS) {
        stallReported = false;
        return;
    }
    if (stallReported && (now - stallLastMs) < PROTO_STALL_REPORT_MS)
        return;

    stallReported = true;
    stallLastMs = now;
    printk("pcproto: STALLED %lums | loop=%lu rx=%lu lost=%lu frames=%lu "
           "req=%lu lastErr=%04x/%04x linked=%u mode=%s\n",
           (unsigned long)(now - seen), (unsigned long)loopSeq,
           (unsigned long)rxBytes, (unsigned long)rxLost,
           (unsigned long)proto.stats.frames,
           (unsigned long)proto.stats.requests,
           (unsigned)proto.stats.lastError, (unsigned)proto.stats.lastErrorMsg,
           (unsigned)pc_proto_linked(&proto),
           settings_portModeName(c62_portMode()));
}

void pc_proto_c62_init(void)
{
    if (started)
        return;
    started = true;

    pc_proto_init(&proto, &port);
    lastFrameMs = k_uptime_get_32();

    /*
     * GPIOA_15 is both the accessory PTT and UART2 RXD.  Who owns it is a
     * setting (enum portMode, docs/ACCESSORY.md) and the settings are not
     * loaded yet -- this runs before state_init() -- so the pad is routed to
     * the UART here, which is what the port needs to be a port at all, and the
     * loop below applies the operator's choice on its first pass, when the
     * record has been read.  The extra pass is also what prints the mode on the
     * boot log.
     */
    c62_portPadToUart(true);
    appliedMode = -1;
    while (sys_read32(UART_BASE + UART_LSR) & LSR_DR)
        (void)sys_read32(UART_BASE + UART_RBR);

    prevPrintkHook = (int (*)(int))__printk_get_hook();
    __printk_hook_install(logHook);
    __stdout_hook_install(logHook);

    k_thread_create(&thread, thread_stack, K_THREAD_STACK_SIZEOF(thread_stack),
                    pcProtoMain, NULL, NULL, NULL, THREAD_PRIORITY, 0,
                    K_NO_WAIT);
    k_thread_name_set(&thread, "pcproto");
}
