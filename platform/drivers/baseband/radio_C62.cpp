/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <hwconfig.h>
#include <interfaces/nvmem.h>
#include <interfaces/radio.h>
#include <interfaces/delays.h>

#include <algorithm>

#include "calibration/calib_C62.h"
#include "core/agc.h"
#include "core/airband.h"
#include "core/state.h"
#include "core/wfm.h"
#include "drivers/baseband/BK4819.h"
#include "drivers/baseband/BK1080.h"
#include "drivers/audio/csk6_codec.h"
#include "radioUtils.h"
#include <stdio.h>

#ifdef CONFIG_C62_AIR_TX
#include "../../targets/c62/air_tx.h"
#endif
#ifdef C62_BASEBAND_DUMP
#include "../audio/baseband_dump_c62.h"
#endif

#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>

static const rtxStatus_t
    *config; // Pointer to data structure with radio configuration

static enum opstatus radioStatus; // Current operating status
static enum opmode radioMode;     // Current operating mode

/*
 * CTCSS TX gain of the stock C62 firmware, REG_51 <6:0> "CTCSS/CDCSS Tx Gain1
 * Tuning" (0 = min, 127 = max) in the BK4819 notes of
 * https://github.com/egzumer/uv-k5-firmware-custom/blob/7607f0a4bd6203d1f06b70556fc1ce0d7399d6b3/driver/bk4819.c#L463-L471
 */
static constexpr uint8_t CTCSS_TX_GAIN = 65;

/*
 * C62 BK4819 settings for M17, on top of the audio bypass mode, from the
 * A36Plus profile: https://wiki.m17foundation.org/index.php?title=A36Plus
 */
// Raw deviation tuning (not Hz), set for the TX DAC gain in hwconfig.h
static constexpr uint16_t M17_TX_DEVIATION = 0x582;
static constexpr uint8_t M17_RF_BW = 7;          // 4.5 kHz
static constexpr uint8_t M17_RF_BW_WEAK = 4;     // 3.75 kHz
static constexpr uint8_t M17_TX_LPF2_BW = 0;     // 3 kHz
static constexpr uint8_t M17_RX_AUDIO_GAIN1 = 1; // -6 dB
static constexpr uint8_t M17_RX_AUDIO_GAIN2 = 0x3f;
static constexpr uint8_t M17_MIC_GAIN = 0;

/*
 * C62 BK4819 settings for DMR. DMR channels are 12.5 kHz, but
 * the demodulator needs the discriminator flat and wide: as with TETRA, the
 * chip runs in 25 kHz mode with the widest RF filters (REG_43 0x7e28), which
 * is what the host prototype was validated on. The receiver estimates its
 * levels per burst, so the deviation (1944 Hz on ETSI, 1600 Hz on the C62)
 * does not have to be configured.
 *
 * C62_DMR_12K5 builds it the other way, the way the stock firmware runs its own
 * 4FSK digital mode (REG_43 = 0x7e48: 12.5 kHz mode, RF filters 4.5 kHz, where
 * the 25 kHz mode doubles them to 9 kHz). A 12.5 kHz channel with a 9 kHz IF
 * filter lets a neighbouring channel through, so this is the setting to try
 * when a capture shows energy from outside the channel. See
 * reports/dmr_rx_hw.md.
 */
#ifdef C62_DMR_12K5
static constexpr uint8_t DMR_RF_BW = 7;      // 4.5 kHz, not doubled
static constexpr uint8_t DMR_RF_BW_WEAK = 7; // 4.5 kHz
#else
static constexpr uint8_t DMR_RF_BW = 7;      // 4.5 kHz, doubled to 9 kHz
static constexpr uint8_t DMR_RF_BW_WEAK = 7; // 4.5 kHz
#endif
static constexpr uint8_t DMR_TX_LPF2_BW = 0; // 3 kHz: REG_43 AF TX LPF2

/*
 * TX deviation tuning word (REG_40 <11:0>).  M17 uses 0x582 for its 4FSK,
 * whose outer symbols sit at +-2.4 kHz; ETSI DMR puts them at +-1.944 kHz, so
 * 0x582 * 1944/2400 = 0x476.  It is a starting point, not a calibration: the
 * BK4819 drives a varactor whose sensitivity differs per band and per PA, and
 * the C62 stores per-band TX calibration (calib_C62, txDevNarrow/txDevWide,
 * raw words with no Hz attached).  Measure +3/-3 alternating symbols with an
 * analyser (or with a second C62 and its level estimates) and put the result
 * in DMR_TX_DEVIATION, then in the calibration table like the FM values.
 *
 * The modulator scales the symbols, not the deviation: DMR_TX_INNER_LEVEL is
 * the audio amplitude and this word is the FM sensitivity.
 */
static constexpr uint16_t DMR_TX_DEVIATION = 0x476;
static constexpr uint8_t DMR_RX_AUDIO_GAIN1 = 1; // -6 dB
static constexpr uint8_t DMR_RX_AUDIO_GAIN2 = 0x3f;
static constexpr uint8_t DMR_MIC_GAIN = 0;

/*
 * C62 BK4819 settings for TETRA, receive only. TETRA channels are 25 kHz
 * wide, so the chip runs in 25 kHz mode with the widest RF filters
 * (REG_43 0x7e28): that is the flat receive path the measured equalizer taps
 * in CONFIG_TETRA_EQ_INIT come from.
 */
static constexpr uint8_t TETRA_RF_BW = 7;      // 4.5 kHz
static constexpr uint8_t TETRA_RF_BW_WEAK = 7; // 4.5 kHz
static constexpr uint8_t TETRA_TX_LPF2_BW = 0; // 3 kHz, unused in RX
static constexpr uint8_t TETRA_RX_AUDIO_GAIN1 = 1; // -6 dB
static constexpr uint8_t TETRA_RX_AUDIO_GAIN2 = 0x3f;
static constexpr uint8_t TETRA_MIC_GAIN = 0;

/*
 * C62 BK4819 settings for the airband mode, AM receive only. The AM detector
 * itself is bk4819_setAmReceive(); what is left here is the AF level it hands
 * over and the software AGC that holds it.
 *
 * REG_48<11:10> (AF Rx Gain1) is the coarse step of that AGC: the chip's own
 * AGC keeps the IF at a constant level in five coarse steps, but what comes
 * out of an AM detector is the carrier amplitude, so the AF level still follows
 * the field strength. The loop in radio_getRssi() moves this step to keep the
 * codec ADC in range; core/agc.h then does the fine work per sample. AM starts
 * one step down from FM: a receiver landing on a strong local tower should not
 * clip the ADC in the 100 ms the loop needs.
 */
static constexpr uint8_t AM_RX_AUDIO_GAIN1 = 1; // -6 dB, first step
static constexpr uint8_t AM_RX_AUDIO_GAIN2 = 0x3f;

/* Codec ADC peak, sample units, that asks for a coarser or a finer AF step */
static constexpr uint16_t AM_AF_PEAK_HIGH = 28000; // 0.85 of full scale
static constexpr uint16_t AM_AF_PEAK_LOW = 7000;   // 0.21 of full scale

/*
 * Airband AM transmitter (CONFIG_C62_AIR_TX). The carrier is a BK4819
 * transmission with no audio in it: the amplitude modulation happens in the PA,
 * where the APC duty sets its bias, and core/am_tx.c turns the microphone into
 * that duty. Three numbers describe the calibration:
 *
 *  - AM_CARRIER_MW: the carrier power the mode asks for. 1 W, the low point of
 *    the factory band calibration and the only one this radio can produce
 *    without leaving the linear part of the PA for the peaks of the envelope:
 *    at 90 % depth the peaks are 3.6 times the carrier.
 *  - AM_DUTY_MIN_PCT: the bottom of the linear window, in percent of the 1 W
 *    duty. One tenth is what a 90 % depth needs; the PA of this radio has not
 *    been measured there yet, and if it stops being linear earlier the
 *    modulator caps the depth instead of clipping the envelope.
 *  - AM_DUTY_MAX_PCT: the top, 190 % of the carrier duty, which is what a 90 %
 *    depth asks for (1.9 times the carrier voltage). Well below the 5 W point,
 *    which would be 4 times the carrier voltage.
 *
 * None of the three is measured: the factory calibration has no point below
 * 136.125 MHz, so the duty for 118 MHz is extrapolated. docs/TX_AIRBAND.md has
 * the bench procedure that replaces them.
 */
