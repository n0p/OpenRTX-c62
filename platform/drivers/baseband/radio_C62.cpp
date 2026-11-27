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
#include "core/blackbox.h"
#include "core/state.h"
#include "core/wfm.h"
#include "drivers/baseband/BK4819.h"
#include "drivers/baseband/radioPlan.h"
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
 * Why the last keying was refused by this driver, as a short word the status
 * of the PC protocol reports.  "none" when the last attempt was accepted.
 * A refusal here used to be a bare `return`: the PA lines dropped, the LED
 * dark, no RF and nothing said, which from the outside is indistinguishable
 * from a broken radio (docs/TX_SAFETY.md).
 */
static const char *txRefusedReason = "none";

/*
 * CTCSS TX gain of the stock C62 firmware, REG_51 <6:0> "CTCSS/CDCSS Tx Gain1
 * Tuning" (0 = min, 127 = max) in the BK4819 notes of
 * https://github.com/egzumer/uv-k5-firmware-custom/blob/7607f0a4bd6203d1f06b70556fc1ce0d7399d6b3/driver/bk4819.c#L463-L471
 */
static constexpr uint8_t CTCSS_TX_GAIN = 65;

/*
 * The BK4819 configuration of every mode -- its channel mode, IF filters, audio
 * path, gains and deviation words -- is a table of register writes in
 * radioPlan.c, and every mode change and reconfiguration walks it.  There is no
 * per-mode constant left in this file: docs/RADIO_CONFIG.md has the section per
 * mode, and tests/unit/regplan.cpp pins every word of the table against what
 * this driver issued before the plan existed.
 *
 * What stays here is what is not a register write: the airband audio AGC and
 * its limits, the amplitude modulator's calibration and the broadcast tuner.
 */

/* Codec ADC peak, sample units, that asks for a coarser or a finer AF step */
static constexpr uint16_t AM_AF_PEAK_HIGH = 28000; // 0.85 of full scale
static constexpr uint16_t AM_AF_PEAK_LOW = 7000;   // 0.21 of full scale

/* REG_48<11:10> has four steps, 0 dB to -18 dB in 6 dB steps */
static constexpr uint8_t AM_RX_AUDIO_GAIN1_MAX = 3;

/*
 * AF Rx Gain2 of the AM path, REG_48<9:4>, which the AGC loop below never
 * moves: it re-writes gain1 with the same gain2 the airband plan set on entry.
 * The two must agree -- a difference would make the loop's first step a change
 * of the fine gain as well -- and the plan's value is in radioPlan.c.
 */
static constexpr uint8_t AM_RX_AUDIO_GAIN2 = 0x3f;

static constexpr int64_t AM_AF_STEP_MS = 300;

/*
 * Airband AM transmitter (CONFIG_C62_AIR_TX). The carrier is a BK4819
 * transmission with no audio in it: the amplitude modulation happens in the PA,
 * where the APC duty sets its bias, and core/am_tx.c turns the microphone into
 * that duty. The three numbers that describe the calibration -- the carrier
 * power the mode asks for, and the bottom and top of the linear window of the
 * bias around it -- are part of the mode's description and live with it, as
 * RADIO_PLAN_AM_CARRIER_MW and RADIO_PLAN_AM_DUTY_{MIN,MAX}_PCT in
 * radioPlan.h.  None of the three is measured: the factory calibration has no
 * point below 136.125 MHz, so the duty for 118 MHz is extrapolated, and
 * docs/TX_AIRBAND.md has the bench procedure that replaces them.
 */

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

/*
 * The options every plan is built with, from the build's macros.  They are
 * passed to the plan instead of being #ifdefs inside it so that the host test
 * reaches each variant, including the DMR one whose absence was the defect the
 * plan exists to expose.
 */
