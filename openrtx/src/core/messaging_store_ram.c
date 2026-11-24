/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file messaging_store_ram.c
 * @brief Fallback message store: RAM only, gone at the next start.
 *
 * The firmware backend of the C62 (messaging_store_zephyr.c) keeps the record
 * in a pair of alternating flash sectors; every other target -- the Linux
 * emulator, the host tests of the messaging screen, a build without
 * CONFIG_MESSAGING -- gets this one.  The functions are weak so a real backend
 * in the same image overrides them without a link error, the same way the
 * platform hooks of the channel and contact stores work.
 */

#include <string.h>

#include "core/messaging_store.h"

static uint8_t ramStore[MSG_STORE_MAX];
static size_t ramLen;

__attribute__((weak)) size_t messaging_store_size(void)
{
    return ramLen;
}

__attribute__((weak)) size_t messaging_store_read(uint8_t *buf, size_t cap)
{
    if ((ramLen == 0) || (ramLen > cap))
        return 0;
    memcpy(buf, ramStore, ramLen);
    return ramLen;
}

__attribute__((weak)) bool messaging_store_write(const uint8_t *buf, size_t len)
{
    if ((buf == NULL) || (len == 0) || (len > sizeof(ramStore)))
        return false;
    memcpy(ramStore, buf, len);
    ramLen = len;
    return true;
}

__attribute__((weak)) void messaging_store_clear(void)
{
    ramLen = 0;
}