static constexpr uint32_t AM_CARRIER_MW = 1000;
static constexpr uint8_t AM_DUTY_MIN_PCT = 10;
static constexpr uint8_t AM_DUTY_MAX_PCT = 190;

/* REG_48<11:10> has four steps, 0 dB to -18 dB in 6 dB steps */
static constexpr uint8_t AM_RX_AUDIO_GAIN1_MAX = 3;
static constexpr int64_t AM_AF_STEP_MS = 300;

/*
 * Broadcast FM. The BK1080 is a receiver of its own, with its own LNA, IF,
 * stereo decoder and audio DACs, and its L/R outputs reach the speaker
 * amplifier through the analog volume pot: no part of this audio goes through
 * the BK4819, the codec or the DSP. What the radio driver has to do is power
 * and tune the tuner, keep the BK4819 audio out of the summing node, and
 * switch the amplifier on.
 *
 * The chip tunes 76.0-108.0 MHz with BAND=01 (the value the factory table
 * leaves) and 87.5-108.0 with BAND=00. BAND=01 is used for the whole band, as
 * in the stock firmware, which is what makes 85.0-87.5 MHz reachable at all.
 */
static constexpr uint32_t WFM_STATUS_MS = 100;  // Status poll period, ms
static constexpr uint8_t WFM_VOLUME = 0x0F;     // 0 dBFS: the pot does volume

/*
 * C62 BK4819 settings for dPMR, receive only.  A dPMR channel is 6,25 kHz, half
 * of what the chip can filter: its narrowest IF filter is 4,5 kHz, in 12,5 kHz
 * mode, so the radio runs the 12,5 kHz receive path (REG_43 0x7e48, the word
 * the stock firmware uses for its own 4FSK mode) and the 6,25 kHz
 * selectivity comes from the mode's own square root raised cosine matched
 * filter, alpha = 0.2, which is what TS 102 490 clause 12.2.2.2 asks for
 * anyway.  A wider filter costs adjacent channel immunity, not sensitivity, and
 * the alternative -- 25 kHz mode, 9 kHz of IF -- lets a whole neighbour in.
 *
 * The RX audio gain is the DMR one: it is the same discriminator output through
 * the same audio bypass, and the slicer estimates its own levels per frame, so
 * the deviation the chip delivers (+-1,6 kHz measured against ETSI's 1,05 kHz)
 * does not have to be corrected here.
 */
static constexpr uint8_t DPMR_RF_BW = 7;      // 4.5 kHz, 12.5 kHz mode
static constexpr uint8_t DPMR_RF_BW_WEAK = 7; // 4.5 kHz
static constexpr uint8_t DPMR_TX_LPF2_BW = 0; // 3 kHz, no transmitter
static constexpr uint8_t DPMR_RX_AUDIO_GAIN1 = 1; // -6 dB
static constexpr uint8_t DPMR_RX_AUDIO_GAIN2 = 0x3f;
static constexpr uint8_t DPMR_MIC_GAIN = 0;   // no transmitter

/**
 * Digital modes receive the raw discriminator through the flat audio path:
 * M17, and DMR (which transmits from the same flat path).
 */
static bool flatMode(enum opmode mode)
{
    return (mode == OPMODE_M17) || (mode == OPMODE_DMR)
           || (mode == OPMODE_TETRA) || (mode == OPMODE_NXDN)
           || (mode == OPMODE_DPMR) || (mode == OPMODE_P25)
           || (mode == OPMODE_YSF) || (mode == OPMODE_DSTAR);
}

/*
 * Fine RX tuning offset, Hz, added to the channel frequency: for tests and the
 * TETRA frequency correction loop. See c62_setRxOffset().
 */
static volatile int32_t rxOffset;

/*
 * Total offset applied to the tuned frequency: the runtime one above (TETRA's
 * automatic correction) plus the manual fine tuning stored in the settings.
 *
 * They are kept apart so they cannot overwrite each other: the AFC loop only
 * ever moves rxOffset, the user only ever moves settings.rxFineHz, and every
 * retune adds both. Reading the setting here also means the offset survives a
 * reboot without anyone having to push it back down at start-up.
 */
static int32_t rxTotalOffset()
{
#ifdef CONFIG_RX_FINE_OFFSET
    return rxOffset + (int32_t)state.settings.rxFineHz;
#else
    return rxOffset;
#endif
}

/* Read by the baseband dump: the REG_43 value currently in use for RX */
extern "C" {
volatile uint16_t c62_rx_reg43 = 0x7808;
}

/*
 * Spectrum analyser state (see the section at the end of this file).
 *
 * Declared up here because radio_updateConfiguration() has to know whether a
 * sweep session owns the chip: it invalidates the sweep's receive profile, and
 * the next sweep has to put it back.
 */
static bool     specOn;          // A sweep session owns the receiver
static bool     specWide;        // The sweep profile is the one in the chip
static bool     specDirty;       // Something else rewrote the profile
static bool     specGateOff;     // The glitch gate did not work: don't poll
static uint8_t  specGateFails;   // Consecutive points whose gate ran out
static uint32_t specGatePolls;   // Glitch polls spent since the screen opened
static uint32_t specPoints;      // Points swept since the screen opened
static uint16_t specReg43;       // REG_43 the mode was using
static uint16_t specReg73;       // REG_73 (AFC), saved at entry
#ifdef CONFIG_C62_SPECTRUM_AGC_FIX
static uint16_t specReg7E;       // REG_7E (AGC fix), saved at entry
#endif
static uint32_t specFreq;        // Frequency the PLL was last set to
static uint32_t specWord;        // Its 10 Hz word, as last written

#ifdef C62_REG43_SWEEP
/*
 * Cycle REG_43 through these values in the digital RX modes,
 * C62_REG43_SWEEP_MS each, to measure them with a fixed test signal. Each
 * dump packet carries the value in use. The last three have undocumented bits
 * set. The sweep clock only runs while the dump records, so with the default
 * dwell one 10.4 s burst covers all seven values.
 */
static const uint16_t reg43Sweep[] = {
    0x7808, // M17 default, 12.5 kHz
    0x7828, // 25 kHz, weak-signal filter 3.75 kHz
    0x7e28, // 25 kHz, weak-signal filter 4.5 kHz
    0x7e2c, // as above, +6 dB after demodulation
    0x7e38, // BW mode 3 (undocumented)
    0x7e20, // bit 3 cleared (undocumented)
    0xfe28, // bit 15 set (undocumented)
};
static size_t reg43SweepIndex;
static int64_t reg43SweepLast;    // Uptime at the previous check
static int64_t reg43SweepElapsed; // Recorded time on the current value
static bool reg43SweepStarted;
#endif

static struct C62Calib calData;  // TX calibration
static struct C62CalPoint txCal; // TX calibration at the TX frequency

/* Level control of the AM receive path, see the airband settings above */
static struct agc amAgc;
static uint8_t amAfGain1 = AM_RX_AUDIO_GAIN1;
static int64_t amAfGainTime;

/**
 * Per-sample processing of the AM audio on its way to the speaker: the AGC and
 * its limiter. Runs in the audio worker (see csk6codec_setRxProcessor()), so it
 * only touches the sample block it is given.
 *
 * @param samples: first sample of the speaker channel.
 * @param count: samples.
 * @param stride: distance between samples.
 */
static void amAudioProcess(int16_t *samples, size_t count, size_t stride)
{
    /*
     * The operator can turn the AGC off from the quick menu or Settings → AM
     * (settings.am_agc): a steady, weak carrier is sometimes easier to copy
     * with the level left alone than with the AGC pumping on the noise between
     * transmissions. The setting is read here, once per block, so the change is
     * heard as soon as the row is moved.
     */
    if (state.settings.am_agc)
        agc_process(&amAgc, samples, count, stride);
}