static void planOptions(struct radioPlanOptions *opt)
{
    radioPlan_defaultOptions(opt);

#ifdef C62_WIDE_RX
    opt->wideRx = true;
#else
    opt->wideRx = false;
#endif

#ifdef C62_DMR_12K5
    opt->dmr12k5 = true;
#else
    opt->dmr12k5 = false;
#endif

#ifdef C62_TETRA_IF_DC_FILTER
    opt->tetraIfDcFilter = true;
#else
    opt->tetraIfDcFilter = false;
#endif

#ifdef CONFIG_C62_SPECTRUM_AGC_FIX
    opt->spectrumAgcFix = true;
#else
    opt->spectrumAgcFix = false;
#endif

#ifdef CONFIG_C62_AIR_TX
    opt->airTx = true;
#else
    opt->airTx = false;
#endif

    /* The FM deviation words are the band calibration's, not a constant */
    opt->fmTxDevNarrow = txCal.txDevNarrow;
    opt->fmTxDevWide = txCal.txDevWide;
}

/** The channel spacing a plan is built for, when it comes from the status */
static enum bandwidth planBandwidth()
{
    return (config != nullptr) ? (enum bandwidth)config->bandwidth : BW_12_5;
}

/**
 * Walk a plan: ask the chip for exactly what the table says, in its order, and
 * keep the driver's own state in step with what is not a register.
 */
static void applyPlan(const struct radioPlan &plan)
{
    radioPlan_apply(&plan, &c62_bk4819);

    /* The word the baseband dump reports and the REG_43 sweep writes */
    if (plan.knobs.rxReg43 != 0)
        c62_rx_reg43 = plan.knobs.rxReg43;
}

/*
 * ---------------------------------------------------------------------------
 * The second transmit barrier (docs/TX_SAFETY.md).
 *
 * The RTX supervisor ends a transmission at TXW_LIMIT_MS, but it runs in the
 * main thread and reads the driver; it is not the driver. This one is: it is
 * fed by radio_enableTx() and radio_disableRtx() themselves, allows
 * TXW_MARGIN_MS more, and does not need any other thread to be alive.
 *
 * Two halves:
 *
 *  - the clock, which refuses a keying and latches the refusal until the PA
 *    has been off the air for TXW_RELEASE_MS. That half catches the caller
 *    that keeps trying: radio_enableTx() in a loop keys nothing and puts the
 *    lines back down every time;
 *  - the timer, armed at each keying for whatever is left of the bar. Its
 *    expiry submits a work item that drops the PA from the system workqueue,
 *    so a keying that nobody ever comes back to unkey still ends at 95 s.
 *
 * Nothing here resets on its own: only the release does, and a mode change
 * does not count as one.
 */
static struct txWatch paWatch;
static struct k_work   paCutWork;
static struct k_timer  paCutTimer;
static bool            paBarrierReady;

static void paCut(struct k_work *work);

static void paCutExpiry(struct k_timer *timer)
{
    (void)timer;
    k_work_submit(&paCutWork);
}

/** Milliseconds of air the driver's clock still has, 0 when it is spent. */
static uint32_t paAirLeft(uint32_t now)
{
    const uint32_t air = txw_airMs(&paWatch, now);

    return (air >= paWatch.limitMs) ? 0 : (paWatch.limitMs - air);
}

/**
 * Bring the barrier up. Idempotent: the boot path and radio_init() both ask.
 *
 * The kernel objects are initialised here and not with the K_WORK_DEFINE()
 * macros: this file is C++, and those expand to designated initialisers.
 */
static void paBarrierUp(void)
{
    if (paBarrierReady)
        return;

    k_work_init(&paCutWork, paCut);
    k_timer_init(&paCutTimer, paCutExpiry, NULL);
    txw_init(&paWatch, k_uptime_get_32(), TXW_DRIVER_LIMIT_MS);
    paBarrierReady = true;
}

