/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_FSK4_TX_H
#define OPENRTX_FSK4_TX_H

/**
 * @file Fsk4Tx.h
 * @brief The 4FSK baseband of the 2400 symbol/s modes (NXDN48, dPMR), the
 * modulator the DMR transmitter uses, with the symbol rate as a parameter.
 *
 * dmr_tx.c shapes DMR with a root raised cosine, alpha 0.2, ten samples per
 * symbol at 48 kHz (4800 symbols/s).  NXDN48 and dPMR run the same pulse shape
 * and the same four levels at half that rate -- 2400 symbols/s, twenty samples
 * per symbol at 48 kHz -- and both receivers (NxdnPhy.c, DpmrPhy.c) match them
 * with the same alpha 0.2 filter.  So the chain is the DMR one: build the RRC
 * from the same closed form, run the same zero-stuffed polyphase convolution,
 * scale it the same way and saturate to int16.  Only the number of samples per
 * symbol and the dibit-to-level table change, which is why this is a shared
 * module and not two copies of dmr_mod_dibits().
 *
 * The tap count is four symbols of span on each side, 8 * sps + 1 taps: 161 at
 * 20 sps (644 bytes of floats, .data because they are computed at init) or 81
 * at 10 sps, the DMR table.
 *
 * There is no envelope ramp here, exactly as in dmr_tx.c: the way out of a
 * transmission is a run of zero-amplitude symbols, which is what carries the
 * pulse tail past the last bit, and the PA gate of the radio
 * (radio_c62_gateStart()) is what ramps the carrier.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest tap table: 20 samples per symbol, four symbols of span each way. */
#define FSK4_TX_MAX_TAPS 161

typedef struct {
    float taps[FSK4_TX_MAX_TAPS];
    float hist[16];   ///< Last symbols, newest at histPos
    unsigned histPos;
    int sps;          ///< Samples per symbol
    int tapsN;        ///< Taps in use
    float gain;
    bool invert;
} fsk4_mod_t;

/**
 * @brief Initialise the modulator.
 *
 * @param mod modulator.
 * @param sps samples per symbol, 20 for the 2400 symbols/s modes.
 * @param gain output for a long run of symbols +1 (a run of +3 gives three
 *             times this).  Peaks are saturated to int16.
 * @param invert negate the output (for a transmitter that inverts).
 */
void fsk4Mod_init(fsk4_mod_t *mod, int sps, float gain, bool invert);

/**
 * @brief Modulate symbols.
 *
 * @param mod modulator.
 * @param sym symbol values in units of the inner level (+-1, +-3), or NULL for
 *            silence (no deviation: the gaps and the filter tail).
 * @param n number of symbols.
 * @param out n * mod->sps samples.
 */
void fsk4Mod_symbols(fsk4_mod_t *mod, const float *sym, int n, int16_t *out);

/**
 * @brief Modulate dibits through a four-entry level map.
 *
 * @param mod modulator.
 * @param dibits dibits to send, or NULL for silence.
 * @param map level of dibit 0..3 in units of the inner level.
 * @param n number of symbols.
 * @param out n * mod->sps samples.
 */
void fsk4Mod_dibits(fsk4_mod_t *mod, const uint8_t *dibits, const float map[4],
                    int n, int16_t *out);

/**
 * @brief Modulate raw levels (the frame sync of a mode is a level sequence,
 * not a dibit sequence).
 */
void fsk4Mod_levels(fsk4_mod_t *mod, const int8_t *levels, int n, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_FSK4_TX_H */
