/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Post-mortem recorder, C62 half: the supervisor thread, the flash sector, the
 * thread walk and the hooks the rest of the firmware calls.
 *
 * The portable half (core/blackbox.c) owns the format and the detector; this
 * file owns everything that needs the kernel:
 *
 *  - a thread of its own, at a priority above the transport one, that touches
 *    no mutex, no queue and no allocator, so it keeps running when the threads
 *    it watches are wedged behind a lock.  This is why the supervisor is not
 *    the main thread: the main thread is one of the things that can be stuck,
 *    and a detector that dies with the thing it watches detects nothing.
 *  - the thread walk: name, priority, state, stack high-water mark and the
 *    stacked PC/LR, read out of the switch-out frame the kernel left on the
 *    thread's own stack.
 *  - the flash sector (storage_partition + 0x6000, docs/BLACKBOX.md), the
 *    boot-time read, print and invalidate, and the copy kept in RAM for the
 *    debug protocol.
 *  - the fault handler: a crash that is not a stall still gets a record,
 *    because "it rebooted and said nothing" is the other half of the mystery.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/devicetree.h>

#include "core/blackbox.h"
#include "core/state.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "rtx/rtx.h"

/**
 * The record's own sector inside the storage partition.
 *
 * The partition's map (docs/BLACKBOX.md, docs/MESSAGING.md):
 *   0x0000 settings, 0x1000 VFO, 0x2000/0x3000 TETRA SDS, 0x4000/0x5000
 *   messages.  0x6000 is the first free sector, and taking one sector leaves
 *   0x7000-0x9FFFF (612 KiB) free for whatever comes next.
 */
#define BB_AREA_OFFSET 0x6000U
#define BB_SECTOR_SIZE 0x1000U

/** One pass of the supervisor every this long: 20 samples inside a 2 s stall. */
#define BB_PERIOD_MS 100

/**
 * Supervisor stack.  It holds the record being built (516 B) plus whatever the
 * flash write path uses, so 2 KiB is not enough margin; 3 KiB is ~1.5 times the
 * measured worst case.
 */
#define BB_SUP_STACK 3072

/**
 * Higher than the transport thread (6) so the supervisor still runs when the
 * transport is the thing that is stuck, and preemptible so it never delays an
 * audio or radio deadline by more than a few microseconds.
 */
#define BB_SUP_PRIO 2

/** Start it after the kernel and the drivers are up, but before the UI. */
#define BB_SUP_DELAY_MS 300

/* Code lives in the application image in flash, with a small part in ITCM. */
#define BB_CODE_BASE DT_REG_ADDR(DT_CHOSEN(zephyr_flash))
#define BB_CODE_SIZE CONFIG_FLASH_LOAD_SIZE
#define BB_ITCM_BASE DT_REG_ADDR(DT_CHOSEN(zephyr_itcm))
#define BB_ITCM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_itcm))

static const struct flash_area *bbArea;
static uint8_t bbLast[BB_RECORD_SIZE];
static bool bbHaveLast;

static volatile const void *bbFaultThread;
static volatile uint32_t bbFaultPc;
static volatile uint32_t bbFaultLr;
static volatile bool bbInFatal;

/* ------------------------------------------------------------------------- */
/* Flash sector                                                              */
/* ------------------------------------------------------------------------- */

static int areaOpen(void)
{
    if (bbArea != NULL)
        return 0;

    if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &bbArea) < 0) {
        bbArea = NULL;
        return -1;
    }

    return 0;
}

static int areaRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;

    if ((areaOpen() != 0) || ((off + len) > BB_SECTOR_SIZE))
        return -1;

    return (flash_area_read(bbArea, BB_AREA_OFFSET + off, buf, len) < 0) ? -1 :
                                                                           0;
}

/**
 * Two kinds of write, and the difference matters: a whole record erases the
 * sector first, the invalidation word does not.  Erasing on the invalidation
 * would cost an erase and a rewrite of the sector at every boot.
 */
static int areaWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    (void)ctx;

    if ((areaOpen() != 0) || ((off + len) > BB_SECTOR_SIZE))
        return -1;

    if ((off == BB_OFF_VALID) && (len == 4)) {
        /* 1 -> 0 over the write block: no erase, and the record is dead. */
        uint32_t zero = 0;

        if (flash_area_write(bbArea, BB_AREA_OFFSET + off, &zero, 4) < 0)
            return -1;

        return 0;
    }

    if (flash_area_erase(bbArea, BB_AREA_OFFSET, BB_SECTOR_SIZE) < 0)
        return -1;

    if (flash_area_write(bbArea, BB_AREA_OFFSET + off, buf, len) < 0)
        return -1;

    return 0;
}

