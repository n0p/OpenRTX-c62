/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/baseband/radioPlan.h"
#include "drivers/baseband/BK4819.h"
#include "drivers/baseband/BK4819_priv.h"

#include <stdio.h>

/*
 * The BK4819 says 12.5 kHz channels are its mode 0, 6.25 kHz mode 1 and 25 kHz
 * mode 2; a channel spacing reaches it through those, never directly.
 *
 * RF filter 0 is 1.7 kHz, 3 is 3 kHz, 4 is 3.75 kHz and 7 is 4.5 kHz; in the
 * 25 kHz channel mode the chip doubles whichever one is selected, so the 4.5 kHz
 * filter is 9 kHz there.  From the REG_43 notes of the UV-K5 custom firmware:
 * https://github.com/egzumer/uv-k5-firmware-custom/blob/7607f0a4bd6203d1f06b70556fc1ce0d7399d6b3/driver/bk4819.c#L598-L601
 */
struct planBandwidth {
    uint8_t rfBw;     ///< RX RF filter, REG_43<14:12>
    uint8_t rfBwWeak; ///< Same, on weak signals, REG_43<11:9>
    uint8_t txLpf2Bw; ///< TX audio LPF2, REG_43<8:6>
    uint8_t channel;  ///< Channel mode, REG_43<5:4>
};

static const struct planBandwidth planBandwidths[] = {
    [BW_12_5] = { .rfBw = 4, .rfBwWeak = 0, .txLpf2Bw = 1, .channel = 0, },
    [BW_25]   = { .rfBw = 3, .rfBwWeak = 0, .txLpf2Bw = 0, .channel = 2, },
    [BW_6_25] = { .rfBw = 3, .rfBwWeak = 3, .txLpf2Bw = 1, .channel = 1, },
};

/*
 * Per-mode numbers, all of them from radio_C62.cpp before the plan existed.
 * Where each one comes from is in docs/RADIO_CONFIG.md.
 */

/* M17 on the A36Plus profile: https://wiki.m17foundation.org/index.php?title=A36Plus */
#define M17_TX_DEVIATION 0x582
#define M17_TX_LPF2_BW 0     /* 3 kHz */
#define M17_RX_AUDIO_GAIN1 1 /* -6 dB */
#define M17_RX_AUDIO_GAIN2 0x3f

/* ETSI DMR puts its outer symbols at +-1.944 kHz: 0x582 * 1944 / 2400 */
#define DMR_TX_DEVIATION 0x476
#define DMR_TX_LPF2_BW 0 /* 3 kHz */

/* The flat receive path of the digital modes and the AM detector's levels */
#define DIGI_RX_AUDIO_GAIN1 1 /* -6 dB */
#define DIGI_RX_AUDIO_GAIN2 0x3f
#define DIGI_MIC_GAIN 0
#define AM_RX_AUDIO_GAIN1 1 /* -6 dB, first step of the AM AGC */
#define AM_RX_AUDIO_GAIN2 0x3f

/* The REG_43 words the baseband dump reports for the modes that use them */
#define REG43_M17_NARROW 0x7808
#define REG43_M17_WIDE 0x7e28
#define REG43_DIGI_12K5 0x7e48 /* the stock firmware's 4FSK setting */
#define REG43_DIGI_25K 0x7e28

/* ------------------------------------------------------------------------- */

static void emitBandwidth(struct radioPlan *plan, enum bandwidth bandwidth);

/**
 * Append one write.
 *
 * RADIO_PLAN_MAX_WRITES is longer than any mode's plan, so the bound is a
 * guard against a future edit, not a condition: dropping a write silently would
 * be exactly the kind of defect this file exists to make impossible.
 */
static void emit(struct radioPlan *plan, uint8_t reg, uint16_t mask,
                 uint16_t value)
{
    if (plan->count >= RADIO_PLAN_MAX_WRITES)
        return;

    plan->write[plan->count].reg = reg;
    plan->write[plan->count].mask = mask;
    plan->write[plan->count].value = value;
    plan->count++;
}

