/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/contact_store.h"
#include <string.h>
#include <errno.h>

#define CSTORE_MAGIC   0x54435443U /* "CTCT" */
#define CSTORE_VERSION 1

static cstore_bank_t bank;
static slot_store_t store;
static const slot_ops_t *port = NULL;
static bool ready = false;
static bool pending = false;

__attribute__((weak)) const slot_ops_t *cstore_platformOps(void)
{
    return NULL;
}

void cstore_bind(const slot_ops_t *ops)
{
    port = ops;
}

static void sanitize(cstore_contact_t *c)
{
    c->call[CSTORE_CALL_LEN - 1] = '\0';
    c->name[CSTORE_NAME_LEN - 1] = '\0';
    if (c->name[0] == '\0')
        memcpy(c->name, c->call, CSTORE_NAME_LEN);
}

int cstore_init(void)
{
    if (ready)
        return 0;

    if (port == NULL)
        port = cstore_platformOps();

    memset(&bank, 0, sizeof(bank));

    if (port == NULL) {
        /* No flash on this device: usable, but empty after a reset */
        ready = true;
        return -ENODEV;
    }

    int ret = slot_store_load(&store, port, CSTORE_MAGIC, CSTORE_VERSION,
                              &bank, sizeof(bank));
    if (ret != 0) {
        memset(&bank, 0, sizeof(bank));
    }

    if (bank.count > CSTORE_MAX_CONTACTS)
        bank.count = 0;

    for (uint16_t i = 0; i < bank.count; i++)
        sanitize(&bank.list[i]);

    ready = true;
    return 0;
}

int cstore_count(void)
{
    return ready ? (int)bank.count : 0;
}

int cstore_get(uint16_t idx, cstore_contact_t *out)
{
    if ((out == NULL) || !ready || idx >= bank.count)
        return -EINVAL;
    *out = bank.list[idx];
    return 0;
}

bool cstore_valid(const cstore_contact_t *c)
{
    if (c == NULL)
        return false;
    size_t len = strnlen(c->call, CSTORE_CALL_LEN);
    return (len > 0) && (len < CSTORE_CALL_LEN);
}

int cstore_add(const cstore_contact_t *c)
{
    if (!ready || !cstore_valid(c) || bank.count >= CSTORE_MAX_CONTACTS)
        return -EINVAL;

    bank.list[bank.count] = *c;
    sanitize(&bank.list[bank.count]);
    bank.count++;
    pending = true;
    return cstore_commit() == 0 ? (int)(bank.count - 1) : -EIO;
}

int cstore_set(uint16_t idx, const cstore_contact_t *c)
{
    if (!ready || !cstore_valid(c) || idx >= bank.count)
        return -EINVAL;

    bank.list[idx] = *c;
    sanitize(&bank.list[idx]);
    pending = true;
    return cstore_commit();
}

int cstore_delete(uint16_t idx)
{
    if (!ready || idx >= bank.count)
        return -EINVAL;

    for (uint16_t i = (uint16_t)(idx + 1); i < bank.count; i++)
        bank.list[i - 1] = bank.list[i];
    bank.count--;
    memset(&bank.list[bank.count], 0, sizeof(bank.list[0]));
    pending = true;
    return cstore_commit();
}

uint16_t cstore_findCall(const char *call)
{
    if (!ready || call == NULL)
        return CSTORE_NONE;

    for (uint16_t i = 0; i < bank.count; i++) {
        if (strncmp(bank.list[i].call, call, CSTORE_CALL_LEN) == 0)
            return i;
    }
    return CSTORE_NONE;
}

uint16_t cstore_findForMode(uint8_t mode, uint16_t from)
{
    if (!ready || bank.count == 0 || from >= bank.count)
        return CSTORE_NONE;

    for (uint16_t i = from; i < bank.count; i++) {
        if (bank.list[i].mode == mode)
            return i;
    }
    return CSTORE_NONE;
}

int cstore_commit(void)
{
    if (!ready || !pending)
        return 0;
    if (port == NULL)
        return -ENODEV;

    int ret = slot_store_save(&store, &bank, sizeof(bank));
    if (ret == 0)
        pending = false;
    return ret;
}

void cstore_forget(void)
{
    ready = false;
    pending = false;
    memset(&bank, 0, sizeof(bank));
}

cstore_bank_t *cstore_bank(void)
{
    return &bank;
}
