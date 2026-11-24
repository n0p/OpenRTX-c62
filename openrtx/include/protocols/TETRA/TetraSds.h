/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_SDS_H
#define OPENRTX_TETRA_SDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/TETRA/SdsHist.h"
#include "protocols/TETRA/SdsMsg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode a short data event and, if it carries something worth showing,
 * store it as the newest message. Called from the libtetra callback, i.e.
 * from the receiver thread: it only touches RAM and a mutex.
 *
 * @param s: short data event, valid during the callback.
 * @return 0 if stored, -1 if the event carried nothing to show.
 */
int tetraSds_onEvent(const struct tetra_sds *s);

/** @return number of stored messages, 0..SDS_HIST_LEN. */
int tetraSds_count(void);

/** @return the i-th message, 0 is the newest, NULL if out of range. */
const struct sds_hist_entry *tetraSds_get(int i);

/** @return number of messages not shown yet. */
int tetraSds_unread(void);

/** Clear the unread flag of every message. */
void tetraSds_markRead(void);

/**
 * Line for the main screen: pages through the unread messages, one every
 * TETRASDS_PAGE_MS, then stops on the newest.
 *
 * @param out: destination buffer.
 * @param len: size of the buffer.
 * @param tick: uptime in ms.
 * @return pixel-independent line contents, empty if there is nothing to show.
 */
const char *tetraSds_line(char *out, size_t len, long long tick);

/**
 * Write the message record to nonvolatile memory if it changed and the
 * debounce time has passed. Called from a non real-time task.
 */
void tetraSds_task(void);

/**
 * Read the message record from nonvolatile memory. Called once at start-up
 * and when entering the TETRA mode.
 */
void tetraSds_load(void);

/** @return true if a write is pending (diagnostics). */
bool tetraSds_dirty(void);

/**
 * Drop every message, in RAM and in the pending record.
 */
void tetraSds_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_SDS_H */