/** Whole-register write, bk4819_writeReg() */
static void emitWhole(struct radioPlan *plan, uint8_t reg, uint16_t value)
{
    emit(plan, reg, 0xffff, value);
}

/** AF output selector, bk4819_setAfOutput() */
static void emitAfOutput(struct radioPlan *plan, enum radioPlanAf af)
{
    emit(plan, 0x47, BK4819_REG47_AF_OUTPUT_MASK, BK4819_REG47_AF_OUTPUT(af));
}

/**
 * The chip's FM demodulator profile, which every non-airband mode puts back
 * before its own: bk4819_setModulation(dev, true).
 *
 * The AM detector changes registers the others would otherwise inherit -- the
 * top of the RX AGC gain table (REG_13), the undocumented REG_4A, the AF
 * de-emphasis (REG_2B<8>) and the AF expander (REG_28) -- so leaving the
 * airband mode is not enough to get the FM path back.
 */
static void emitFmProfile(struct radioPlan *plan)
{
    emitAfOutput(plan, RADIO_PLAN_AF_FM);
    emitWhole(plan, 0x13, BK4819_REG13_AGC_TOP_FM);
    emitWhole(plan, 0x4A, BK4819_REG4A_RX_FM);
    emit(plan, 0x2B, BK4819_REG2B_RX_DEEMPHASIS_DISABLE, 0);
    emit(plan, 0x28, BK4819_REG28_EXPANDER_MASK, BK4819_REG28_EXPANDER_1_2);
}

/**
 * The AM detector profile: bk4819_setModulation(dev, false).
 *
 * No de-emphasis (AM carries none at the transmitter) and no expander (it would
 * undo part of what the audio AGC does).  The 300 Hz high-pass of REG_2B stays
 * as the FM path leaves it: it is what removes the DC of the detected carrier.
 */
static void emitAmProfile(struct radioPlan *plan)
{
    emitAfOutput(plan, RADIO_PLAN_AF_AM);
    emitWhole(plan, 0x13, BK4819_REG13_AGC_TOP_AM);
    emitWhole(plan, 0x4A, BK4819_REG4A_RX_AM);
    emit(plan, 0x2B, BK4819_REG2B_RX_DEEMPHASIS_DISABLE,
         BK4819_REG2B_RX_DEEMPHASIS_DISABLE);
    emit(plan, 0x28, BK4819_REG28_EXPANDER_MASK, BK4819_REG28_EXPANDER_OFF);
}

/**
 * The audio processing mode, bk4819_setAudioMode().
 *
 * Bypass is the flat, DC-coupled path the 4FSK and C4FM demodulators need: no
 * audio filters, no compander, no ALC, no TX DC filter, so the discriminator
 * reaches the DSP with its levels intact.  Voice puts back the values
 * bk4819_init() leaves, and with them the channel-dependent settings the mode
 * then owns (deviation, bandwidth, gains), which is why those follow here.
 */