/*
 * Broadcast FM receiver state.  wfmReady means the tuner answered its chip ID
 * and is powered: entering the mode costs the configuration write plus the
 * calibration waits (about 0.3 s, see BK1080_Init), so it is done once on
 * entry and not on every retune.
 */
static bool wfmReady;
static uint8_t wfmEnterFails;       // Failed power-ups, to bound the retries
static enum bk1080Spacing wfmSpacing = BK1080_SPACE_100K;
static wfm_status_t wfmLast;        // Last status read from the tuner
static rssi_t wfmLastDbm = -127;    // Its level on the dBm scale
static bool wfmLastOk;              // The last read reached the chip
static int64_t wfmLastPoll;         // Uptime of that read, ms

/**
 * Power and configure the tuner, once per entry into the mode.
 */
static bool wfmEnter()
{
    gpioPin_setMode(&c62_bk1080.pwr, OUTPUT);
    gpioPin_set(&c62_bk1080.pwr);       // make sure it is off before powering

    if (!BK1080_Init(&c62_bk1080)) {
        /*
         * A chip that does not answer costs a second of polling, so do not
         * retry it on every retune: three attempts per power cycle, then the
         * mode stays silent until the next one.
         */
        if (wfmEnterFails < UINT8_MAX)
            wfmEnterFails++;

        return false;
    }

    /*
     * The stock firmware leaves the factory table's values here: 100 kHz
     * spacing, full-scale digital volume (the analog pot is the volume
     * control), AGC and the soft mute enabled, and 50 us de-emphasis, which
     * is what Europe transmits (the table has DE = 0, 75 us, for the US).
     */
    BK1080_SetVolume(&c62_bk1080, WFM_VOLUME);
    BK1080_SetDeemphasis50us(&c62_bk1080, true);
    BK1080_SetMono(&c62_bk1080, false);
    BK1080_SetSeekThreshold(&c62_bk1080, 8);    // dBuV, around S3
    BK1080_SetSeekQuality(&c62_bk1080, 2, 14);  // the factory table's values

    wfmLastPoll = 0;
    return true;
}

/**
 * Mute the tuner and drop its supply: the mode is over.
 */
static void wfmLeave()
{
    if (!wfmReady)
        return;

    BK1080_PowerOff(&c62_bk1080);
    wfmReady = false;
}

/**
 * Read the tuner status, no more often than WFM_STATUS_MS.
 *
 * @param force: read now even if the last read is recent.
 * @return true if the chip answered.
 */
static bool wfmUpdateStatus(const bool force)
{
    int64_t now = k_uptime_get();

    if ((!force) && ((now - wfmLastPoll) < (int64_t)WFM_STATUS_MS))
        return wfmLastOk;

    wfmLastPoll = now;

    struct bk1080Status hw;
    if (!BK1080_ReadStatus(&c62_bk1080, &hw)) {
        wfmLastOk = false;
        return false;
    }

    wfmLast.chan = hw.chan;
    wfmLast.rssi = hw.rssi;
    wfmLast.snr = hw.snr;
    wfmLast.freqd = (int16_t)hw.freqd;
    wfmLast.tuned = hw.stc;
    wfmLast.afcrl = hw.afcrl;
    wfmLast.stereo = hw.stereo;
    wfmLast.stereoSync = hw.stereoSync;
    wfmLast.rds = false;
    wfmLast.freq = (config != nullptr) ? config->rxFrequency : 0;

    wfmLastDbm = wfm_dbuVToDbm(hw.rssi);
    wfmLastOk = true;

    return true;
}

/**
 * Tune the broadcast receiver, muted across the retune so the PLL settling
 * does not reach the speaker.
 */
static void wfmTune(const uint32_t freq)
{
    if (!wfmReady)
        return;

    freq_t target = wfm_snap(freq, (wfmSpacing == BK1080_SPACE_50K)
                                      ? WFM_STEP_FINE_HZ : WFM_STEP_HZ);

    BK1080_SetMute(&c62_bk1080, true);
    bool ok = BK1080_SetFrequency(&c62_bk1080, target, wfmSpacing);
    BK1080_SetMute(&c62_bk1080, !ok);

    /* The previous status was another channel: do not carry it over */
    wfmLastPoll = 0;
    wfmLast = wfm_status_t();
    wfmLastDbm = -127;
    wfmLastOk = false;
}

/**
 * Set the TX power control (APC) voltage through the PWM on pin A03. The TX
 * calibration gives the duty cycles of the low and high power settings at the
 * TX frequency; the power is interpolated linearly between them, from the low
 * setting at 1 W to the high one at 5 W, as on the MD-3x0 and MD-UV3x0. The
 * PWM runs at 100 kHz, as in the stock firmware: at 1 kHz the APC voltage
 * follows the ripple and the output power stops tracking the duty cycle.
 *
 * @param power: TX power, in mW.
 */
static void set_tx_power(const uint32_t power)
{
    float pwr = static_cast<float>(power) / 1000.0f;
    pwr = std::max(std::min(pwr, 5.0f), 1.0f);
    float pwrHi = static_cast<float>(txCal.hiPwr);
    float pwrLo = static_cast<float>(txCal.loPwr);
    uint8_t duty =
        static_cast<uint8_t>(pwrLo + (pwrHi - pwrLo) / 4.0f * (pwr - 1.0f));

    const uint32_t period_ns = 10000;
    uint32_t pulse_ns = (period_ns * duty) / 100;

    int ret = pwm_set_dt(&pwm_rf_apc, period_ns, pulse_ns);

    if (ret < 0) {
        printk("Failed to set TX power PWM: %d\n", ret);
    }
}

void radio_init(const rtxStatus_t *rtxState)
{
    config = rtxState;
    radioStatus = OFF;
    radioMode = OPMODE_FM;

    nvm_readCalibData(&calData);

    bk4819_setAfOutput(&c62_bk4819, BK4819_AF_MUTE);

    /*
     * The broadcast receiver is off until its mode is selected: drive its
     * power pin high (active low) so the chip cannot draw its 20 mA in FM,
     * M17, DMR or TETRA.
     */
    gpioPin_setMode(&c62_bk1080.pwr, OUTPUT);
    gpioPin_set(&c62_bk1080.pwr);
    wfmReady = false;

#ifdef CONFIG_C62_AIR_TX
    /* The amplitude modulator of the airband transmitter, see air_tx.h */
    int airTx = air_tx_init();
    if (airTx == -ENOTSUP)
        printk("AM TX: no pacing timer, airband transmit limited to 1 kHz\n");
    else if (airTx != 0)
        printk("AM TX: APC power control not available, airband is RX only\n");
#endif

    gpioDev_setMode(LNA_VHF_EN, OUTPUT_ACTIVE_LOW);
    gpioDev_setMode(LNA_UHF_EN, OUTPUT_ACTIVE_LOW);
    gpioDev_setMode(PA_VHF_EN, OUTPUT);
    gpioDev_setMode(PA_UHF_EN, OUTPUT);
    gpioDev_setMode(ALC_TX_LED_EN, OUTPUT);
}

void radio_terminate()
{
    /* The tuner has its own supply switch: do not leave it drawing 20 mA */
    wfmLeave();

#ifdef CONFIG_C62_AIR_TX
    /* Nothing may be left keyed, and no ramp matters at shutdown */
    air_tx_abort();
#endif
}

void radio_tuneVcxo(const int16_t vhfOffset, const int16_t uhfOffset)
{
    (void)vhfOffset;
    (void)uhfOffset;
}