static const bbFlashIo_t bbIo = {
    .read = areaRead,
    .write = areaWrite,
    .ctx = NULL,
};

/* ------------------------------------------------------------------------- */
/* Snapshot: radio state and the thread walk                                 */
/* ------------------------------------------------------------------------- */

static bool inImage(uint32_t addr, uint32_t base, uint32_t size)
{
    return (addr >= base) && (addr < (base + size));
}

/**
 * Can @p addr be code?
 *
 * A stacked value is only reported when it falls inside the application image
 * or the ITCM: a thread that was switched out while its frame was half built
 * otherwise yields a plausible-looking number that is really a buffer address,
 * and a post-mortem that lies is worse than one that says zero.
 */
static bool looksLikeCode(uint32_t addr)
{
    addr &= ~1U; /* Thumb: the low bit is not part of the address */

    return inImage(addr, BB_CODE_BASE, BB_CODE_SIZE)
        || inImage(addr, BB_ITCM_BASE, BB_ITCM_SIZE);
}

/**
 * Read PC and LR out of a thread's switch-out frame.
 *
 * On Cortex-M the hardware pushes {r0-r3, r12, lr, pc, xpsr} on the thread's
 * own stack when an exception is taken, and the context switch saves that PSP
 * in the thread control block -- the callee-saved registers go to the TCB, not
 * to the stack (arch/arm/core/aarch32/swap_helper.S).  So the frame is at PSP,
 * LR at +0x14 and PC at +0x18, for every thread but the one running, whose PSP
 * is not meaningful.
 */
static void stackFrame(const struct k_thread *t, uint32_t *pc, uint32_t *lr,
                       uint32_t *sp)
{
    *pc = 0;
    *lr = 0;
    *sp = 0;

#if defined(CONFIG_THREAD_STACK_INFO)
    if ((t->stack_info.start == 0) || (t->stack_info.size == 0))
        return;

    uint32_t psp = (uint32_t)t->callee_saved.psp;
    uint32_t base = (uint32_t)t->stack_info.start;
    uint32_t end = base + (uint32_t)t->stack_info.size;

    if ((psp < base) || ((psp + 0x20U) > end))
        return;

    *sp = psp;

    const volatile uint32_t *frame = (const volatile uint32_t *)psp;

    if (looksLikeCode(frame[5]))
        *lr = frame[5];
    if (looksLikeCode(frame[6]))
        *pc = frame[6];
#else
    (void)t;
#endif
}

static void threadVisit(const struct k_thread *t, void *user)
{
    bbSnapshot_t *s = (bbSnapshot_t *)user;

    if (s->threadCount >= BB_MAX_THREADS)
        return;

    const uint16_t idx = s->threadCount;
    char *name = s->threads[idx].name;

    memset(name, 0, sizeof(s->threads[idx].name));

#if defined(CONFIG_THREAD_NAME)
    {
        const char *n = k_thread_name_get((k_tid_t)t);
        if (n != NULL)
            strncpy(name, n, sizeof(s->threads[idx].name) - 1);
    }
#endif

    s->threads[idx].prio = t->base.prio;
    s->threads[idx].state = t->base.thread_state;
    s->threads[idx].role = bb_threadRole((const void *)t);

    uint32_t pc = 0;
    uint32_t lr = 0;
    uint32_t sp = 0;
    stackFrame(t, &pc, &lr, &sp);

    /*
     * The faulting thread is not switched out: its frame is the one the fault
     * handler was given, and the kernel's copy of the PSP points at whatever it
     * was doing before.  The exception frame wins.
     */
    if ((const void *)t == bbFaultThread) {
        pc = bbFaultPc;
        lr = bbFaultLr;
    }

    s->threads[idx].pc = pc;
    s->threads[idx].lr = lr;
    s->threads[idx].sp = sp;

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
    {
        size_t unused = 0;
        if (k_thread_stack_space_get(t, &unused) == 0) {
            s->threads[idx].stackFree = (uint16_t)unused;
            s->threads[idx].stackSize = (uint16_t)t->stack_info.size;
        }
    }
#endif

    s->threadCount++;
}