static void emitAudioMode(struct radioPlan *plan, bool bypass)
{
    const uint16_t deemph = bypass ? BK4819_REG2B_AF_FILTERS_DISABLE : 0;
    const uint16_t dcf =
        bypass ? 0 :
                 (BK4819_REG7E_TX_DCF_BW(BK4819_DEFAULT_TX_DCF_BW)
                  | BK4819_REG7E_RX_DCF_BW(BK4819_DEFAULT_RX_DCF_BW));

    /* Order of audioModeFields[] in BK4819.c, which is the order on the bus */
    emit(plan, 0x2B, BK4819_REG2B_AF_FILTERS_DISABLE, deemph);
    emit(plan, 0x31, BK4819_REG31_COMPANDER | BK4819_REG31_VOX, 0);
    emit(plan, 0x47, BK4819_REG47_BYPASS, bypass ? BK4819_REG47_BYPASS : 0);
    emit(plan, 0x4B, BK4819_REG4B_ALC_DISABLE,
         bypass ? BK4819_REG4B_ALC_DISABLE : 0);
    emit(plan, 0x70, BK4819_REG70_TONE1_ENABLE | BK4819_REG70_TONE2_ENABLE, 0);
    emit(plan, 0x7E, BK4819_REG7E_TX_DCF_BW_MASK | BK4819_REG7E_RX_DCF_BW_MASK,
         dcf);

    if (bypass)
        return;

    /*
     * The voice profile always asks for the 12.5 kHz channel mode and for the
     * chip's own deviation word: a mode that wants another spacing or another
     * deviation says so right after, which is what every digital branch does.
     */
    emit(plan, 0x40,
         BK4819_REG40_TX_DEVIATION_ENABLE | BK4819_REG40_TX_DEVIATION_MASK,
         BK4819_REG40_TX_DEVIATION_ENABLE
             | BK4819_REG40_TX_DEVIATION(BK4819_DEFAULT_TX_DEVIATION));
    emitBandwidth(plan, BW_12_5);
    emit(plan, 0x48,
         BK4819_REG48_AF_RX_GAIN1_MASK | BK4819_REG48_AF_RX_GAIN2_MASK,
         BK4819_REG48_AF_RX_GAIN1(BK4819_DEFAULT_RX_AUDIO_GAIN1)
             | BK4819_REG48_AF_RX_GAIN2(BK4819_DEFAULT_RX_AUDIO_GAIN2));
    emit(plan, 0x7D, BK4819_REG7D_MIC_GAIN_MASK,
         BK4819_REG7D_MIC_GAIN(BK4819_DEFAULT_MIC_GAIN));
}

/** Channel mode and the three filters of REG_43, bk4819_setBandwidth() */
static void emitBandwidth(struct radioPlan *plan, enum bandwidth bandwidth)
{
    const struct planBandwidth *f = &planBandwidths[bandwidth];
    const uint16_t mask = BK4819_REG43_RF_BW_MASK | BK4819_REG43_RF_BW_WEAK_MASK
                        | BK4819_REG43_AF_TX_LPF2_BW_MASK
                        | BK4819_REG43_BW_MODE_MASK | BK4819_REG43_UNDOC3;
    const uint16_t value = BK4819_REG43_RF_BW(f->rfBw)
                         | BK4819_REG43_RF_BW_WEAK(f->rfBwWeak)
                         | BK4819_REG43_AF_TX_LPF2_BW(f->txLpf2Bw)
                         | BK4819_REG43_BW_MODE(f->channel)
                         | BK4819_REG43_UNDOC3;

    emit(plan, 0x43, mask, value);
}

/** The two RF filters alone, bk4819_setRfFilter() */
static void emitRfFilter(struct radioPlan *plan, uint8_t rfBw, uint8_t weak)
{
    emit(plan, 0x43, BK4819_REG43_RF_BW_MASK | BK4819_REG43_RF_BW_WEAK_MASK,
         BK4819_REG43_RF_BW(rfBw) | BK4819_REG43_RF_BW_WEAK(weak));
}

/** TX audio LPF2 alone, bk4819_setTxAudioFilter() */
static void emitTxAudioFilter(struct radioPlan *plan, uint8_t bw)
{
    emit(plan, 0x43, BK4819_REG43_AF_TX_LPF2_BW_MASK,
         BK4819_REG43_AF_TX_LPF2_BW(bw));
}

/** AF RX gains, bk4819_setRxAudioGain() */
static void emitRxAudioGain(struct radioPlan *plan, uint8_t gain1,
                            uint8_t gain2)
{
    emit(plan, 0x48,
         BK4819_REG48_AF_RX_GAIN1_MASK | BK4819_REG48_AF_RX_GAIN2_MASK,
         BK4819_REG48_AF_RX_GAIN1(gain1) | BK4819_REG48_AF_RX_GAIN2(gain2));
}