void radio_setOpmode(const enum opmode mode)
{
    // The core calls this before disabling the old mode: unkey first
    radio_disableRtx();

    /*
     * Only the airband mode receives AM. Every other one goes back to the FM
     * demodulator profile first, because the AM one changes registers they
     * would otherwise inherit: the top of the RX AGC gain table (REG_13), the
     * undocumented REG_4A, the AF de-emphasis (REG_2B<8>) and the AF expander
     * (REG_28). The audio processing of the AM path is uninstalled at the same
     * time, while its audio path is closed.
     */
    if (mode != OPMODE_AIRBAND) {
        bk4819_setModulation(&c62_bk4819, true);
        csk6codec_setRxProcessor(NULL);
    }

    /*
     * Leaving broadcast FM: the tuner has its own output on the amplifier
     * input, so it has to be powered down and the analog path closed, or it
     * keeps playing under the next mode.
     */
    if ((mode != OPMODE_WFM_BC) && wfmReady) {
        wfmLeave();
        audio_setExtSource(false);
    }

    if (mode == OPMODE_M17) {
        bk4819_setAudioMode(&c62_bk4819, BK4819_AUDIO_BYPASS);
        bk4819_setTxDeviation(&c62_bk4819, true, M17_TX_DEVIATION);
#ifdef C62_WIDE_RX
        // 25 kHz mode with the widest documented RF filters (REG_43 0x7e28)
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_25K);
        bk4819_setRfFilter(&c62_bk4819, M17_RF_BW, 7);
        c62_rx_reg43 = 0x7e28;
#else
        // 12.5 kHz bandwidth mode; the M17 filters then replace its filters
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_12K5);
        bk4819_setRfFilter(&c62_bk4819, M17_RF_BW, M17_RF_BW_WEAK);
        c62_rx_reg43 = 0x7808;
#endif
        bk4819_setTxAudioFilter(&c62_bk4819, M17_TX_LPF2_BW);
        bk4819_setRxAudioGain(&c62_bk4819, M17_RX_AUDIO_GAIN1,
                              M17_RX_AUDIO_GAIN2);
        bk4819_setMicGain(&c62_bk4819, M17_MIC_GAIN);
    } else if (mode == OPMODE_DMR) {
        /*
         * Flat, wide receive path, as for TETRA, and the same flat,
         * DC-coupled transmit path: AUDIO_BYPASS clears the AF TX filters,
         * the ALC, the compander and the TX DC filter (REG_2B/31/47/4B/7E),
         * which is what a 4FSK baseband that starts at DC needs.  The
         * deviation word sets the FM sensitivity of the varactor, and the
         * modulator (DmrTx.c) does the pulse shaping.
         */
        bk4819_setAudioMode(&c62_bk4819, BK4819_AUDIO_BYPASS);
        bk4819_setTxDeviation(&c62_bk4819, true, DMR_TX_DEVIATION);
#ifdef C62_DMR_12K5
        /* The stock firmware's own digital setting: 12.5 kHz channel mode */
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_12K5);
        bk4819_setRfFilter(&c62_bk4819, DMR_RF_BW, DMR_RF_BW_WEAK);
        c62_rx_reg43 = 0x7e48;
#else
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_25K);
        bk4819_setRfFilter(&c62_bk4819, DMR_RF_BW, DMR_RF_BW_WEAK);
        c62_rx_reg43 = 0x7e28;
#endif
        bk4819_setTxAudioFilter(&c62_bk4819, DMR_TX_LPF2_BW);
        bk4819_setRxAudioGain(&c62_bk4819, DMR_RX_AUDIO_GAIN1,
                              DMR_RX_AUDIO_GAIN2);
        bk4819_setMicGain(&c62_bk4819, DMR_MIC_GAIN);
    } else if (mode == OPMODE_NXDN) {
        /*
         * NXDN is 4FSK at 2400 symbols/s with a 2.9 kHz occupied bandwidth in
         * a 6.25 kHz channel, and the same waveform in 12.5 kHz.  The BK4819
         * has no 6.25 kHz mode, so the narrowest it offers is used (12.5 kHz
         * channel, RF filters not doubled: REG_43 = 0x7e48, the stock
         * firmware's own digital setting) and the demodulator's 81-tap
         * matched filter does the channel selection.  AUDIO_BYPASS is what the
         * DC-coupled 4FSK baseband needs: no de-emphasis, no compander, no
         * high-pass (REG_7E = 0, the defect that costs DMR its payload).
         */
        bk4819_setAudioMode(&c62_bk4819, BK4819_AUDIO_BYPASS);
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_12K5);
        /*
         * The IF filter follows the channel spacing the operator picked. 7,7 is
         * 4.5 kHz, the widest, and the one every measurement in docs/RX_NXDN.md
         * was taken with; a 6.25 kHz channel asks for 3,3 (about 2.9 kHz), which
         * fits the 2.9 kHz the waveform occupies and rejects the adjacent
         * 6.25 kHz slot. Only the IF changes: the channel selection is the
         * demodulator's matched filter either way.
         */
        if (config->bandwidth == BW_6_25)
            bk4819_setRfFilter(&c62_bk4819, 3, 3);
        else
            bk4819_setRfFilter(&c62_bk4819, 7, 7);
        c62_rx_reg43 = 0x7e48;
        bk4819_setTxAudioFilter(&c62_bk4819, DMR_TX_LPF2_BW);
        bk4819_setRxAudioGain(&c62_bk4819, DMR_RX_AUDIO_GAIN1,
                              DMR_RX_AUDIO_GAIN2);
        bk4819_setMicGain(&c62_bk4819, DMR_MIC_GAIN);
    } else if (mode == OPMODE_DPMR) {
        /*
         * Flat, narrow receive path: 4FSK baseband straight to the DSP, with
         * the mode's own matched filter doing the shaping.  No transmit setup:
         * the mode has no transmitter.
         */
        bk4819_setAudioMode(&c62_bk4819, BK4819_AUDIO_BYPASS);
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_12K5);
        bk4819_setRfFilter(&c62_bk4819, DPMR_RF_BW, DPMR_RF_BW_WEAK);
        bk4819_setTxAudioFilter(&c62_bk4819, DPMR_TX_LPF2_BW);
        bk4819_setRxAudioGain(&c62_bk4819, DPMR_RX_AUDIO_GAIN1,
                              DPMR_RX_AUDIO_GAIN2);
        bk4819_setMicGain(&c62_bk4819, DPMR_MIC_GAIN);
        c62_rx_reg43 = 0x7808;
    } else if ((mode == OPMODE_P25) || (mode == OPMODE_YSF) ||
               (mode == OPMODE_DSTAR)) {
        /*
         * P25 Phase 1 (C4FM, 4800 sym/s), System Fusion (C4FM, 4800 sym/s) and
         * D-STAR (GMSK, 4800 bit/s in a 6.25 kHz channel) are all constant
         * envelope digital basebands: they need the same flat, DC-coupled path
         * as DMR, with no de-emphasis, no compander and no high-pass, or the
         * slicer sees a shaped constellation instead of the discriminator.
         *
         * None of the three has been measured on air yet, so they take the
         * 12.5 kHz channel mode with the stock firmware's digital word
         * (REG_43 = 0x7e48) and the demodulator's own matched filter does the
         * channel selection, exactly like NXDN.  Receive only: no transmit
         * setup.
         */
        bk4819_setAudioMode(&c62_bk4819, BK4819_AUDIO_BYPASS);
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_12K5);
        bk4819_setRfFilter(&c62_bk4819, 7, 7);
        c62_rx_reg43 = 0x7e48;
        bk4819_setTxAudioFilter(&c62_bk4819, DMR_TX_LPF2_BW);
        bk4819_setRxAudioGain(&c62_bk4819, DMR_RX_AUDIO_GAIN1,
                              DMR_RX_AUDIO_GAIN2);
        bk4819_setMicGain(&c62_bk4819, DMR_MIC_GAIN);
    } else if (mode == OPMODE_TETRA) {
        // Flat, wide receive path: 25 kHz channel, widest RF filters
        bk4819_setAudioMode(&c62_bk4819, BK4819_AUDIO_BYPASS);
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_25K);
        bk4819_setRfFilter(&c62_bk4819, TETRA_RF_BW, TETRA_RF_BW_WEAK);
        bk4819_setTxAudioFilter(&c62_bk4819, TETRA_TX_LPF2_BW);
        bk4819_setRxAudioGain(&c62_bk4819, TETRA_RX_AUDIO_GAIN1,
                              TETRA_RX_AUDIO_GAIN2);
        bk4819_setMicGain(&c62_bk4819, TETRA_MIC_GAIN);
        c62_rx_reg43 = 0x7e28;
    } else if (mode == OPMODE_AIRBAND) {
        /*
         * AM receive, DSB with carrier. The IF filter follows the channel
         * spacing; the rest is the profile of the stock firmware plus the AF
         * processing of the airband mode: no de-emphasis, no expander, and the
         * AGC of core/agc.h on the audio on its way to the speaker, installed
         * here because it is part of the receive path.
         */
        bk4819_setAmReceive(&c62_bk4819, (config->bandwidth == BW_25)
                                             ? BK4819_AM_WIDE
                                             : BK4819_AM_NARROW);
        amAfGain1 = AM_RX_AUDIO_GAIN1;
        bk4819_setRxAudioGain(&c62_bk4819, amAfGain1, AM_RX_AUDIO_GAIN2);
        agc_init(&amAgc, NULL);
        csk6codec_setRxProcessor(amAudioProcess);
#ifdef CONFIG_C62_AIR_TX
        air_tx_setDepth(state.settings.am_depth);
#endif
    } else if (mode == OPMODE_WFM_BC) {
        /*
         * Broadcast FM. The BK4819 is not in this path at all: its audio is
         * muted so it cannot sum into the amplifier input, and the analog FM
         * profile is restored for when the operator leaves the mode. The
         * tuner is powered and configured here, before the first tune, so
         * that the calibration waits of BK1080_Init are paid once.
         */
        bk4819_setAudioMode(&c62_bk4819, BK4819_AUDIO_VOICE);
        bk4819_setAfOutput(&c62_bk4819, BK4819_AF_MUTE);
        wfmReady = wfmEnter();
        if (!wfmReady)
            printk("BK1080: no answer on the bus, broadcast FM unavailable\n");
    } else {
        bk4819_setAudioMode(&c62_bk4819, BK4819_AUDIO_VOICE);
    }

    radioMode = mode;
}

