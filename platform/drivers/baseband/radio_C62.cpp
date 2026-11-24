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
#include "core/state.h"
#include "drivers/baseband/BK4819.h"
#include "drivers/audio/csk6_codec.h"
#include "radioUtils.h"
#include <stdio.h>
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

/**
 * Digital modes receive the raw discriminator through the flat audio path:
 * M17, and DMR (which transmits from the same flat path).
 */
static bool flatMode(enum opmode mode)
{
    return (mode == OPMODE_M17) || (mode == OPMODE_DMR)
           || (mode == OPMODE_TETRA) || (mode == OPMODE_NXDN);
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

    gpioDev_setMode(LNA_VHF_EN, OUTPUT_ACTIVE_LOW);
    gpioDev_setMode(LNA_UHF_EN, OUTPUT_ACTIVE_LOW);
    gpioDev_setMode(PA_VHF_EN, OUTPUT);
    gpioDev_setMode(PA_UHF_EN, OUTPUT);
    gpioDev_setMode(ALC_TX_LED_EN, OUTPUT);
}

void radio_terminate()
{
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
     * Only the RTX to speaker path opens it: FM demodulated audio for the
     * analog modes, the raw discriminator for the digital ones.
     */
    if (flatMode(radioMode))
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

    bk4819_setFrequency(&c62_bk4819, config->rxFrequency + rxTotalOffset());

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
    } else if (config->rxToneEn) {
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
    // TETRA is receive only on this radio: its modulation needs a linear PA
    if ((config->txDisable == 1) || (radioMode == OPMODE_TETRA))
        return;

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
    bk4819_disableCtdcss(&c62_bk4819);

    gpioDev_clear(LNA_VHF_EN);
    gpioDev_clear(LNA_UHF_EN);
    gpioDev_clear(PA_VHF_EN);
    gpioDev_clear(PA_UHF_EN);
    gpioDev_clear(ALC_TX_LED_EN);

    // Leaving RX closes the AF output: the digital modes opened it in
    // radio_enableRx(), FM's audio path closes it too
    if (radioStatus == RX)
        radio_disableAfOutput();

    bk4819_disableRtx(&c62_bk4819);
    radioStatus = OFF;
}

void radio_updateConfiguration()
{
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

rssi_t radio_getRssi()
{
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

#ifdef CONFIG_TETRA_AFC
void radio_setRxOffset(int32_t hz)
{
    c62_setRxOffset(hz);
}
#endif