/** Microphone sensitivity, bk4819_setMicGain() */
static void emitMicGain(struct radioPlan *plan, uint8_t gain)
{
    emit(plan, 0x7D, BK4819_REG7D_MIC_GAIN_MASK, BK4819_REG7D_MIC_GAIN(gain));
}

/** TX deviation and its enable, bk4819_setTxDeviation(dev, true, word) */
static void emitTxDeviation(struct radioPlan *plan, uint16_t word)
{
    emit(plan, 0x40,
         BK4819_REG40_TX_DEVIATION_ENABLE | BK4819_REG40_TX_DEVIATION_MASK,
         BK4819_REG40_TX_DEVIATION_ENABLE | BK4819_REG40_TX_DEVIATION(word));
}

/**
 * The AM detector's channel filter, bk4819_setAmReceive().
 *
 * The AM detector has no bandwidth of its own: what limits it is the RF/IF
 * filter of REG_43, so a channel spacing is chosen by picking the filter that
 * fits it.  12.5 kHz mode with RF filter 0 is 1.7 kHz of IF, the stock
 * firmware's AM setting and narrow enough for the 8.33 kHz raster; 25 kHz mode
 * with RF filter 3 doubles 3 kHz to 6 kHz, which fits a 25 kHz AM channel.
 */
static void emitAmReceive(struct radioPlan *plan, bool wide)
{
    uint16_t reg43;

    if (wide)
        reg43 = BK4819_REG43_RF_BW(3) | BK4819_REG43_RF_BW_WEAK(3)
              | BK4819_REG43_AF_TX_LPF2_BW(0) | BK4819_REG43_BW_MODE(2)
              | BK4819_REG43_UNDOC3;
    else
        reg43 = BK4819_REG43_RF_BW(0) | BK4819_REG43_RF_BW_WEAK(0)
              | BK4819_REG43_AF_TX_LPF2_BW(1) | BK4819_REG43_BW_MODE(0)
              | BK4819_REG43_UNDOC3;

    emitWhole(plan, 0x43, reg43);
    emitAmProfile(plan);
}

/* ------------------------------------------------------------------------- */

void radioPlan_defaultOptions(struct radioPlanOptions *opt)
{
    /*
     * The configuration the C62 ships with: the wide M17 receive path
     * (C62_WIDE_RX) and the TETRA IF DC filter (C62_TETRA_IF_DC_FILTER) are the
     * defaults of the firmware's build options, DMR's 12.5 kHz mode
     * (C62_DMR_12K5), the fixed AGC of the analyser and the airband modulator
     * (C62_AIR_TX) are not.  The driver passes its macros explicitly anyway;
     * this is what a caller that has no opinion gets.
     */
    opt->wideRx = true;
    opt->dmr12k5 = false;
    opt->tetraIfDcFilter = true;
    opt->spectrumAgcFix = false;
    opt->airTx = false;
    opt->fmTxDevNarrow = BK4819_DEFAULT_TX_DEVIATION;
    opt->fmTxDevWide = BK4819_DEFAULT_TX_DEVIATION;
}

bool radioPlan_isFlat(enum opmode mode)
{
    switch (mode) {
        case OPMODE_M17:
        case OPMODE_DMR:
        case OPMODE_TETRA:
        case OPMODE_NXDN:
        case OPMODE_DPMR:
        case OPMODE_P25:
        case OPMODE_YSF:
        case OPMODE_DSTAR:
            return true;

        default:
            return false;
    }
}

enum radioPlanAf radioPlan_afOutput(enum opmode mode)
{
    if (mode == OPMODE_WFM_BC)
        return RADIO_PLAN_AF_MUTE; /* the tuner drives the amplifier */
    if (mode == OPMODE_AIRBAND)
        return RADIO_PLAN_AF_AM;
    if (radioPlan_isFlat(mode))
        return RADIO_PLAN_AF_BYPASS;