bool radio_checkRxDigitalSquelch()
{
    return bk4819_getCtcss(&c62_bk4819);
}

void radio_enableAfOutput()
{
    /*
     * In broadcast FM the BK4819 audio must stay out of the amplifier input:
     * the speaker is driven by the tuner, through the analog path, and
     * unmuting the transceiver here would mix its noise under the station.
     */
    if (radioMode == OPMODE_WFM_BC)
        return;

    /*
     * Only the RTX to speaker path opens it: FM demodulated audio for the
     * analog modes, the raw discriminator for the digital ones and the AM
     * detector for the airband one.
     */
    if (radioMode == OPMODE_AIRBAND)
        bk4819_setAfOutput(&c62_bk4819, BK4819_AF_AM);
    else if (flatMode(radioMode))
        bk4819_setAfOutput(&c62_bk4819, BK4819_AF_BYPASS);
    else
        bk4819_setAfOutput(&c62_bk4819, BK4819_AF_FM);
}

void radio_disableAfOutput()
{
    bk4819_setAfOutput(&c62_bk4819, BK4819_AF_MUTE);
}

#ifdef C62_BASEBAND_DUMP
/*
 * What the baseband dump sends alongside the samples.
 *
 * The samples alone do not say whether the radio was configured the way the
 * code thinks: the BK4819 registers are written by three different modes, by
 * the bypass entry of the application note and by the vendor init sequence, and
 * a capture can only be interpreted against what the chip actually holds. These
 * control packets carry that, read back over SPI, plus the level and
 * continuity figures the DSP path reports, so a capture file is self-describing.
 */
static const uint8_t dumpedRegs[] = {
    0x10, 0x11, 0x12, 0x13, 0x14, /* RX AGC gains */
    0x2B, 0x31, 0x47, 0x4B,       /* AF filters, compander, AF output, ALC */
    0x43, 0x48, 0x73, 0x7E,       /* BW/filters, AF RX gains, AFC, DC filters */
};

static void dumpSendRegisters()
{
    char text[192];
    int n = snprintf(text, sizeof(text), "regs");

    for (size_t i = 0; (i < ARRAY_SIZE(dumpedRegs)) && (n > 0)
                       && (n < (int)sizeof(text)); i++) {
        n += snprintf(text + n, sizeof(text) - (size_t)n, " %02X=%04X",
                      dumpedRegs[i], bk4819_readReg(&c62_bk4819, dumpedRegs[i]));
    }

    if ((n > 0) && (n < (int)sizeof(text)))
        baseband_dump_send_text(text, (size_t)n);
}

/**
 * Send the DSP path figures once a second: lost buffer halves and the peak of
 * the radio channel, which is what says whether the codec ADC is clipping.
 */
static void dumpSendStats()
{
    static int64_t last;
    int64_t now = k_uptime_get();

    if ((now - last) < 1000)
        return;
    last = now;

    char text[96];
    int n = snprintf(text, sizeof(text), "stats misses=%u rxpk=%u/32767 rssi=%d",
                     (unsigned int)csk6codec_getStreamMisses(),
                     (unsigned int)csk6codec_getRxPeak(),
                     (int)bk4819_getRssi(&c62_bk4819));

    if ((n > 0) && (n < (int)sizeof(text)))
        baseband_dump_send_text(text, (size_t)n);
}
#endif /* C62_BASEBAND_DUMP */

void radio_enableRx()
{
    /*
     * Broadcast FM: the tuner is the receiver. Its audio does not go through
     * the codec -- it reaches the amplifier through the analog volume pot --
     * so there is no audio path to open here, only the amplifier switch, and
     * the BK4819 stays out of the way with its audio muted.
     */
    if (radioMode == OPMODE_WFM_BC) {
        if ((!wfmReady) && (wfmEnterFails < 3))
            wfmReady = wfmEnter();

        if (wfmReady) {
            wfmTune(config->rxFrequency + rxTotalOffset());
            audio_setExtSource(true);
        }

        radioStatus = RX;
        return;
    }

    gpioDev_clear(LNA_VHF_EN);
    gpioDev_clear(LNA_UHF_EN);
    gpioDev_clear(PA_VHF_EN);
    gpioDev_clear(PA_UHF_EN);
    gpioDev_clear(ALC_TX_LED_EN);

    //Before enable RX path delay to allow hardware to settle
    delayMs(5);

    if (config->rxFrequency < 174000000) {
        gpioDev_set(LNA_VHF_EN);
    } else {
        gpioDev_set(LNA_UHF_EN);
    }

    /*
     * The airband mode tunes to the 8.33 kHz channel raster: its step is a
     * whole number of hertz (8333), so a frequency reached by stepping can be a
     * fraction of a hertz off the channel, and a channel entered by its
     * designator (118.005, whose carrier is 118.00833) is not on it at all.
     * Snapping is a no-op outside the band.
     */
    uint32_t rxFrequency = config->rxFrequency + rxTotalOffset();
    if (radioMode == OPMODE_AIRBAND)
        rxFrequency = airband_snap(rxFrequency);

    bk4819_setFrequency(&c62_bk4819, rxFrequency);

    if (flatMode(radioMode)) {
        /*
         * The DSP takes the baseband from the AF output for the whole
         * reception. M17 and TETRA set their filters on the mode change and
         * have no CTCSS.
         */
        bk4819_setAfOutput(&c62_bk4819, BK4819_AF_BYPASS);
#ifdef C62_TETRA_IF_DC_FILTER
        /*
         * TETRA receive: re-enable the RX IF DC filter (REG_7E<2:0>, default
         * 0b110) that the audio bypass mode clears. With it off, the IF DC
         * offset of this low-IF receiver reaches the demodulator as a tone at
         * the IF (16.9 kHz in 25 kHz mode), one-sided: it distorts the +3pi/4
         * symbols much more than the -3pi/4 ones. The wanted signal is centred
         * on the IF, so filtering DC at the IF input does not touch it.
         *
         * DMR used to inherit this, and it must not: TETRA decodes phase
         * differences and needs the IF DC gone, while DMR slices absolute 4FSK
         * levels and the baseband has to stay flat where the flat path leaves
         * it, exactly as in M17, which is the mode that works on this radio.
         * Keeping the two paths identical is what makes a DMR failure mean
         * something when it is compared against M17.
         */
        if (radioMode == OPMODE_TETRA)
            bk4819_maskSetRegister(&c62_bk4819, 0x7E, 0x0007, 0x0006);
#endif
    } else if ((config->rxToneEn) && (radioMode != OPMODE_AIRBAND)) {
        bk4819_enableRxCtcss(&c62_bk4819, config->rxTone);
    }

    bk4819_enableRx(&c62_bk4819);
    radioStatus = RX;

#ifdef C62_BASEBAND_DUMP
    /* The chip is configured and owns the bus: say what it really holds */
    if (baseband_dump_active()) {
        csk6codec_clearStreamMisses();
        dumpSendRegisters();
    }
#endif
}