/**
 * Arm the barrier for what is left of the bar.
 *
 * Called after every keying: one shot, the remaining margin. Re-arming on a
 * keying that is already keyed (radio_updateConfiguration()) restarts the same
 * deadline, which is what keeps the timer honest.
 */
static void paArm(uint32_t now)
{
    const uint32_t msLeft = paAirLeft(now);

    k_timer_start(&paCutTimer, K_MSEC(msLeft), K_NO_WAIT);
}

static void paDisarm(void)
{
    k_timer_stop(&paCutTimer);
}

/**
 * Note that the PA is on the air and arm the barrier for what is left.
 *
 * Called at the end of both keying paths of radio_enableTx() (the generic one
 * and the airband amplitude modulator), once the lines are actually up.
 */
static void paNoteKeyed(void)
{
    const uint32_t now = k_uptime_get_32();

    txRefusedReason = "none";
    paBarrierUp();
    txw_sample(&paWatch, now, true);
    paArm(now);
}

/** Note that the PA is off the air: accumulate the air, drop the deadline. */
static void paNoteOff(void)
{
    const uint32_t now = k_uptime_get_32();

    paBarrierUp();
    txw_sample(&paWatch, now, false);
    paDisarm();
}

/* Level control of the AM receive path, see the airband settings above */
static struct agc amAgc;
/* REG_48<11:10>, the coarse AF step: set from the plan on airband entry */
static uint8_t amAfGain1;
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

    /*
     * One lock around the whole power-up: the driver's frame state lives
     * between calls, so a register access from the protocol thread in the
     * middle of the init table would land in the wrong register (see
     * c62Bk1080Lock()).  A second of polling holds it, which is why the lock
     * is bounded and the other side reports a busy bus.
     */
    if (!c62Bk1080Lock())
    {
        printk("WFM: tuner bus busy, not powering up\n");
        return false;
    }

    if (!BK1080_Init(&c62_bk1080)) {
        c62Bk1080Unlock();

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

    c62Bk1080Unlock();

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

    if (c62Bk1080Lock())
    {
        BK1080_PowerOff(&c62_bk1080);
        c62Bk1080Unlock();
    }

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
    bool ok = false;

    if (c62Bk1080Lock())
    {
        ok = BK1080_ReadStatus(&c62_bk1080, &hw);
        c62Bk1080Unlock();
    }

    if (!ok) {
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

    if (c62Bk1080Lock())
    {
        BK1080_SetMute(&c62_bk1080, true);
        bool ok = BK1080_SetFrequency(&c62_bk1080, target, wfmSpacing);
        BK1080_SetMute(&c62_bk1080, !ok);
        c62Bk1080Unlock();
    }

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
    /*
     * A radio must not start up transmitting, and the first thing that proves
     * it is the read-back of the PA latch: the enable lines are driven low and
     * REG_33 is checked before the driver reads its calibration, resets the
     * codec or touches anything else.
     */
    if (!radio_paForceOff())
        printk("PA: STILL KEYED AT BOOT, booting anyway\n");

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
     * Leaving broadcast FM: the tuner has its own output on the amplifier
     * input, so it has to be powered down and the analog path closed, or it
     * keeps playing under the next mode.
     */
    if ((mode != OPMODE_WFM_BC) && wfmReady) {
        wfmLeave();
        audio_setExtSource(false);
    }

    /*
     * The chip configuration of the mode, as one list of writes in one place
     * (radioPlan.c, docs/RADIO_CONFIG.md): the FM demodulator profile that every
     * mode but airband puts back first (the AM detector's REG_13, REG_4A,
     * REG_2B<8> and REG_28 would otherwise be inherited), then the mode's own
     * audio path, channel mode, filters, deviation and gains.
     */
    struct radioPlanOptions opt;
    struct radioPlan plan;

    planOptions(&opt);
    radioPlan_build(&plan, RADIO_PLAN_STAGE_MODE, mode, planBandwidth(),
                    RADIO_PLAN_BOTH, &opt);

    if (mode == OPMODE_AIRBAND)
        bb_mark(BB_M_AIRBAND_LOAD);

    applyPlan(plan);

    /*
     * What the plan asks for that is not a register. Only the airband mode
     * receives AM, and its audio AGC is part of the receive path: installed
     * here, and uninstalled by every other mode while the path is still closed.
     */
    if (mode != OPMODE_AIRBAND)
        csk6codec_setRxProcessor(NULL);

    if (plan.knobs.amAudio) {
        amAfGain1 = plan.knobs.rxAfGain1;
        agc_init(&amAgc, NULL);
        csk6codec_setRxProcessor(amAudioProcess);
    }

    if (mode == OPMODE_WFM_BC) {
        /*
         * The tuner is powered and configured here, before the first tune, so
         * that the calibration waits of BK1080_Init are paid once.
         */
        wfmReady = wfmEnter();
        if (!wfmReady)
            printk("BK1080: no answer on the bus, broadcast FM unavailable\n");
    }

#ifdef CONFIG_C62_AIR_TX
    if (plan.knobs.amTx)
        air_tx_setDepth(state.settings.am_depth);
#endif

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
     * analog modes, the raw discriminator for the digital ones and the AM
     * detector for the airband one, as the mode's plan says.  Broadcast FM has
     * no write at all: the speaker is driven by the tuner, through the analog
     * path, and unmuting the transceiver there would mix its noise under the
     * station.
     */
    struct radioPlanOptions opt;
    struct radioPlan plan;

    planOptions(&opt);
    radioPlan_build(&plan, RADIO_PLAN_STAGE_AF, radioMode, planBandwidth(),
                    RADIO_PLAN_RX, &opt);
    applyPlan(plan);
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
    bb_mark(BB_M_RX_ON);

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

    if (radioPlan_isFlat(radioMode)) {
        /*
         * The DSP takes the baseband from the AF output for the whole
         * reception: the plan opens the raw discriminator and, for TETRA, puts
         * back the RX IF DC filter (REG_7E<2:0>) that the audio bypass clears.
         * With it off, the IF DC offset of this low-IF receiver reaches the
         * demodulator as a one-sided tone at the IF (16.9 kHz in 25 kHz mode)
         * and distorts the +3pi/4 symbols much more than the -3pi/4 ones.
         * TETRA decodes phase differences and needs it gone; DMR slices
         * absolute 4FSK levels and must not inherit it, which is why it is
         * TETRA's alone.  The digital modes have no CTCSS.
         */
        struct radioPlanOptions opt;
        struct radioPlan plan;

        planOptions(&opt);
        radioPlan_build(&plan, RADIO_PLAN_STAGE_RX, radioMode,
                        planBandwidth(), RADIO_PLAN_RX, &opt);
        applyPlan(plan);
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
    bb_mark(BB_M_TX_ON);

    /*
     * The PA is keyed only while the RTX layer says a permitted transmission is
     * in progress (rtx_txPermitted(), rtx.h): a mode change, a PTT that was not
     * released since the last one, a queued message arriving during it, or any
     * path that skipped the gate is refused here, before a single line is
     * driven. The gate is the mode's rule; this is the choke point that makes
     * it impossible to bypass.
     */
    if (!rtx_txPermitted())
    {
        printk("PA: keying refused (no permitted transmission)\n");
        return;
    }

    /*
     * The second barrier, in the driver (docs/TX_SAFETY.md): the keying clock
     * counts the air this driver has put out since the PA was last released,
     * and once it has used TXW_LIMIT_MS + TXW_MARGIN_MS it refuses to key
     * again until the radio has been idle for a release. A caller in a loop
     * gets the same answer every time, and the lines go back down with it.
     */
    paBarrierUp();
    const uint32_t now = k_uptime_get_32();
    txw_sample(&paWatch, now, radioStatus == TX);
    if (!txw_keyAllowed(&paWatch, now))
    {
        /*
         * Spent: latch it and put the lines down, whatever the caller does.
         * The line is throttled -- a mode that retries every slot would turn
         * the log into the reason the refusal cannot be seen.
         */
        static uint32_t refusedAt = 0;

        txw_tick(&paWatch, now);
        if ((uint32_t)(now - refusedAt) >= 1000)
        {
            refusedAt = now;
            printk("PA: keying refused (air %lums, barrier %lums)\n",
                   (unsigned long)txw_airMs(&paWatch, now),
                   (unsigned long)paWatch.limitMs);
        }
        radio_paForceOff();
        return;
    }

    /*
     * Broadcast FM has no transmitter by definition: the band is receive only
     * and the mode has no microphone path. Refuse before touching the PA.
     */
    if (radioMode == OPMODE_WFM_BC)
    {
        txRefusedReason = "wfmbc rx only";
        return;
    }

    // TETRA has no transmitter, and neither has the airband without the
    // amplitude modulator built in
    if ((config->txDisable == 1) || (radioMode == OPMODE_TETRA))
    {
        txRefusedReason = "tx disabled";
        return;
    }

#ifndef CONFIG_C62_AIR_TX
    if (radioMode == OPMODE_AIRBAND)
    {
        txRefusedReason = "no am modulator";
        return;
    }
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
        {
            /*
             * Same rule as the generic path below: a keying this driver does
             * not do has to say so.  A bare return here left the operator (and
             * the CPS) with the state of the last keying, which is "none", and
             * a PTT that does nothing at all.
             */
            static uint32_t refusedAt = 0;
            const uint32_t nowMs = k_uptime_get_32();

            txRefusedReason = "tx out of airband";
            if ((uint32_t)(nowMs - refusedAt) >= 1000)
            {
                refusedAt = nowMs;
                printk("PA: keying refused, TX %lu Hz outside the airband "
                       "%lu-%lu Hz\n",
                       (unsigned long)config->txFrequency,
                       (unsigned long)AIRBAND_FREQ_MIN,
                       (unsigned long)AIRBAND_FREQ_MAX);
            }
            return;
        }

        /* The carrier level, from the band calibration at the TX frequency */
        const uint32_t pwr = (config->txPower < RADIO_PLAN_AM_CARRIER_MW)
                                 ? config->txPower
                                 : RADIO_PLAN_AM_CARRIER_MW;

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
        air_tx_setCalibration((uint8_t)carrierPct, RADIO_PLAN_AM_DUTY_MIN_PCT,
                              RADIO_PLAN_AM_DUTY_MAX_PCT,
                              (uint8_t)carrierPct);
        air_tx_setDepth(state.settings.am_depth);
        air_tx_key(true);

        radioStatus = TX;
        paNoteKeyed();
        return;
    }
#endif

    gpioDev_clear(LNA_VHF_EN);
    gpioDev_clear(LNA_UHF_EN);
    gpioDev_clear(PA_VHF_EN);
    gpioDev_clear(PA_UHF_EN);
    gpioDev_clear(ALC_TX_LED_EN);

    /*
     * Nothing is driven for a power of 0: the PA would come up with no carrier
     * behind it, which is the "keys into nothing" failure this check exists to
     * end.  The gate should have refused first; this is the driver's own.
     */
    if (config->txPower == 0)
    {
        static uint32_t refusedAt = 0;
        const uint32_t nowMs = k_uptime_get_32();

        txRefusedReason = "no tx power";
        if ((uint32_t)(nowMs - refusedAt) >= 1000)
        {
            refusedAt = nowMs;
            printk("PA: keying refused, TX power is 0 mW\n");
        }
        return;
    }

    //Before enable TX path delay to allow hardware to settle
    delayMs(5);

    if (config->txFrequency < 136000000 || config->txFrequency > 600000000)
    {
        /*
         * Out of the PA's band, and loud about it (docs/TX_SAFETY.md).  This
         * used to be a bare return: the lines had already been dropped, the
         * mode saw radio_getStatus() still OFF, and the operator got a PTT
         * that did nothing -- no LED, no RF, no message.  It happens on a VFO
         * whose record carries no transmit frequency, which is why state.c
         * derives one from the receiver and this check stays as the last
         * barrier.
         */
        static uint32_t refusedAt = 0;
        const uint32_t nowMs = k_uptime_get_32();

        txRefusedReason = "tx freq out of range";
        if ((uint32_t)(nowMs - refusedAt) >= 1000)
        {
            refusedAt = nowMs;
            printk("PA: keying refused, TX %lu Hz outside 136-600 MHz\n",
                   (unsigned long)config->txFrequency);
        }
        return;
    }

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
    paNoteKeyed();
}

const char *radio_txRefusedReason(void)
{
    return txRefusedReason;
}

void radio_disableRtx()
{
    bb_mark(BB_M_TX_OFF);

    /*
     * The air ends here as far as the driver's clock is concerned: it is the
     * one place that puts the PA down, so it is the one place that tells the
     * barrier. Doing it first means a slow teardown (a DSP call, a tuner
     * power-down) is not counted as air.
     */
    paNoteOff();

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
        {
            if (c62Bk1080Lock())
            {
                BK1080_SetMute(&c62_bk1080, true);
                c62Bk1080Unlock();
            }
        }

        audio_setExtSource(false);
    } else if (radioStatus == RX) {
        // Leaving RX closes the AF output: the digital modes opened it in
        // radio_enableRx(), FM's audio path closes it too
        radio_disableAfOutput();
    }

    bk4819_disableRtx(&c62_bk4819);
    radioStatus = OFF;
}