    return RADIO_PLAN_AF_FM;
}

const char *radioPlan_modeName(enum opmode mode)
{
    switch (mode) {
        case OPMODE_FM:
            return "FM";
        case OPMODE_DMR:
            return "DMR";
        case OPMODE_M17:
            return "M17";
        case OPMODE_TETRA:
            return "TETRA";
        case OPMODE_NXDN:
            return "NXDN";
        case OPMODE_DPMR:
            return "dPMR";
        case OPMODE_P25:
            return "P25";
        case OPMODE_YSF:
            return "YSF";
        case OPMODE_DSTAR:
            return "D-STAR";
        case OPMODE_AIRBAND:
            return "AIRBAND";
        case OPMODE_WFM_BC:
            return "WFM";
        default:
            return "NONE";
    }
}

/**
 * The mode's profile: everything radio_setOpmode() writes, in order.
 *
 * The digital modes share a shape -- flat audio path, 12.5 or 25 kHz channel
 * mode, their own IF filters, the deviation word and the two gains -- and differ
 * in the numbers, which is the whole point of the table being here: DMR at
 * 25 kHz was a defect, and here it is one line.
 */
static void buildModeProfile(struct radioPlan *plan, enum opmode mode,
                             enum bandwidth bandwidth,
                             const struct radioPlanOptions *opt)
{
    /*
     * Every mode but airband puts the FM demodulator profile back first: the
     * AM detector's REG_13, REG_4A, REG_2B<8> and REG_28 would otherwise be
     * inherited by a mode that has nothing to do with them.
     */
    if (mode != OPMODE_AIRBAND)
        emitFmProfile(plan);

    switch (mode) {
        case OPMODE_M17:
            emitAudioMode(plan, true);
            emitTxDeviation(plan, M17_TX_DEVIATION);
            if (opt->wideRx) {
                /* 25 kHz channel mode, the widest documented IF filters */
                emitBandwidth(plan, BW_25);
                emitRfFilter(plan, 7, 7);
                plan->knobs.rxReg43 = REG43_M17_WIDE;
            } else {
                /* 12.5 kHz mode, the M17 filters replace its own */
                emitBandwidth(plan, BW_12_5);
                emitRfFilter(plan, 7, 4);
                plan->knobs.rxReg43 = REG43_M17_NARROW;
            }
            emitTxAudioFilter(plan, M17_TX_LPF2_BW);
            emitRxAudioGain(plan, M17_RX_AUDIO_GAIN1, M17_RX_AUDIO_GAIN2);
            emitMicGain(plan, DIGI_MIC_GAIN);
            break;

        case OPMODE_DMR:
            /*
             * 4FSK at 12.5 kHz.  The stock firmware runs its own digital mode
             * in the 12.5 kHz channel mode with the 4.5 kHz filters (REG_43 =
             * 0x7e48); the 25 kHz mode doubles those to 9 kHz, which lets the
             * neighbouring channel into a 12.5 kHz one and shows up as a
             * dancing colour code on the air, never in a test.
             */
            emitAudioMode(plan, true);
            emitTxDeviation(plan, DMR_TX_DEVIATION);
            if (opt->dmr12k5) {
                emitBandwidth(plan, BW_12_5);
                plan->knobs.rxReg43 = REG43_DIGI_12K5;
            } else {
                emitBandwidth(plan, BW_25);
                plan->knobs.rxReg43 = REG43_DIGI_25K;
            }
            emitRfFilter(plan, 7, 7);
            emitTxAudioFilter(plan, DMR_TX_LPF2_BW);
            emitRxAudioGain(plan, DIGI_RX_AUDIO_GAIN1, DIGI_RX_AUDIO_GAIN2);
            emitMicGain(plan, DIGI_MIC_GAIN);
            break;

        case OPMODE_NXDN:
            /*
             * 4FSK, 2.9 kHz of occupied bandwidth in a 6.25 kHz channel and the
             * same waveform in 12.5 kHz.  The chip has no 6.25 kHz mode, so the
             * 12.5 kHz one is used and its IF filter follows the channel
             * spacing; the demodulator's matched filter does the selection.
             */
            emitAudioMode(plan, true);
            /*
             * The deviation word used to be inherited: only M17 and DMR wrote
             * it, so an NXDN transmission carried whatever the previous mode
             * left in REG_40 -- M17's word, FM's band-calibrated one or the
             * chip's reset value.  The 4FSK modes want the wide digital word,
             * the same one M17 uses; a bench run is what would tune it per
             * mode, and until then at least it is the same on every boot.
             */
            emitTxDeviation(plan, M17_TX_DEVIATION);
            emitBandwidth(plan, BW_12_5);
            if (bandwidth == BW_6_25)
                emitRfFilter(plan, 3, 3); /* about 2.9 kHz */
            else
                emitRfFilter(plan, 7, 7);
            plan->knobs.rxReg43 = REG43_DIGI_12K5;
            emitTxAudioFilter(plan, DMR_TX_LPF2_BW);
            emitRxAudioGain(plan, DIGI_RX_AUDIO_GAIN1, DIGI_RX_AUDIO_GAIN2);
            emitMicGain(plan, DIGI_MIC_GAIN);
            break;

        case OPMODE_DPMR:
            /*
             * A dPMR channel is 6.25 kHz, half of what the chip can filter: the
             * 12.5 kHz receive path and the mode's own root-raised-cosine
             * matched filter (alpha 0.2, TS 102 490 clause 12.2.2.2) do the
             * channel selection.  No transmitter.
             */
            emitAudioMode(plan, true);
            emitTxDeviation(plan, M17_TX_DEVIATION); /* see NXDN */
            emitBandwidth(plan, BW_12_5);
            emitRfFilter(plan, 7, 7);
            emitTxAudioFilter(plan, DMR_TX_LPF2_BW);
            emitRxAudioGain(plan, DIGI_RX_AUDIO_GAIN1, DIGI_RX_AUDIO_GAIN2);
            emitMicGain(plan, DIGI_MIC_GAIN);
            plan->knobs.rxReg43 = REG43_M17_NARROW;
            break;

        case OPMODE_P25:
        case OPMODE_YSF:
        case OPMODE_DSTAR:
            /*
             * C4FM at 4800 symbols/s and GMSK at 4800 bit/s: constant-envelope
             * digital basebands that need the same flat, DC-coupled path as DMR.
             * None of the three has been measured on air, so they take the
             * 12.5 kHz channel mode with the stock firmware's digital word.
             */
            emitAudioMode(plan, true);
            emitTxDeviation(plan, M17_TX_DEVIATION); /* see NXDN */
            emitBandwidth(plan, BW_12_5);
            emitRfFilter(plan, 7, 7);
            plan->knobs.rxReg43 = REG43_DIGI_12K5;
            emitTxAudioFilter(plan, DMR_TX_LPF2_BW);
            emitRxAudioGain(plan, DIGI_RX_AUDIO_GAIN1, DIGI_RX_AUDIO_GAIN2);
            emitMicGain(plan, DIGI_MIC_GAIN);
            break;

        case OPMODE_TETRA:
            /* 25 kHz channels: 25 kHz mode, the widest RF filters */
            emitAudioMode(plan, true);
            emitBandwidth(plan, BW_25);
            emitRfFilter(plan, 7, 7);
            emitTxAudioFilter(plan, DMR_TX_LPF2_BW);
            emitRxAudioGain(plan, DIGI_RX_AUDIO_GAIN1, DIGI_RX_AUDIO_GAIN2);
            emitMicGain(plan, DIGI_MIC_GAIN);
            plan->knobs.rxReg43 = REG43_DIGI_25K;
            break;

        case OPMODE_AIRBAND:
            /*
             * AM receive.  The IF filter follows the channel spacing, the rest
             * is the stock firmware's AM profile plus the audio AGC the mode
             * runs on the way to the speaker.
             */
            emitAmReceive(plan, bandwidth == BW_25);
            emitRxAudioGain(plan, AM_RX_AUDIO_GAIN1, AM_RX_AUDIO_GAIN2);
            plan->knobs.rxAfGain1 = AM_RX_AUDIO_GAIN1;
            plan->knobs.amAudio = true;
            plan->knobs.amTx = opt->airTx;
            break;

        case OPMODE_WFM_BC:
            /*
             * The BK4819 is not in this path at all: its audio is muted so it
             * cannot sum into the amplifier input, and the analog FM profile is
             * left in place for when the operator leaves the mode.
             */
            emitAudioMode(plan, false);
            emitAfOutput(plan, RADIO_PLAN_AF_MUTE);
            break;

        default:
            /* Analog FM, and nothing selected at all */
            emitAudioMode(plan, false);
            break;
    }
}