void radio_enableTx()
{
    /*
     * Broadcast FM has no transmitter by definition: the band is receive only
     * and the mode has no microphone path. Refuse before touching the PA.
     */
    if (radioMode == OPMODE_WFM_BC)
        return;

    // TETRA has no transmitter, and neither has the airband without the
    // amplitude modulator built in
    if ((config->txDisable == 1) || (radioMode == OPMODE_TETRA))
        return;

#ifndef CONFIG_C62_AIR_TX
    if (radioMode == OPMODE_AIRBAND)
        return;
#else
    /*
     * Airband AM: the BK4819 transmits a bare carrier -- the codec's
     * microphone path is taken over by the modulator, which leaves silence for
     * the chip -- and the PA bias is driven with the voice envelope. The order
     * matters: the VCO and the PA come up first, and only then does the carrier
     * ramp start, so that the ramp is the only thing between silence and full
     * power.
     */
    if (radioMode == OPMODE_AIRBAND) {
        gpioDev_clear(LNA_VHF_EN);
        gpioDev_clear(LNA_UHF_EN);
        gpioDev_clear(PA_VHF_EN);
        gpioDev_clear(PA_UHF_EN);
        gpioDev_clear(ALC_TX_LED_EN);
        delayMs(5);

        const uint32_t freq = airband_snap(config->txFrequency);
        if ((freq < AIRBAND_FREQ_MIN) || (freq > AIRBAND_FREQ_MAX))
            return;

        /* The carrier level, from the band calibration at the TX frequency */
        const uint32_t pwr = (config->txPower < AM_CARRIER_MW)
                                 ? config->txPower
                                 : AM_CARRIER_MW;

        bk4819_setFrequency(&c62_bk4819, freq);
        if (freq < 174000000)
            gpioDev_set(PA_VHF_EN);
        else
            gpioDev_set(PA_UHF_EN);

        gpioDev_set(ALC_TX_LED_EN);
        set_tx_power(pwr);

        /* The carrier, then the envelope */
        bk4819_enableTx(&c62_bk4819);

        const uint32_t carrierPct = (txCal.loPwr > 0) ? txCal.loPwr : 33;
        air_tx_setCalibration((uint8_t)carrierPct, AM_DUTY_MIN_PCT,
                              AM_DUTY_MAX_PCT, (uint8_t)carrierPct);
        air_tx_setDepth(state.settings.am_depth);
        air_tx_key(true);

        radioStatus = TX;
        return;
    }
#endif

    gpioDev_clear(LNA_VHF_EN);
    gpioDev_clear(LNA_UHF_EN);
    gpioDev_clear(PA_VHF_EN);
    gpioDev_clear(PA_UHF_EN);
    gpioDev_clear(ALC_TX_LED_EN);

    //Before enable TX path delay to allow hardware to settle
    delayMs(5);

    if (config->txFrequency < 136000000 || config->txFrequency > 600000000)
        return;

    bk4819_setFrequency(&c62_bk4819, config->txFrequency);

    if ((radioMode != OPMODE_M17) && config->txToneEn) {
        bk4819_enableTxCtcss(&c62_bk4819, config->txTone, CTCSS_TX_GAIN);
    }

    if (config->txFrequency < 174000000) {
        gpioDev_set(PA_VHF_EN);
    } else {
        gpioDev_set(PA_UHF_EN);
    }

    gpioDev_set(ALC_TX_LED_EN);

    set_tx_power(config->txPower);

    bk4819_enableTx(&c62_bk4819);
    radioStatus = TX;
}

void radio_disableRtx()
{
#ifdef CONFIG_C62_AIR_TX
    /*
     * The airband transmitter is stopped before anything else: the modulator
     * ramps the carrier down in 16 ms and then releases the APC line, so that
     * the PA switch is not what cuts the carrier. It is a no-op when the
     * modulator is not keyed.
     */
    if (radioMode == OPMODE_AIRBAND)
        air_tx_key(false);
#endif

    bk4819_disableCtdcss(&c62_bk4819);

    gpioDev_clear(LNA_VHF_EN);
    gpioDev_clear(LNA_UHF_EN);
    gpioDev_clear(PA_VHF_EN);
    gpioDev_clear(PA_UHF_EN);
    gpioDev_clear(ALC_TX_LED_EN);

    if (radioMode == OPMODE_WFM_BC) {
        /*
         * Not leaving the mode, so the tuner stays powered and keeps its
         * calibration: only silence it and close the analog path, which is
         * the pop-suppression sequence the stock firmware uses as well.
         */
        if (wfmReady)
            BK1080_SetMute(&c62_bk1080, true);

        audio_setExtSource(false);
    } else if (radioStatus == RX) {
        // Leaving RX closes the AF output: the digital modes opened it in
        // radio_enableRx(), FM's audio path closes it too
        radio_disableAfOutput();
    }

    bk4819_disableRtx(&c62_bk4819);
    radioStatus = OFF;
}

void radio_updateConfiguration()
{
    /*
     * A reconfiguration writes REG_43 and the AF routing from the mode's
     * profile: while the analyser is open that means the sweep profile is no
     * longer in the chip, and the next sweep has to put it back.
     */
    if (specOn)
    {
        specDirty = true;

        /* The cached PLL word is only valid while nothing else tunes the chip */
        specWord = 0xFFFFFFFFU;
    }

    // TX calibration at the TX frequency, from the band its PA is in
    const struct C62BandCalib *band = &calData.uhf;
    if (config->txFrequency < 174000000)
        band = &calData.vhf;

    txCal = calibC62_interp(band, config->txFrequency);

    // Set bandwidth and deviation, only for analog FM mode
    if (config->opMode == OPMODE_FM) {
        switch (config->bandwidth) {
            case BW_6_25:
                bk4819_setBandwidth(&c62_bk4819, BK4819_BW_6K25);
                bk4819_setTxDeviation(&c62_bk4819, true, txCal.txDevNarrow);
                break;

            case BW_12_5:
                bk4819_setBandwidth(&c62_bk4819, BK4819_BW_12K5);
                bk4819_setTxDeviation(&c62_bk4819, true, txCal.txDevNarrow);
                break;

            case BW_25:
                bk4819_setBandwidth(&c62_bk4819, BK4819_BW_25K);
                bk4819_setTxDeviation(&c62_bk4819, true, txCal.txDevWide);
                break;

            default:
                break;
        }
    }

    /*
     * The AM IF filter follows the channel spacing: 1.7 kHz in 12.5 kHz mode
     * for the 8.33 kHz raster, the 6 kHz of 25 kHz mode for the 25 kHz one.
     */
    if (config->opMode == OPMODE_AIRBAND)
        bk4819_setAmReceive(&c62_bk4819, (config->bandwidth == BW_25)
                                             ? BK4819_AM_WIDE
                                             : BK4819_AM_NARROW);

    /*
     * Update VCO frequency and tuning parameters if current operating status
     * is different from OFF.
     * This is done by calling again the corresponding functions, which is safe
     * to do and avoids code duplication.
     */
    if (radioStatus == RX)
        radio_enableRx();
    if (radioStatus == TX)
        radio_enableTx();
}

