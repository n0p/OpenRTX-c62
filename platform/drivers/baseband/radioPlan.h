/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_PLAN_H
#define RADIO_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <rtx/rtx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The chip a plan is written to; the driver's struct, declared and not included
 * so that this header stays free of the driver's own includes. */
struct BK4819;

/**
 * The BK4819 configuration of every C62 mode, as data.
 *
 * The radio driver used to describe each mode by calling the chip's setters in
 * an order of its own, spread over radio_setOpmode(), radio_enableRx(),
 * radio_updateConfiguration(), radio_enableAfOutput() and the spectrum
 * analyser.  None of that could be read without the radio in the hand, and a
 * real defect hid in it: DMR ran the 25 kHz channel mode with the 9 kHz IF
 * filter while the stock firmware runs its 4FSK at 12.5 kHz with 4.5 kHz
 * filters (docs/RADIO_CONFIG.md).
 *
 * Here a mode is a list of register writes -- {register, mask, value}, plus the
 * few decisions that are not writes -- produced by a pure function.  It depends
 * on neither Zephyr, the target's hwconfig.h nor the driver: the host test in
 * tests/unit/regplan.cpp builds every mode's plan and compares it, write by
 * write, with the sequence the pre-refactor driver issued.
 *
 * The driver does not call the setters for these any more: it walks the plan
 * (the applyPlan() of radio_C62.cpp), so what the chip is asked for is decided
 * here and nowhere else.
 */

/**
 * One register write.
 *
 * mask == 0xffff is a whole-register write (bk4819_writeReg()), anything else
 * is a read-modify-write of the masked bits (bk4819_maskSetRegister()):
 * the chip ends up holding (current & ~mask) | (value & mask).  The two are not
 * interchangeable on the bus -- a masked write reads the register back first --
 * so the plan keeps them apart.
 */
struct radioPlanWrite {
    uint8_t reg;    ///< Register address
    uint16_t mask;  ///< Bits written, 0xffff for the whole register
    uint16_t value; ///< Value of those bits
};

/// Longest plan any mode produces: 17 writes, for M17 and DMR.
#define RADIO_PLAN_MAX_WRITES 20

/**
 * AF output selector of REG_47<11:8>, the values of enum bk4819_afOutput.
 *
 * Repeated here so that this header stays free of the driver's includes; the
 * host test asserts that the two enumerations agree.
 */
enum radioPlanAf {
    RADIO_PLAN_AF_MUTE = 0,   ///< Muted
    RADIO_PLAN_AF_FM = 1,     ///< FM demodulator
    RADIO_PLAN_AF_AM = 7,     ///< AM detector (undocumented value)
    RADIO_PLAN_AF_BYPASS = 9, ///< Raw discriminator, for the digital modes
};

/**
 * Which side of the radio a plan describes.
 *
 * A mode change writes both sides at once, so RADIO_PLAN_BOTH is what
 * radio_setOpmode() asks for.  RADIO_PLAN_TX is what keying writes, and it is
 * empty for every mode: the transmit configuration is applied when the mode is
 * selected, not when the PA comes up.
 */
enum radioPlanDir {
    RADIO_PLAN_RX = 0,  ///< Receive side only
    RADIO_PLAN_TX = 1,  ///< Transmit side only
    RADIO_PLAN_BOTH = 2 ///< Both at once, as a mode change does
};

/**
 * Which part of a mode's configuration a plan describes, one per call site in
 * the driver.
 */
enum radioPlanStage {
    RADIO_PLAN_STAGE_MODE,     ///< radio_setOpmode(): the mode's profile
    RADIO_PLAN_STAGE_RX,       ///< radio_enableRx(): the receive path
    RADIO_PLAN_STAGE_UPDATE,   ///< radio_updateConfiguration()
    RADIO_PLAN_STAGE_AF,       ///< radio_enableAfOutput()
    RADIO_PLAN_STAGE_SPECTRUM, ///< radio_spectrumSweep(true)
};

/**
 * What the plan asks for that is not a register write.
 *
 * These are the other half of a mode's configuration: the value the driver
 * keeps in c62_rx_reg43 for the baseband dump, whether the DSP wants the raw
 * discriminator rather than demodulated audio, whether the airband mode
 * installs its audio AGC, and whether there is an amplitude modulator at all.
 */
struct radioPlanKnobs {
    uint16_t rxReg43;  ///< Mirror of REG_43 for the dump, 0: not updated
    uint8_t rxAfGain1; ///< Initial REG_48<11:10>, the AM AGC's first step
    bool flat;         ///< The DSP takes the raw discriminator
    bool amAudio;      ///< Install the AM audio AGC on the RX path
    bool amTx;         ///< There is an amplitude modulator on the PA
};