/**
 * The receive path a mode opens when the receiver starts: radio_enableRx().
 *
 * The flat modes hand the raw discriminator to the DSP here, at every receive
 * start, because the audio path is closed and reopened around a transmission.
 */
static void buildRxPath(struct radioPlan *plan, enum opmode mode,
                        const struct radioPlanOptions *opt)
{
    if (!radioPlan_isFlat(mode))
        return;

    emitAfOutput(plan, RADIO_PLAN_AF_BYPASS);

    if ((mode == OPMODE_TETRA) && opt->tetraIfDcFilter) {
        /*
         * The RX IF DC filter (REG_7E<2:0>, chip default 0b110) that the audio
         * bypass clears.  With it off, the IF DC offset of this low-IF receiver
         * reaches the demodulator as a one-sided tone at 16.9 kHz and distorts
         * the +3pi/4 symbols much more than the -3pi/4 ones.  TETRA decodes
         * phase differences and needs it gone; DMR slices absolute 4FSK levels
         * and must not inherit it, which is why this is TETRA's alone.
         */
        emit(plan, 0x7E, BK4819_REG7E_RX_DCF_BW_MASK,
             BK4819_REG7E_RX_DCF_BW(6));
    }
}

/**
 * What a reconfiguration writes: radio_updateConfiguration().
 *
 * The mode change sets the channel-dependent part once; this is the same thing
 * again when the operator moves the channel spacing or the TX calibration at
 * run time, which is why FM repeats its deviation word and airband its filter.
 */