/**
 * Read the PA enable latch back from the chip.
 *
 * REG_33 is the register that drives the BK4819 GPIOs the PA enable lines, the
 * ALC/bias and the two LNA switches hang from, so reading it back proves what
 * the chip is doing rather than what the driver believes it wrote. Bits 2 (PA
 * VHF), 3 (PA UHF) and 4 (ALC / TX LED) at zero are the transmit path off.
 *
 * @return true if the PA and its bias are off in the chip.
 */
static bool paLatchOff(void)
{
    /* The output latch of pin n is bit (0x0040 >> n) of REG_33 */
    const uint16_t txMask = (0x0040u >> 2) | (0x0040u >> 3) | (0x0040u >> 4);
    uint16_t reg;

    /*
     * A read that did not complete is not proof of anything.  Answering "not
     * off" is the safe direction: radio_paForceOff() retries and reports the
     * radio as keyed, and radio_txPathOff() never lets an unproven latch count
     * as an off one.  This used to return the contents of an uninitialised
     * stack buffer when the transfer failed, which could "prove" that a keyed
     * PA was off.
     */
    if (bk4819_readRegChecked(&c62_bk4819, 0x33, &reg) < 0)
        return false;

    return (reg & txMask) == 0;
}

/**
 * Check that nothing is driving the PA.
 *
 * @return true if the PA and its bias are off.
 */
