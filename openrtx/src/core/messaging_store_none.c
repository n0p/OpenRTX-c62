/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file messaging_store_none.c
 * @brief Fallback message store: nothing survives a restart.
 *
 * The firmware backend of the C62 (messaging_store_zephyr.c) keeps the record
 * in a pair of alternating flash sectors.  Every other target -- the Linux
 * emulator, the host build of the messaging screen, a Zephyr board without a
 * flash backend -- gets this one: the write is accepted and dropped, because
 * there is nowhere to put it, and the store comes up empty.
 *
 * It is deliberately empty: a RAM copy of the record would be 3,4 kB of SRAM
 * on every board to hold something that is already in messaging.c's own model,
 * and the model is what a screen reads during a session.  The functions are
 * weak so a real backend in the same image overrides them, the same way the
 * platform hooks of the channel and contact stores work.
 */

#include "core/messaging_store.h"

/** @return 0: this backend has never stored anything. */
__attribute__((weak)) size_t messaging_store_size(void)
{
    return 0;
}

/** @return 0: nothing was ever written. */
__attribute__((weak)) size_t messaging_store_read(uint8_t *buf, size_t cap)
{
    (void)buf;
    (void)cap;
    return 0;
}

/**
 * Accept the record and let it go: the caller's model is already up to date,
 * and a rejection would only make it retry a write that can never succeed.
 */
__attribute__((weak)) bool messaging_store_write(const uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
    return true;
}

__attribute__((weak)) void messaging_store_clear(void)
{
}