static void buildUpdate(struct radioPlan *plan, enum opmode mode,
                        enum bandwidth bandwidth,
                        const struct radioPlanOptions *opt)
{
    if (mode == OPMODE_FM) {
        switch (bandwidth) {
            case BW_6_25:
                emitBandwidth(plan, BW_6_25);
                emitTxDeviation(plan, opt->fmTxDevNarrow);
                break;

            case BW_12_5:
                emitBandwidth(plan, BW_12_5);
                emitTxDeviation(plan, opt->fmTxDevNarrow);
                break;

            case BW_25:
                emitBandwidth(plan, BW_25);
                emitTxDeviation(plan, opt->fmTxDevWide);
                break;

            default:
                break;
        }
    } else if (mode == OPMODE_AIRBAND) {
        emitAmReceive(plan, bandwidth == BW_25);
    }
}

void radioPlan_build(struct radioPlan *plan, enum radioPlanStage stage,
                     enum opmode mode, enum bandwidth bandwidth,
                     enum radioPlanDir dir, const struct radioPlanOptions *opt)
{
    struct radioPlanOptions defaults;

    if (opt == NULL) {
        radioPlan_defaultOptions(&defaults);
        opt = &defaults;
    }

    plan->mode = mode;
    plan->bandwidth = bandwidth;
    plan->dir = dir;
    plan->stage = stage;
    plan->count = 0;
    plan->knobs.rxReg43 = 0;
    plan->knobs.rxAfGain1 = 0;
    plan->knobs.flat = radioPlan_isFlat(mode);
    plan->knobs.amAudio = false;
    plan->knobs.amTx = false;