/*
 * Airband amplitude modulation (CONFIG_C62_AIR_TX): the carrier power the mode
 * asks for and the linear window of the PA bias around it, in percent of the
 * duty that produces the carrier.  docs/TX_AIRBAND.md has where the three
 * numbers come from and the bench procedure that replaces them.
 */
#define RADIO_PLAN_AM_CARRIER_MW 1000U
#define RADIO_PLAN_AM_DUTY_MIN_PCT 10
#define RADIO_PLAN_AM_DUTY_MAX_PCT 190

/**
 * One plan: the writes, in the order they must reach the chip, and the knobs.
 */
struct radioPlan {
    enum opmode mode;          ///< Mode the plan is for
    enum bandwidth bandwidth;  ///< Channel spacing it was built for
    enum radioPlanDir dir;     ///< Side it was built for
    enum radioPlanStage stage; ///< Part of the configuration
    uint8_t count;             ///< Writes in `write`
    struct radioPlanWrite write[RADIO_PLAN_MAX_WRITES];
    struct radioPlanKnobs knobs;
};

/**
 * Compile-time and runtime options a plan is built with.
 *
 * They are passed in rather than read from the build's macros so that every
 * variant is reachable from the host test: the DMR 12.5 kHz setting the defect
 * above was about is one of them.
 */
struct radioPlanOptions {
    bool wideRx;            ///< C62_WIDE_RX: M17 in the 25 kHz channel mode
    bool dmr12k5;           ///< C62_DMR_12K5: DMR in the 12.5 kHz channel mode
    bool tetraIfDcFilter;   ///< C62_TETRA_IF_DC_FILTER: keep REG_7E<2:0>
    bool spectrumAgcFix;    ///< CONFIG_C62_SPECTRUM_AGC_FIX: REG_7E<15>
    bool airTx;             ///< CONFIG_C62_AIR_TX: the PA has an AM modulator
    uint16_t fmTxDevNarrow; ///< FM deviation word, 6.25 and 12.5 kHz channels
    uint16_t fmTxDevWide;   ///< FM deviation word, 25 kHz channels
};

/**
 * Default options: the plain build, no experimental setting, and the deviation
 * word bk4819_init() leaves in REG_40.
 *
 * @param opt: options to fill.
 */
void radioPlan_defaultOptions(struct radioPlanOptions *opt);

/**
 * Build a plan.
 *
 * @param plan: plan to fill, fully overwritten.
 * @param stage: which part of the mode's configuration is wanted.
 * @param mode: operating mode.
 * @param bandwidth: channel spacing of the configuration.
 * @param dir: RX, TX or both.
 * @param opt: build options, may be NULL for the defaults.
 */
void radioPlan_build(struct radioPlan *plan, enum radioPlanStage stage,
                     enum opmode mode, enum bandwidth bandwidth,
                     enum radioPlanDir dir, const struct radioPlanOptions *opt);

/**
 * Whether the DSP takes the raw discriminator for this mode.
 *
 * The digital modes all do: their demodulators slice the discriminator
 * themselves, and any audio filtering, de-emphasis or compander in the chip
 * would shape the constellation they are trying to read.
 *
 * @param mode: operating mode.
 * @return true for the flat, DC-coupled receive path.
 */
bool radioPlan_isFlat(enum opmode mode);

/**
 * The AF output selector a mode opens its audio path with.
 *
 * @param mode: operating mode.
 * @return value of REG_47<11:8>, one of enum radioPlanAf; MUTE for broadcast
 *         FM, whose audio does not come from the BK4819 at all.
 */
enum radioPlanAf radioPlan_afOutput(enum opmode mode);

/**
 * Name of a mode, for the tests and the documentation.
 *
 * @param mode: operating mode.
 * @return short, static name.
 */
const char *radioPlan_modeName(enum opmode mode);

/**
 * Walk a plan: issue its writes, in order, to the chip.
 *
 * This is the only place a plan becomes bus traffic.  A masked write reads the
 * register back first, a whole write does not, so the plan's mask decides which
 * of the driver's two write functions is used: the sequence the chip sees is
 * the plan, byte for byte.
 *
 * @param plan: plan to apply.
 * @param dev: BK4819 to write to.
 */
void radioPlan_apply(const struct radioPlan *plan, const struct BK4819 *dev);

/**
 * Render a plan as text, one write per line, for a failure message or a bench
 * log.  It prints exactly what is walked, so the diff between two plans reads.
 *
 * @param plan: plan to render.
 * @param buf: destination buffer.
 * @param len: its size.
 * @return buf.
 */
char *radioPlan_format(const struct radioPlan *plan, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_PLAN_H */