void bb_platformSnapshot(bbSnapshot_t *s)
{
    memset(s, 0, sizeof(*s));

    rtxStatus_t st = rtx_getCurrentStatus();
    enum opstatus radioStatus = radio_getStatus();

    s->rtxMode = st.opMode;
    s->uiMode = state.channel.mode;
    s->rxFreq = (uint32_t)st.rxFrequency;
    s->txFreq = (uint32_t)st.txFrequency;
    s->rtxFlags = (uint16_t)(((radioStatus == RX) ? 0x01U : 0U)
                             | ((radioStatus == TX) ? 0x02U : 0U)
                             | ((st.opStatus == TX) ? 0x04U : 0U));

    /* The CSK6 port does not implement the hwinfo driver: zero, not a guess. */
    s->resetCause = 0;

#if defined(CONFIG_THREAD_MONITOR)
    k_thread_foreach(threadVisit, s);
#endif
}

const void *bb_platformCurrentThread(void)
{
    return (const void *)k_current_get();
}

uint32_t bb_platformNowMs(void)
{
    return k_uptime_get_32();
}

int bb_platformPersist(const uint8_t *rec, size_t len)
{
    return areaWrite(NULL, 0, rec, len);
}

void bb_platformReboot(void)
{
    /* Same path platform_terminate() uses: the SoC reset clears the
     * peripherals too, so the next boot finds the chip where it left it. */
    sys_reboot(SYS_REBOOT_COLD);
}

/* ------------------------------------------------------------------------- */
/* Supervisor thread                                                         */
/* ------------------------------------------------------------------------- */

static void bbSupervisor(void *p1, void *p2, void *p3)
{
    (void)p1;
    (void)p2;
    (void)p3;

    /* Arm only now: during start-up the threads the record watches do not
     * exist yet, and a detector that fires before the radio is up is noise. */
    bb_resume();

    for (;;) {
        k_sleep(K_MSEC(BB_PERIOD_MS));

        uint32_t now = k_uptime_get_32();

        /*
         * The watchdog is fed from here as well as from the device thread, and
         * that is the point: the supervisor is the only thread that cannot be
         * waiting on anything the firmware could be stuck behind.  When it
         * detects a hang it writes the record and reboots before the watchdog
         * window closes; when *it* stops running, nothing feeds the watchdog
         * and the hardware does what it does today.
         */
        if (!bb_supervise(now))
            platform_wdtFeed();
    }
}

K_THREAD_DEFINE(bb_supervisor, BB_SUP_STACK, bbSupervisor, NULL, NULL, NULL,
                BB_SUP_PRIO, 0, BB_SUP_DELAY_MS);

/* ------------------------------------------------------------------------- */
/* Fault path                                                                */
/* ------------------------------------------------------------------------- */

/* The kernel's weak hook (kernel/include/kernel_arch_interface.h), which the
 * CSK6 fatal handler ends in.  Declared here rather than by including kernel
 * internals. */
void arch_system_halt(unsigned int reason);

/**
 * The exception frame the hardware pushed when the fault was taken.
 *
 * On Cortex-M a fault from a thread stacks {r0-r3, r12, lr, pc, xpsr} on the
 * thread's PSP, and handler mode runs on the MSP, so during the fault handler
 * the PSP still points at that frame.  If the value is not inside the current
 * thread's stack the frame was not on it (a fault inside an interrupt, or a
 * thread that had not started) and nothing is reported: a wrong PC in a
 * post-mortem is worse than no PC.
 */
static void faultFrame(volatile uint32_t *pc, volatile uint32_t *lr)
{
    *pc = 0;
    *lr = 0;

    uint32_t psp;
    __asm__ volatile("mrs %0, psp" : "=r"(psp));

#if defined(CONFIG_THREAD_STACK_INFO)
    const struct k_thread *self = k_current_get();
    uint32_t base = (uint32_t)self->stack_info.start;
    uint32_t size = (uint32_t)self->stack_info.size;

    if ((base == 0) || (size == 0))
        return;
    if ((psp < base) || ((psp + 0x20U) > (base + size)))
        return;

    const volatile uint32_t *frame = (const volatile uint32_t *)psp;

    if (looksLikeCode(frame[6]))
        *pc = frame[6];
    if (looksLikeCode(frame[5]))
        *lr = frame[5];
#else
    (void)psp;
#endif
}