    /*
     * Keying writes no part of a mode's configuration: the transmitter comes up
     * with the profile the mode change left, and only the PLL, the CTCSS and the
     * signal path enables change.  Saying so here means a deviation "fixed" at
     * keying time would have to be added to this table to exist.
     */
    if (dir == RADIO_PLAN_TX)
        return;

    switch (stage) {
        case RADIO_PLAN_STAGE_MODE:
            buildModeProfile(plan, mode, bandwidth, opt);
            break;

        case RADIO_PLAN_STAGE_RX:
            buildRxPath(plan, mode, opt);
            break;

        case RADIO_PLAN_STAGE_UPDATE:
            buildUpdate(plan, mode, bandwidth, opt);
            break;

        case RADIO_PLAN_STAGE_AF:
            /* Broadcast FM's audio does not come from the BK4819 */
            if (mode != OPMODE_WFM_BC)
                emitAfOutput(plan, radioPlan_afOutput(mode));
            break;

        case RADIO_PLAN_STAGE_SPECTRUM:
            emitBandwidth(plan, BW_25);
            emitRfFilter(plan, 7, 7);
            emitAfOutput(plan, RADIO_PLAN_AF_MUTE);
            /*
             * AFC off (REG_73<4>): a carrier near the bin under measurement
             * would pull the synthesizer onto itself and move the bin.
             */
            emit(plan, 0x73, 0x0010, 0x0010);
            if (opt->spectrumAgcFix) /* AGC fixed, REG_7E<15> */
                emit(plan, 0x7E, 0x8000, 0x8000);
            plan->knobs.rxReg43 = REG43_DIGI_25K;
            break;
    }
}

void radioPlan_apply(const struct radioPlan *plan, const struct BK4819 *dev)
{
    for (uint8_t i = 0; i < plan->count; i++) {
        const struct radioPlanWrite *w = &plan->write[i];

        if (w->mask == 0xffff)
            bk4819_writeReg(dev, w->reg, w->value);
        else
            bk4819_maskSetRegister(dev, w->reg, w->mask, w->value);
    }
}

char *radioPlan_format(const struct radioPlan *plan, char *buf, size_t len)
{
    size_t n = 0;

    if (len == 0)
        return buf;

    n += (size_t)snprintf(buf + n, len - n, "%s %s %s: %u writes\n",
                          radioPlan_modeName(plan->mode),
                          (plan->dir == RADIO_PLAN_RX) ? "RX" :
                          (plan->dir == RADIO_PLAN_TX) ? "TX" :
                                                         "BOTH",
                          plan->stage == RADIO_PLAN_STAGE_MODE   ? "mode" :
                          plan->stage == RADIO_PLAN_STAGE_RX     ? "rx" :
                          plan->stage == RADIO_PLAN_STAGE_UPDATE ? "update" :
                          plan->stage == RADIO_PLAN_STAGE_AF     ? "af" :
                                                                   "spectrum",
                          (unsigned int)plan->count);

    for (size_t i = 0; (i < plan->count) && (n < len); i++) {
        const struct radioPlanWrite *w = &plan->write[i];

        if (w->mask == 0xffff)
            n += (size_t)snprintf(buf + n, len - n, "  [%2u] 0x%02x = 0x%04x\n",
                                  (unsigned int)i, w->reg, w->value);
        else
            n += (size_t)snprintf(buf + n, len - n,
                                  "  [%2u] 0x%02x mask 0x%04x value 0x%04x\n",
                                  (unsigned int)i, w->reg, w->mask, w->value);
    }

    return buf;
}
