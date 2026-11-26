/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_MODE_STATE_H
#define UI_MODE_STATE_H

/**
 * @file ui_mode_state.h
 * @brief What the current mode has decoded, as the main screen shows it.
 *
 * The state of a receive mode is not a setting and does not belong in a menu:
 * the operator wants to read it while working the radio, so it is drawn on the
 * main screen, in one block that every mode fills the same way (docs/UI.md,
 * "El estado del modo"). This header is that block and the only place that
 * knows which getter of which protocol answers each field.
 *
 * The fields are pre-formatted text because every mode names its identifiers
 * differently (a colour code, a RAN, a NAC, a cell), and because a test has to
 * be able to check "SRCH" and "-" without a framebuffer:
 *
 *  - `id`    identifiers and the sync state: "CC1 TS2", "RAN12 VOX",
 *            "NAC293 VOX", "228-8889 LA 1", "EA5ABC>CQCQCQ", "SRCH".
 *  - `call`  source and destination of the call in progress, "" with none.
 *  - `qual`  the received-quality figure the mode publishes, "" if none.
 *  - `note`  secondary state that used to be a menu row: registration,
 *            data-unit type, slow-data text. "" when there is none.
 *  - `noteShort` the same in fewer characters, for the line where `note` does
 *            not fit beside the identifiers (the screen picks, it never
 *            overflows).
 *
 * Nothing decoded reads as "SRCH" or "-", never as a zero: a colour code of 0
 * and "no colour code decoded yet" are different things, and a screen that
 * prints 0 for the second one tells the operator a lie. tests/unit/
 * ui_mode_audit check the two states from the host stub.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest text of one field, in bytes, terminator included. */
#define UI_MODE_STATE_LEN 28

typedef struct
{
    bool sync;                        /**< The receiver is decoding this mode */
    bool call;                        /**< A call is in progress              */
    char id[UI_MODE_STATE_LEN];       /**< Identifiers, or "SRCH"             */
    char callTxt[UI_MODE_STATE_LEN];  /**< Source>destination, "" if none     */
    char qual[UI_MODE_STATE_LEN];     /**< Quality figure, "" when unpublished*/
    char note[UI_MODE_STATE_LEN];     /**< Registration/type/text, "" if none */
    char noteShort[UI_MODE_STATE_LEN];/**< Shorter note, used when note+id do
                                           not fit the line, "" if none      */
} ui_mode_state_t;

/**
 * Fill the block from the current mode's status.
 *
 * Safe from the UI thread: every getter it uses copies a snapshot (the
 * protocol status modules are written by the rtx thread and read with a struct
 * copy). Modes with no decoded state - FM, and the airband and broadcast FM
 * receivers, which publish their state through the rest of the screen - leave
 * the fields empty and the block draws nothing.
 *
 * @param out: destination, always fully written.
 */
void ui_mode_state_read(ui_mode_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* UI_MODE_STATE_H */