/**
 * Coarse AF gain of the AM receive path.
 *
 * The chip's AGC holds the IF level in five coarse steps, but an AM detector
 * hands over the carrier amplitude, so the AF output still swings with the
 * field strength and the codec ADC is the first thing to clip on a strong
 * tower. This moves the chip's own AF gain step (REG_48<11:10>, 6 dB per step)
 * to keep the ADC between a fifth and five sixths of full scale, one step at a
 * time and no faster than AM_AF_STEP_MS, because the peak of the codec has a
 * 100 ms hold and reading it twice in that window would take two steps for one
 * level. Everything between the steps is left to the per-sample AGC.
 */
static void amAfGainStep(void)
{
    const int64_t now = k_uptime_get();
    if ((now - amAfGainTime) < AM_AF_STEP_MS)
        return;

    const uint16_t peak = csk6codec_getRxPeak();

    if ((peak > AM_AF_PEAK_HIGH) && (amAfGain1 < AM_RX_AUDIO_GAIN1_MAX)) {
        amAfGain1++;
    } else if ((peak < AM_AF_PEAK_LOW) && (amAfGain1 > 0)) {
        amAfGain1--;
    } else {
        return;
    }

    amAfGainTime = now;
    bk4819_setRxAudioGain(&c62_bk4819, amAfGain1, AM_RX_AUDIO_GAIN2);
}

rssi_t radio_getRssi()
{
    if (radioMode == OPMODE_WFM_BC) {
        /*
         * The level comes from the tuner, in dBuV, and is converted to the
         * dBm scale the S-meter and the squelch use. Its own reads are
         * throttled to 10 Hz by wfmUpdateStatus().
         */
        if (wfmReady && wfmUpdateStatus(false))
            return wfmLastDbm;

        return (rssi_t)-127;
    }

#ifdef C62_REG43_SWEEP
    /* Called by the rtx thread in RX, so it can own the BK4819 here */
    if (flatMode(radioMode) && (radioStatus == RX)) {
        int64_t now = k_uptime_get();
        bool recording = true;
#ifdef C62_BASEBAND_DUMP
        recording = baseband_dump_recording();
#endif
        if (reg43SweepStarted && recording)
            reg43SweepElapsed += now - reg43SweepLast;
        reg43SweepLast = now;
        if (!reg43SweepStarted || (reg43SweepElapsed >= C62_REG43_SWEEP_MS)) {
            if (reg43SweepStarted)
                reg43SweepIndex =
                    (reg43SweepIndex + 1) % ARRAY_SIZE(reg43Sweep);
            reg43SweepStarted = true;
            reg43SweepElapsed = 0;
            c62_rx_reg43 = reg43Sweep[reg43SweepIndex];
            bk4819_writeReg(&c62_bk4819, 0x43, c62_rx_reg43);
        }
    }
#endif
#ifdef C62_BASEBAND_DUMP
    /* Once a second: lost buffer halves and the level into the codec ADC */
    if (baseband_dump_active())
        dumpSendStats();
#endif

    /* Called by the rtx thread, in RX: the AM level loop owns REG_48 here */
    if ((radioMode == OPMODE_AIRBAND) && (radioStatus == RX))
        amAfGainStep();

    return bk4819_getRssi(&c62_bk4819);
}

enum opstatus radio_getStatus()
{
    return radioStatus;
}

extern "C" void c62_setRxOffset(int32_t hz)
{
    rxOffset = hz;
    if ((radioStatus == RX) && (config != NULL))
        bk4819_setFrequency(&c62_bk4819, config->rxFrequency + rxTotalOffset());
}

extern "C" int32_t c62_getRxOffset(void)
{
    return rxOffset;
}

/*
 * Broadcast FM receiver, for the operating mode (openrtx/src/core/wfm.c) and
 * for whatever screen ends up showing the station.  Everything below is a
 * thin pass-through to the tuner driver: the mode owns neither the chip nor
 * the bus.
 */

extern "C" bool c62_wfmPoll(wfm_status_t *st)
{
    if ((st == nullptr) || (radioMode != OPMODE_WFM_BC) || (!wfmReady))
        return false;

    if (!wfmUpdateStatus(false))
        return false;

    *st = wfmLast;
    return true;
}

/**
 * Channel raster of the broadcast receiver: 100 kHz (CCIR, the default) or
 * 50 kHz, for the countries that offset some stations (Italy, among others).
 */
extern "C" void c62_wfmSetSpacing(bool fine)
{
    wfmSpacing = fine ? BK1080_SPACE_50K : BK1080_SPACE_100K;
}

#ifdef CONFIG_TETRA_AFC
void radio_setRxOffset(int32_t hz)
{
    c62_setRxOffset(hz);
}
#endif

/* ------------------------------------------------------------------------- */
/* Spectrum analyser                                                          */
/* ------------------------------------------------------------------------- */

/*
 * The BK4819 has no I/Q output, so a spectrum is a sweep: write the PLL word of
 * a frequency (REG_39 then REG_38, 10 Hz units), let the receiver catch up with
 * it, read the RSSI (REG_67, 0.5 dB per step, dBm = value/2 - 160) and move on.
 * core/spectrum_service.c owns the raster, the storage and the timing; this is
 * only the hardware half, and it runs in the RTX thread, which is the owner of
 * the chip.
 *
 * The chip has no sweep engine of its own: its only "scan" is a frequency
 * counter (REG_32, results in REG_0D/0E) that measures the frequency of one
 * carrier above -40 dBm and takes 0.2 to 1.6 s to do it, which is a
 * measurement tool, not a spectrum. Its other scan features -- the squelch
 * interrupts of REG_02/REG_3F and the CTCSS/CDCSS scanners of REG_68/REG_69 --
 * only look at the frequency the PLL is on. So the sweep is a loop, and what
 * makes it fast is (docs/SPECTRUM.md §3):
 *
 *  - the bus: 96 us for a register access at the radio's 200 kHz, three
 *    accesses per bin, over a chip that takes 8 MHz. The analyser uses a second
 *    configuration of the same bus (spiFast) and writes the high half of the
 *    PLL word only when it changed, so a bin costs one or two accesses;
 *  - the settling: instead of a fixed sleep, poll the glitch indicator (REG_63)
 *    that saturates while the IF recovers from a retune, which is what the
 *    UV-K5 spectrum firmware does, with a bounded number of polls;
 *  - the profile: AFC off (it would pull the LO back onto a carrier and undo
 *    the bin), and optionally the AGC fixed (CONFIG_C62_SPECTRUM_AGC_FIX: the
 *    automatic loop cannot follow the bins, so the trace carries the gain of
 *    whatever the receiver was doing).
 *
 * Two profile switches, because a spectrum of 9 kHz bins is the right filter
 * for measuring and the wrong one for listening:
 *
 *  - sweep: the widest receive path the chip has (25 kHz channel mode, REG_43
 *    0x7e28: the 4.5 kHz IF filter, doubled to 9 kHz by the mode), AF muted so
 *    the bins are not heard;
 *  - park: the filter, the AFC and the AF output the mode had (and the AGC,
 *    when the fix is compiled in),
 *    restored from the words saved here, on the configured frequency. The
 *    receiver is then exactly what it was before the analyser opened, which is
 *    what lets the mode's own squelch pause the sweep with the audio working.
 */

/* Polls of the glitch gate, and how long between them, when the Kconfig has
   nothing to say (host builds). */
#ifndef CONFIG_C62_SPECTRUM_GATE_POLLS
#define CONFIG_C62_SPECTRUM_GATE_POLLS 20
#endif

/** Microseconds between two polls of the glitch indicator. */
#define SPEC_GATE_STEP_US 20U

/**
 * Consecutive points whose gate ran out of polls before the gate is turned off.
 *
 * A glitch indicator that never comes down would turn every point into the full
 * budget of polls for ever. Three points in a row is enough to tell a chip that
 * does not behave as the UV-K5's from one that is merely slow on a noisy bin,
 * and after that the sweep trusts the settling time it was given.
 */
#define SPEC_GATE_FAILS_MAX 3

/**
 * What the sweep waits when the gate has been given up on, us.
 *
 * The datasheet's worst case, which is what this driver did before there was a
 * gate: a glitch indicator that does not come down must cost speed, never
 * accuracy, so the fallback is the number the analyser used to trust.
 */
