/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Minimal Cortex-M33 startup for the QEMU mps2-an505 and semihosting.
 *
 * The bare-metal harnesses under tests/platform (c62_dpmr_phy_cost.c and the
 * others) are not a Zephyr or Miosix image: they are the firmware's own
 * protocol sources, compiled for an M33 with a single-precision FPU, and run
 * on QEMU to count instructions and to check that a capture still decodes.
 * This file brings the core up, enables the FPU the hard-float objects need,
 * and exits QEMU through the semihosting call when main() returns -- without
 * that the run would have to be killed by a timeout.
 */
#include <stdint.h>
#include <stddef.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);
static void exit_qemu(int code);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;

    while (dst < &_edata)
        *dst++ = *src++;

    for (dst = &_sbss; dst < &_ebss; dst++)
        *dst = 0;

    /* enable CP10/CP11 (FPU): required for -mfloat-abi=hard code */
    *(volatile uint32_t *)0xE000ED88u |= (0xFu << 20);
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    int rc = main();
    exit_qemu(rc);
    for (;;) { }
}

void Default_Handler(void)
{
    for (;;) { }
}

/* semihosting SYS_EXIT */
static void exit_qemu(int code)
{
    register int r0 __asm__("r0") = 0x18;
    register void *r1 __asm__("r1") = (void *)(0x20026); /* ApplicationExit */
    (void)code;
    __asm__ volatile("bkpt 0xab" : : "r"(r0), "r"(r1) : "memory");
    for (;;) { }
}

/* bump allocator: 1 MB heap above .bss, far below the stack */
extern uint32_t _ebss;
static char *heap_end;

char *_sbrk(ptrdiff_t incr)
{
    if (!heap_end)
        heap_end = (char *)&_ebss;

    char *new_end = heap_end + incr;

    if (new_end > (char *)0x10380000)
        return (char *)-1;

    char *prev = heap_end;
    heap_end = new_end;
    return prev;
}

char *sbrk_ptr(void) { return heap_end ? heap_end : (char *)&_ebss; }

__attribute__((section(".vectors"), used))
void (*const g_vectors[])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, 0, 0, 0, 0,
    Default_Handler, Default_Handler, 0,
    Default_Handler,
};
