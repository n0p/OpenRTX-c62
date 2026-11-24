/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TXMODEL_H
#define TXMODEL_H

#include <stdint.h>

#include "protocols/YSF/YsfFrame.h"

/**
 * Build one V/D mode 2 frame that carries a text message block (bn = 2).
 *
 * @param fn: field index inside the message.
 * @param ft: index of the last field.
 * @param text10: exactly 10 characters of payload.
 * @param frame: receives the 480 dibits.
 */
void txmodel_vd2_text(uint8_t fn, uint8_t ft, const char *text10,
                      struct ysfFrame *frame);

#endif /* TXMODEL_H */