/**
 * Where every fatal path ends.
 *
 * The CSK6 SoC port installs its own k_sys_fatal_error_handler
 * (csk/soc/arm/csk6/fatal.c): it prints a backtrace and then calls
 * arch_system_halt(), the kernel's weak "stop this CPU" hook.  That hook is
 * the one seam the application can take without patching a module, and it is a
 * complete one: a fault, an assert, a stack overflow and a panic all reach it.
 *
 * The default implementation spins with interrupts locked and leaves the radio
 * dead until the volume knob.  This one writes the record first, then reboots
 * into receive -- the same recovery as a detected stall, so a crash is one
 * reboot and not a dead radio.
 *
 * The supervisor is suspended before the write, so that its watchdog feed
 * stops: if this path wedges -- a fault inside the flash driver is a real
 * possibility -- the hardware watchdog still resets the radio, and the record
 * it will show is the one from the previous crash.
 */
void arch_system_halt(unsigned int reason)
{
    if (!bbInFatal) {
        bbInFatal = true;
        bb_suspend();

        bbFaultThread = (const void *)k_current_get();
        faultFrame(&bbFaultPc, &bbFaultLr);

        bb_mark(BB_M_HANG);
        bb_capture(BB_REASON_FATAL, (uint32_t)reason, k_uptime_get_32());
    }

    sys_reboot(SYS_REBOOT_COLD);

    /*
     * Only reached when the SoC did not take the reset.  Interrupts are locked
     * here (z_fatal_error took them), so the hardware watchdog is the only way
     * out: spin, and do not sleep -- a watchdog reset is not an interrupt and
     * would never wake an idle core.
     */
    for (;;) {
        __asm__ volatile("nop");
    }
}

/* ------------------------------------------------------------------------- */
/* Boot-time read, and the copy the debug protocol serves                    */
/* ------------------------------------------------------------------------- */

static void printLine(void *ctx, const char *line)
{
    (void)ctx;
    printk("%s\n", line);
}

void bb_c62_init(void)
{
    bbHaveLast = false;

    if (bb_recordLoad(&bbIo, bbLast, sizeof(bbLast)) != 0)
        return;

    bbHaveLast = true;

    /*
     * Straight out of the UART, before the PC protocol takes the console over:
     * these lines are the first thing a captured boot shows, and a crash dump
     * that only exists inside a 4-line log ring is a crash dump nobody sees.
     */
    printk("\n=== C62 post-mortem from the previous boot ===\n");
    bb_textDump(bbLast, sizeof(bbLast), printLine, NULL);
    printk("=== end of post-mortem ===\n\n");

    /* Read once and once only: the magic, the CRC and the validity word all
     * stay on flash, and the record is served from RAM from here on. */
    bb_recordInvalidate(&bbIo);
}

bool bb_c62_haveRecord(void)
{
    return bbHaveLast;
}

int bb_c62_record(uint32_t off, uint8_t *buf, uint16_t cap, uint16_t *total)
{
    if (!bbHaveLast || (buf == NULL) || (total == NULL))
        return -1;
    if (off >= BB_RECORD_SIZE)
        return -1;

    uint32_t len = BB_RECORD_SIZE - off;
    if (len > cap)
        len = cap;

    memcpy(buf, &bbLast[off], len);
    *total = BB_RECORD_SIZE;

    return (int)len;
}

int bb_c62_summary(char *buf, size_t cap)
{
    if (!bbHaveLast || (buf == NULL) || (cap == 0))
        return -1;

    const uint8_t *u = &bbLast[BB_OFF_UPTIME];
    uint32_t uptime = (uint32_t)u[0] | ((uint32_t)u[1] << 8)
                    | ((uint32_t)u[2] << 16) | ((uint32_t)u[3] << 24);
    const uint8_t *m = &bbLast[BB_OFF_RTXMODE];
    uint16_t mode = (uint16_t)((uint16_t)m[0] | ((uint16_t)m[1] << 8));

    int n = snprintf(buf, cap, "post-mortem: %s at %u ms, mode %s",
                     bb_reasonName(bbLast[BB_OFF_REASON]), (unsigned)uptime,
                     bb_modeName(mode));

    return (n < 0) ? -1 : 0;
}

/*
 * The debug protocol's view of the record (docs/PC_PROTO.md, PC_MSG_DBG_BLACKBOX).
 * The record is grabbed at boot and invalidated then, so this serves the RAM
 * copy: it works with the cable only, after the reboot, with the flash sector
 * already free for the next crash.
 */
#ifdef C62_PC_PROTO
int pc_proto_platformBlackbox(uint32_t off, uint8_t *buf, uint16_t cap,
                              uint16_t *total)
{
    return bb_c62_record(off, buf, cap, total);
}
#endif