bool radio_txPathOff(void)
{
    if (radioStatus == TX)
        return false;

    return paLatchOff();
}

/**
 * Force the transmit path off and prove it.
 *
 * Three attempts: one SPI write that got through and one that did not look
 * exactly the same from the caller, and the latch is what decides. The
 * driver's own status and its transmit clock follow the lines down.
 *
 * @return true when the latch reads back with the three bits at zero.
 */
bool radio_paForceOff(void)
{
    paBarrierUp();

    /* OUTPUT drives the pin low, whatever the chip had latched before */
    gpioDev_setMode(PA_VHF_EN, OUTPUT);
    gpioDev_setMode(PA_UHF_EN, OUTPUT);
    gpioDev_setMode(ALC_TX_LED_EN, OUTPUT);

    bool off = false;
    for (unsigned attempt = 0; attempt < 3; attempt++)
    {
        gpioDev_clear(PA_VHF_EN);
        gpioDev_clear(PA_UHF_EN);
        gpioDev_clear(ALC_TX_LED_EN);

        if (paLatchOff())
        {
            off = true;
            break;
        }
    }

    if (radioStatus == TX)
        radioStatus = OFF;

    /* The lines are down: the keying clock sees the end of the air */
    paNoteOff();

    return off;
}

/**
 * The driver barrier fired: the keying has lasted the limit plus the margin.
 *
 * Runs in the system work queue, not in the RTX thread, because the RTX thread
 * is exactly what may be stuck. The PA goes first and the rest of the teardown
 * follows the normal unkey path; the clock stays blocked, so nothing keys
 * again until the radio has been off the air for a release.
 */
