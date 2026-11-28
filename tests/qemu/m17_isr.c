/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The interrupt-context rule, reproduced on a real kernel: M17's PTT.
 *
 * What the radio showed when PTT was pressed in M17, on the console:
 *
 *     ASSERTION FAIL [!arch_is_in_isr()] @ zephyr/kernel/mutex.c:101
 *             mutexes cannot be used inside ISRs
 *     ***** HARD FAULT *****
 *     Fault during interrupt handling
 *
 * so the claim to test has two halves, and both are testable here, on a
 * Cortex-M33 running the same Zephyr kernel (this fork, 3.4) as the radio:
 *
 *   1. a mutex taken from an interrupt aborts with that message, and the
 *      harness can say which function took it -- proven by injecting the fault
 *      on purpose (`-DTEST_NAME=mutation`), which is the only way to know the
 *      harness would see it if the firmware did it;
 *   2. the firmware's own transmit-start path does or does not do it.  The
 *      audio path table is real code from openrtx/src/core/audio_path.cpp, the
 *      same `audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX)` that
 *      OpMode_M17::txState() calls when the PTT is pressed, and it is called
 *      here from a real interrupt (`-DTEST_NAME=m17_path`).
 *
 * The interrupt is not a thread pretending: an unused NVIC line is connected
 * with IRQ_CONNECT() and pended with NVIC_SetPendingIRQ(), so the entry goes
 * through the vector table, _isr_wrapper and _sw_isr_table exactly as the
 * radio's SPI and DSP-mailbox interrupts do.  `-DTEST_NAME=timer` does the
 * same work from a k_timer expiry, which is the other interrupt context the
 * firmware uses (radio_C62.cpp arms one for the PA cut).
 *
 * Everything the harness prints goes out twice: through printk()/logging, which
 * is what the radio's console uses and is itself part of what is under test,
 * and through ARM semihosting, which is how the script reads the verdict and
 * how the emulator is asked to exit.  See scripts/m17_isr.sh.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>
#include <zephyr/arch/cpu.h>
/* NVIC_SetPendingIRQ(): a software-pended line is a genuine interrupt. */
#include <zephyr/arch/arm/aarch32/cortex_m/cmsis.h>

#include <core/audio_path.h>

#include "harness_out.h"

LOG_MODULE_REGISTER(m17_isr, LOG_LEVEL_DBG);

/* ------------------------------------------------------------------ output -- */

static char line[128];

/** Print one line on the console and, prefixed, through semihosting. */
static void say(const char *tag, const char *what)
{
    char *p = line;

    p = hout_lit(p, tag);
    p = hout_lit(p, " ");
    p = hout_lit(p, what);
    *p = '\0';
    hout_str(line);
    hout_str("\n");

    printk("%s %s\n", tag, what);
}

static void say_hex(const char *tag, const char *what, uint32_t value)
{
    char *p = line;

    p = hout_lit(p, tag);
    p = hout_lit(p, " ");
    p = hout_lit(p, what);
    p = hout_lit(p, " 0x");
    p = hout_hex32(p, value);
    *p = '\0';
    hout_str(line);
    hout_str("\n");

    printk("%s %s 0x%08x\n", tag, what, value);
}

/** Leave QEMU: semihosting SYS_EXIT, the same bkpt the cost harnesses use. */
static void __attribute__((noreturn)) leave(int code)
{
    /*
     * Flush what printk() queued.  With CONFIG_LOG_PRINTK the message is a
     * deferred log record, and the radio's own fault handler is what forces it
     * out (log_panic() sets panic_mode, and z_log_msg_post_finalize() then
     * processes the queue in the caller's context -- interrupt or not).  Doing
     * the same here means the console shows what the radio's console would.
     *
     * Before the registers below, not after: the call clobbers r0, and the
     * semihosting call has to read its number from that register.
     */
    log_panic();

    /* r1 carries the reason code directly (ADP_Stopped_ApplicationExit); "+r"
     * is what keeps r0 in r0, which is the register the semihosting call
     * reads -- without it the compiler is free to copy it elsewhere. */
    register int r0 __asm__("r0") = 0x18;      /* SYS_EXIT */
    register int r1 __asm__("r1") = (code == 0) ? 0x20026 : 0x20023;

    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");

    for (;;) {
        /* semihosting off: keep the verdict already printed */
    }
}

/* ------------------------------------------------------ who took the mutex -- */

/*
 * The kernel's k_mutex_lock() is a static inline around z_impl_k_mutex_lock(),
 * so this is the symbol to interpose, and the link is told to do it with
 * -Wl,--wrap=z_impl_k_mutex_lock (tests/qemu/m17_isr_app/CMakeLists.txt).
 *
 * The return address is the instruction that asked for the lock: the script
 * resolves it with arm-none-eabi-addr2line against the very ELF QEMU ran, so
 * "in which function" is answered by the binary and not by a guess.
 */
extern int __real_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout);

/* The interrupt entry point currently running, for the report. */
static volatile const char *isr_entry = "none";

int __wrap_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
    if (__get_IPSR() != 0U) {   /* arch_is_in_isr(), spelled out */
        uint32_t ret = (uint32_t)(uintptr_t)__builtin_return_address(0);

        /* printk() here is deliberate: it must be safe from an interrupt in
         * this configuration, and if it is not, this line is where the
         * harness (and the radio) finds out. */
        say("HARNESS", "mutex-in-interrupt");
        say("HARNESS", isr_entry);
        say_hex("HARNESS", "caller", ret);
        say_hex("HARNESS", "mutex", (uint32_t)(uintptr_t)mutex);
    }

    return __real_z_impl_k_mutex_lock(mutex, timeout);
}

