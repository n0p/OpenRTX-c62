/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file startup.c
 * @brief Minimal Cortex-M33 startup for QEMU mps2-an505 + semihosting.
 *
 * Only used by tests/qemu/run_qemu.sh, to run the vocoder on an emulated
 * Cortex-M33 (single-precision FPU, as in the CSK6011B).
 */

#include <stddef.h>
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(int argc, char **argv);

void Reset_Handler(void);
void Default_Handler(void);
static void exit_qemu(int code);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst;

    for (dst = &_sdata; dst < &_edata; dst++)
        *dst = *src++;
    for (dst = &_sbss; dst < &_ebss; dst++)
        *dst = 0;

    /* Enable CP10/CP11: mandatory with -mfloat-abi=hard, else the first
     * floating-point instruction faults with NOCP.  In the C62 firmware Zephyr
     * does this itself (CONFIG_FPU). */
    *(volatile uint32_t *)0xE000ED88u |= (0xFu << 20);
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    exit_qemu(main(0, NULL));
    for (;;) { }
}

void Default_Handler(void)
{
    for (;;) { }
}

/* Semihosting SYS_EXIT (ARMv7-M): stop the emulator. */
static void exit_qemu(int code)
{
    register int r0 __asm__("r0") = 0x18;
    register void *r1 __asm__("r1") = (void *)(0x20026); /* ADB_Stopped_ApplicationExit */

    (void)code;
    __asm__ volatile("bkpt 0xab" : : "r"(r0), "r"(r1) : "memory");
    for (;;) { }
}

/* Bump allocator (1 MiB above .bss, below the stack) for the semihosting
 * stdio of the test build only. */
extern uint32_t _ebss;
static char *heap_end;

char *_sbrk(ptrdiff_t incr)
{
    if (heap_end == NULL)
        heap_end = (char *)&_ebss;
    char *new_end = heap_end + incr;
    if (new_end > (char *)0x10380000)
        return (char *)-1;
    char *prev = heap_end;
    heap_end = new_end;
    return prev;
}

__attribute__((section(".vectors"), used))
void (*const g_vectors[])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, 0, 0, 0, 0,
    Default_Handler, Default_Handler, 0,
    Default_Handler,
};
