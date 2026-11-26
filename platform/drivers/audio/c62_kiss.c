/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * KISS transport over the C62's programming cable (C62_KISS_UART builds).
 *
 * The cable's host TX line arrives on GPIOA_15, which is UART2 RXD and also
 * the PTT button: this switches the pad to UART2 RXD, so PTT does not work in
 * these builds, exactly as with C62_REMOTE. The two options are mutually
 * exclusive because there is one pad.
 *
 * The protocol is binary here, so unlike the remote control (which shares the
 * line with the baseband dump's control packets) the firmware log cannot share
 * the port: this transport mutes it, the same hook the dump uses.
 *
 * When the PC protocol layer (pc_proto) lands, it binds its own kiss_port_t
 * with packet_service_bind() and this file is not compiled at all: that is the
 * one and only hook, see docs/KISS.md.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <IOMuxManager.h>
#include <hwconfig.h>

#include "core/packet_service.h"

#define UART_BASE DT_REG_ADDR(DT_CHOSEN(zephyr_console))
#define UART_RBR  0x00
#define UART_THR  0x00
#define UART_LSR  0x14
#define LSR_DR    BIT(0)
#define LSR_THRE  BIT(5)

/* The cable tops out at 115200 baud, which is 11.5 kB/s: a 330 byte frame is
 * 29 ms on the wire, so the KISS stream can never keep a 9600 baud channel
 * busy for long, but it can starve on long frames. Documented in docs/KISS.md. */
#define KISS_UART_BAUD 115200u

extern void __printk_hook_install(int (*fn)(int c));

static int discardChar(int c)
{
    (void)c;
    return c;
}

static size_t uartRead(uint8_t *buf, size_t cap, void *user)
{
    (void)user;
    size_t n = 0;

    while ((n < cap) && (sys_read32(UART_BASE + UART_LSR) & LSR_DR))
        buf[n++] = (uint8_t)sys_read32(UART_BASE + UART_RBR);

    return n;
}

static bool uartWrite(const uint8_t *buf, size_t len, void *user)
{
    (void)user;

    for (size_t i = 0; i < len; i++)
    {
        /* The console UART has no flow control: wait for room, with a bound so
         * a stuck host cannot hang the service thread forever */
        uint32_t spins = 0;
        while ((sys_read32(UART_BASE + UART_LSR) & LSR_THRE) == 0)
        {
            if (++spins > 200000u)
                return false;
            k_busy_wait(10);
        }
        sys_write32(buf[i], UART_BASE + UART_THR);
    }

    return true;
}

static const kiss_port_t uartPort =
{
    .read  = uartRead,
    .write = uartWrite,
    .user  = NULL
};

void kiss_uart_init(void)
{
    /* GPIOA_15 to UART2 RXD, as the remote control does */
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, 15, CSK_IOMUX_FUNC_ALTER3);

    /* KISS is binary: nothing else may write to this line */
    __printk_hook_install(discardChar);

    packet_service_bind(&uartPort);
}