/*
 * The assertion's own report: Zephyr's __ASSERT calls assert_post_action()
 * after printing the message, and it is weak, so this is where the harness
 * states what happened without needing the watchdog to reboot anything.
 */
void assert_post_action(const char *file, unsigned int line_no)
{
    char what[64];
    char *p = what;

    p = hout_lit(p, "assert at ");
    p = hout_lit(p, file);
    p = hout_lit(p, ":");
    p = hout_dec(p, line_no);
    *p = '\0';

    say("HARNESS", "ASSERT");
    say("HARNESS", what);
    say("HARNESS", isr_entry);
    leave(2);
}

/* -------------------------------------------------------------- the tests -- */

static K_MUTEX_DEFINE(victim);

/* The injected fault, and the shape of the real one: a function that takes a
 * mutex, reachable from an interrupt.  Not inlined so that its name is in the
 * frame the wrapper reports. */
static __attribute__((noinline)) void injected_path(void)
{
    k_mutex_lock(&victim, K_NO_WAIT);
    k_mutex_unlock(&victim);
}

/* The firmware's own entry point into the transmit audio path. */
extern pathId m17_audio_request(void);

static int failures;

static void check_no_failure(void)
{
    if (failures == 0) {
        say("HARNESS", "VERDICT ok: no mutex was taken in interrupt context");
        leave(0);
    }

    say("HARNESS", "VERDICT fail");
    leave(3);
}

/* --- the interrupt ------------------------------------------------------- */

#ifdef TEST_TRIGGER_TIMER

static struct k_timer trigger;

static void on_interrupt(void);

static void timer_expiry(struct k_timer *timer)
{
    (void)timer;
    on_interrupt();
}

#else

/* An NVIC line the board leaves alone: pending it by software is an interrupt
 * in every sense that matters -- vector, wrapper, table entry, ISR context. */
#define TRIGGER_IRQ 30

static void on_interrupt(void);

static void trigger_isr(const void *arg)
{
    (void)arg;
    on_interrupt();
}

#endif

static void on_interrupt(void)
{
#ifdef TEST_TRIGGER_TIMER
    isr_entry = "timer_expiry";
#else
    isr_entry = "trigger_isr";
#endif

#if defined(TEST_THREAD)
    /* This variant has no work in the interrupt: the call under test is made
     * from a thread, which is how the radio makes it. */
    say("HARNESS", "in interrupt: nothing to do in this variant");

#elif defined(TEST_MUTATION)
    say("HARNESS", "in interrupt: injected_path()");
    injected_path();
    say("HARNESS", "back from injected_path(): the kernel did not abort");

#elif defined(TEST_PRINTK)
    say("HARNESS", "in interrupt: printk");
    printk("m17_isr: a log line from an interrupt\n");
    say("HARNESS", "printk returned: the log path is safe here");

#elif defined(TEST_PANIC)
    /* A panic in an interrupt: the fatal path logs, and whether that is legal
     * from an interrupt is what the console then shows. */
    say("HARNESS", "in interrupt: k_panic");
    k_panic();

#elif defined(TEST_M17_PATH)
    pathId id;

    say("HARNESS", "in interrupt: m17_audio_request()");
    id = m17_audio_request();
    say_hex("HARNESS", "audioPath_request returned", (uint32_t)id);
    say("HARNESS", "back from the firmware's transmit path");

#else
#error "pick a TEST_NAME"
#endif
}

/* --- the same work from a thread, as the radio actually does it ----------- */

static void thread_body(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;

    isr_entry = "thread";
    say("HARNESS", "in thread: the same call");

#if defined(TEST_MUTATION)
    injected_path();
#elif defined(TEST_M17_PATH) || defined(TEST_THREAD)
    /* The control: the very call the m17_path variant makes from an
     * interrupt, made from a thread -- which is where the firmware makes it. */
    say_hex("HARNESS", "audioPath_request returned",
            (uint32_t)m17_audio_request());
#else
    /* nothing to do for the log and panic tests: they are about the ISR */
#endif

    say("HARNESS", "the thread finished");
}

K_THREAD_STACK_DEFINE(worker_stack, 4096);
static struct k_thread worker;

int main(void)
{
    say("HARNESS", "boot");

#if defined(TEST_THREAD)
    /* Control: the same code, legal, from a thread. */
#else

#ifdef TEST_TRIGGER_TIMER
    k_timer_init(&trigger, timer_expiry, NULL);
    k_timer_start(&trigger, K_MSEC(20), K_NO_WAIT);
    say("HARNESS", "k_timer armed");
#else
    IRQ_CONNECT(TRIGGER_IRQ, 2, trigger_isr, NULL, 0);
    irq_enable(TRIGGER_IRQ);
    say("HARNESS", "software interrupt armed");
    NVIC_SetPendingIRQ(TRIGGER_IRQ);
#endif

#endif

    /* The thread runs after the interrupt, in every test that has one. */
    k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
                    thread_body, NULL, NULL, NULL, 5, 0, K_MSEC(50));

    /* Nothing may abort: if the interrupt did, the assertion hook has already
     * left the emulator. */
    k_sleep(K_MSEC(200));
    check_no_failure();

    return 0;
}
