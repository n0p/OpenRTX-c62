/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EMULATOR_H
#define EMULATOR_H

#include "interfaces/keyboard.h"
#include <stdbool.h>
#include <stdint.h>
#include "SDL2/SDL.h"

#ifndef CONFIG_SCREEN_WIDTH
#define CONFIG_SCREEN_WIDTH 160
#endif

#ifndef CONFIG_SCREEN_HEIGHT
#define CONFIG_SCREEN_HEIGHT 128
#endif

enum choices {
    VAL_RSSI = 1,
    VAL_BAT,
    VAL_MIC,
    VAL_VOL,
    VAL_CH,
    VAL_PTT,
    PRINT_STATE,
    EXIT
};

typedef struct {
    float RSSI;
    float vbat;
    float micLevel;
    float volumeLevel;
    float chSelector;
    bool PTTstatus;
    bool powerOff;
} emulator_state_t;

extern emulator_state_t emulator_state;

void emulator_start();

keyboard_t emulator_getKeys();

/**
 * Translate a key name ("UP", "GREEN", "P1", "2", "KNOB_LEFT") to its bit.
 *
 * Accepts the name with or without its KEY_ prefix, case insensitively, and
 * the physical names of this radio: GREEN, RED, P1, P2 and SIDE, which are
 * KEY_ENTER, KEY_ESC, KEY_F1, KEY_F2 and KEY_MONI.
 *
 * @return the keyboard_t bit, or 0 when the name is unknown.
 */
keyboard_t emulator_keyFromName(const char *name);

/*
 * The TETRA receiver state the host has no radio for (ui_mode_status.c).
 *
 * Nothing is injected by default: tetra_getStatus() answers "nothing decoded".
 * The simulator's `inject tetra-cell` command and the tests fill it, so the
 * "Cell uplink" row can be walked with a known uplink and with none.
 */
#ifdef __cplusplus
extern "C" {
#endif

void tetra_sim_clearCell(void);

void tetra_sim_setCell(uint16_t mcc, uint16_t mnc, uint8_t colourCode,
                       uint32_t dlHz, uint32_t ulHz);

void tetra_sim_setUplink(bool uplink, uint32_t ssi);

/*
 * Mobility management and a call in progress: two pieces of TETRA state that
 * used to be rows of the mode's page (REG, CALL) and are now on the main
 * screen. A receiver fills them from D-LOCATION-UPDATE-ACCEPT and the call
 * events; the host has no receiver, so a script says what it heard.
 */
void tetra_sim_setRegistration(bool accepted, uint8_t groups, uint8_t mmCause);

void tetra_sim_setCall(bool call, bool group, uint32_t dst, uint32_t talker,
                       uint16_t callId, bool voice, bool encrypted);

/*
 * The DMR receiver state the host has no radio for: sync, colour code,
 * timeslot, link quality and the call the last link control announced.
 * dmr_sim_clear() is what dmrStatus_clear() calls, so a test that wants
 * "nothing decoded" just clears the block.
 */
void dmr_sim_clear(void);

void dmr_sim_setStatus(bool sync, int cc, int ts, bool voice, unsigned evm,
                       bool idsValid, bool group, uint32_t src, uint32_t dst,
                       int lastType);

/* Sync, colour code and timeslot only: the state a locked receiver reports. */
void dmr_sim_setLan(bool sync, int cc, int ts);

/*
 * NXDN, P25 and D-STAR: defined by their own status modules under SIM_INJECT,
 * which the simulator build defines and the test builds do not (there the
 * modules are either the real engine or the honest "decodes nothing").
 */
void nxdnStatus_simSet(bool sync, int ran, bool voice, unsigned evm,
                       bool msgValid, unsigned source, unsigned dest,
                       unsigned type);

void p25Status_simSet(bool sync, bool voice, int nac, int duid, long source,
                      int talkgroup, unsigned imbe, unsigned imbeBad,
                      unsigned frames);

void dstarStatus_simSet(bool sync, bool voice, const char *my, const char *ur,
                        const char *rpt, int viterbi, const char *text);

#ifdef __cplusplus
}
#endif

/**
 * Queue one keyboard_t state for the firmware's key scan.
 *
 * The queue is consumed one entry per kbd_getKeys() call, so a press and a
 * release are two entries: `key`, then 0.
 *
 * @return false when the queue is full.
 */
bool emulator_keyEnqueue(keyboard_t keys);

/** Entries still waiting in the key queue. */
int emulator_keyPending(void);

#endif /* EMULATOR_H */