#define SPEC_GATE_FALLBACK_US 400U

/** Cycles of the core clock to microseconds, for the per-point measurement. */
static uint32_t specCyclesToUs(const uint32_t cycles)
{
    const uint64_t hz = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;

    if (hz == 0)
        return 0;

    return (uint32_t)(((uint64_t)cycles * 1000000ULL) / hz);
}

bool radio_spectrumEnter(void)
{
    if ((radioStatus != RX) || (config == nullptr))
        return false;

    if (specOn)
        return true;

    /* Remember what the mode is using before any register is touched */
    specReg43 = bk4819_readReg(&c62_bk4819, 0x43);
    specReg73 = bk4819_readReg(&c62_bk4819, 0x73);
#ifdef CONFIG_C62_SPECTRUM_AGC_FIX
    specReg7E = bk4819_readReg(&c62_bk4819, 0x7E);
#endif
    specFreq  = config->rxFrequency + rxTotalOffset();
    /* Nothing is known about what the chip holds: write both halves first */
    specWord  = 0xFFFFFFFFU;
    specOn    = true;
    specWide  = false;
    specDirty = true;
    specGateOff = false;
    specGateFails = 0;
    specGatePolls = 0;
    specPoints = 0;

    radio_spectrumSweep(true);
    return true;
}

void radio_spectrumSweep(const bool on)
{
    if (!specOn)
        return;

    if (on)
    {
        /* Already wide, and nothing else has touched the chip since */
        if (specWide && (!specDirty))
            return;

        /*
         * The widest documented receive path (the profile C62_WIDE_RX and
         * TETRA use): 4.5 kHz IF filter in the 25 kHz channel mode, about
         * 9 kHz of resolution. Narrower than the smallest step the analyser
         * offers, so two bins never see the same channel.
         */
        bk4819_setBandwidth(&c62_bk4819, BK4819_BW_25K);
        bk4819_setRfFilter(&c62_bk4819, 7, 7);
        c62_rx_reg43 = 0x7e28;
        bk4819_setAfOutput(&c62_bk4819, BK4819_AF_MUTE);

        /*
         * AFC off (REG_73<4>, application note V3 "AFC, ALC, MIC AGC"). It is
         * on by default and its range is the widest (REG_73<13:11> = 000): a
         * carrier anywhere near the bin under measurement would pull the
         * synthesizer onto itself, which moves the bin and lengthens the
         * settling it is trying to measure. Every implementation of a spectrum
         * on this chip that this fork could read disables it for the sweep.
         */
        bk4819_maskSetRegister(&c62_bk4819, 0x73, 0x0010, 0x0010);

#ifdef CONFIG_C62_SPECTRUM_AGC_FIX
        /*
         * AGC fixed (REG_7E<15>): the automatic loop cannot follow one bin per
         * 50 us, so each reading carries the gain the previous bins left behind,
         * and the gain the receiver happened to have when a sweep started
         * changes while it is parked between sweeps -- which is a trace that
         * flickers. Fixing it makes the trace a measurement at one gain, the
         * same one every sweep, which is what egzumer's spectrum does
         * (BK4819_SetAGC(false), REG_7E<15> with <14:12> = 011, the maximum).
         *
         * Off by default: <14:12> is a fixed index, not the live gain, so
         * fixing it also moves the absolute level of the trace (and a local
         * carrier strong enough to overload a 0 dB front end would flatten it),
         * and that is something only the radio can say. See docs/SPECTRUM.md
         * §3.2.
         */
        bk4819_maskSetRegister(&c62_bk4819, 0x7E, 0x8000, 0x8000);
#endif

        specWide  = true;
        specDirty = false;
    }
    else
    {
        /* The mode is using its own filters again: nothing to restore */
        if ((!specWide) && (!specDirty))
            return;

        bk4819_writeReg(&c62_bk4819, 0x43, specReg43);
        bk4819_writeReg(&c62_bk4819, 0x73, specReg73);
#ifdef CONFIG_C62_SPECTRUM_AGC_FIX
        bk4819_writeReg(&c62_bk4819, 0x7E, specReg7E);
#endif
        c62_rx_reg43 = specReg43;
        radio_enableAfOutput();
        specWide  = false;
        specDirty = false;
    }
}

int16_t radio_spectrumPoint(const uint32_t frequency, const uint32_t settleUs,
                            uint32_t *pointUs)
{
    if (!specOn)
        return (int16_t)-160;

    const uint32_t t0 = k_cycle_get_32();

    /*
     * The LNA of the C62 is switched by band: a sweep that crosses 174 MHz has
     * to move it too, or half the span is measured through the wrong front
     * end. Only when it crosses, which costs nothing on a span inside a band.
     */
    if ((frequency < 174000000U) != (specFreq < 174000000U))
    {
        gpioDev_clear(LNA_VHF_EN);
        gpioDev_clear(LNA_UHF_EN);

        if (frequency < 174000000U)
            gpioDev_set(LNA_VHF_EN);
        else
            gpioDev_set(LNA_UHF_EN);
    }

    bk4819_tuneCached(&c62_bk4819, frequency, &specWord);
    specFreq = frequency;

    /*
     * Two ways to cover the time between writing the PLL word and believing the
     * RSSI, and they are exclusive:
     *
     *  - a fixed settling time, given by the caller. Under 100 us it is a busy
     *    wait, which is exact; above that it sleeps, because the RTX thread has
     *    an audio worker to leave alone and k_usleep() rounds up by at most
     *    100 us at the 10 kHz tick;
     *  - no time at all, and the chip says when: REG_63 counts the glitches of
     *    the discriminator and saturates while the IF recovers from a retune,
     *    exactly as egzumer's spectrum GetRssi() uses it. Each poll is a
     *    register read, so the gate costs a few microseconds when the receiver
     *    is quick and the whole budget when it is not -- and the sweep records
     *    how often it ran out, so a chip on which the indicator does not behave
     *    that way stops polling and waits SPEC_GATE_FALLBACK_US instead: a gate
     *    that does not work must cost speed, never accuracy.
     */
    if (settleUs > 0)
    {
        if (settleUs <= 100U)
            k_busy_wait(settleUs);
        else
            k_usleep(settleUs);
    }
    else if (specGateOff)
    {
        k_usleep(SPEC_GATE_FALLBACK_US);
    }
    else
    {
        uint16_t polls = 0;
        bool gaveUp = false;

        while (bk4819_getGlitch(&c62_bk4819) >= 255U)
        {
            if (++polls >= (uint16_t)CONFIG_C62_SPECTRUM_GATE_POLLS)
            {
                gaveUp = true;
                break;
            }

            k_busy_wait(SPEC_GATE_STEP_US);
        }

        specGatePolls += polls;

        if (gaveUp)
        {
            if (++specGateFails >= SPEC_GATE_FAILS_MAX)
                specGateOff = true;
        }
        else
        {
            specGateFails = 0;
        }
    }

    const int16_t rssi = bk4819_getRssiFast(&c62_bk4819);

    if (pointUs != nullptr)
        *pointUs = specCyclesToUs(k_cycle_get_32() - t0);

    specPoints++;

    return rssi;
}

/**
 * What the sweep found out about the chip, for the status line.
 *
 * @param polls: average number of glitch polls per point, 0 if the gate is off.
 * @return false when the gate has been turned off for this session.
 */
bool radio_spectrumGate(uint32_t *polls)
{
    if (polls != nullptr)
        *polls = (specPoints > 0) ? (specGatePolls / specPoints) : 0;

    return (!specGateOff);
}

void radio_spectrumPark(void)
{
    if ((!specOn) || (config == nullptr))
        return;

    radio_spectrumSweep(false);

    specFreq = config->rxFrequency + rxTotalOffset();
    specWord = specFreq / 10U;
    bk4819_setFrequency(&c62_bk4819, specFreq);
}

void radio_spectrumLeave(void)
{
    if (!specOn)
        return;

    radio_spectrumPark();
    specOn = false;
}
