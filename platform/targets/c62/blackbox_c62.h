/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BLACKBOX_C62_H
#define BLACKBOX_C62_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file blackbox_c62.h
 * @brief Platform side of the post-mortem recorder (docs/BLACKBOX.md).
 *
 * Called once from platform_init_csk6(): reads the record the previous boot
 * left behind, prints it on the console and invalidates it.  The supervisor
 * thread and the fault handler are started by the kernel, not from here.
 */
void bb_c62_init(void);

/** True when a record from the previous boot was found and printed. */
bool bb_c62_haveRecord(void);

/**
 * Copy a chunk of the last post-mortem out of RAM.
 *
 * @param off: byte offset into the record.
 * @param buf: destination.
 * @param cap: bytes available.
 * @param total: receives BB_RECORD_SIZE.
 * @return bytes copied, or a negative value when there is no record.
 */
int bb_c62_record(uint32_t off, uint8_t *buf, uint16_t cap, uint16_t *total);

/** One line describing the record, for the debug protocol. */
int bb_c62_summary(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_C62_H */