static void paCut(struct k_work *work)
{
    (void)work;

    const uint32_t now = k_uptime_get_32();

    /* Latch the cut before touching anything: a re-key must be refused */
    txw_sample(&paWatch, now, true);
    txw_tick(&paWatch, now);

    radio_disableRtx();

    printk("PA: TX BARRIER %us: driver cut the PA, air=%lums cuts=%lu\n",
           (unsigned int)(TXW_DRIVER_LIMIT_MS / 1000),
           (unsigned long)txw_airMs(&paWatch, now),
           (unsigned long)paWatch.cuts);
}

struct txWatch *radio_txWatch(void)
{
    paBarrierUp();
    return &paWatch;
}

void radio_updateConfiguration()
{
    /* A mode change converges here: the chip's filters, the DSP profile and the
     * audio routing are all rewritten from it, so it is the last breadcrumb
     * before an AM mode that never comes back. */
    bb_mark(BB_M_RADIO_CONF);

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

    /*
     * The part of the configuration that depends on the channel spacing and on
     * the band calibration, and not on the mode being entered: FM's bandwidth
     * and deviation word (its mode change leaves the chip in 12.5 kHz), and the
     * AM detector's IF filter, which follows the spacing -- 1.7 kHz in 12.5 kHz
     * mode for the 8.33 kHz raster, the 6 kHz of 25 kHz mode for the 25 kHz one.
     */
    struct radioPlanOptions opt;
    struct radioPlan plan;

    planOptions(&opt);
    radioPlan_build(&plan, RADIO_PLAN_STAGE_UPDATE, (enum opmode)config->opMode,
                    planBandwidth(), RADIO_PLAN_BOTH, &opt);
    applyPlan(plan);

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
    if (radioPlan_isFlat(radioMode) && (radioStatus == RX)) {
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
         * The sweep's own profile, from the plan: the widest documented receive
         * path (4.5 kHz IF filter in the 25 kHz channel mode, about 9 kHz of
         * resolution, narrower than the smallest step the analyser offers so
         * two bins never see the same channel), AF muted so the bins are not
         * heard, AFC off (REG_73<4>: a carrier near the bin would pull the
         * synthesizer onto itself and move it) and, with
         * CONFIG_C62_SPECTRUM_AGC_FIX, the AGC fixed (REG_7E<15>).
         *
         * Fixing the AGC is off by default: REG_7E<14:12> is a fixed index and
         * not the live gain, so fixing it also moves the absolute level of the
         * trace, and a local carrier strong enough to overload a 0 dB front end
         * would flatten the trace as well -- something only the radio can say.
         * See docs/SPECTRUM.md §3.2.
         */
        struct radioPlanOptions opt;
        struct radioPlan plan;

        planOptions(&opt);
        radioPlan_build(&plan, RADIO_PLAN_STAGE_SPECTRUM, radioMode, BW_25,
                        RADIO_PLAN_RX, &opt);
        applyPlan(plan);

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
