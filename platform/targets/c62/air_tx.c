/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "air_tx.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>

#include <csk6_cm33/include/gpt.h>
#include <csk6_cm33/include/gpt_pwm.h>
#include <csk6_cm33/include/gpt_timer.h>
#include <csk6_cm33/include/Driver_GPT_TIMER.h>

#include "core/am_tx.h"
#include "drivers/audio/csk6_codec.h"
#include "hwconfig.h"

/*
 * The APC PWM is GPT channel 3, 100 kHz: 1000 counts of period at 200 MHz with
 * the prescaler the devicetree asks for. The Zephyr PWM driver that owns it
 * converts a duty cycle to *integer percent* (pwm_csk6.c: duty_cycle is a
 * uint8_t), so through pwm_set_dt() the amplitude would have 101 levels: at
 * 60 % depth around a 33 % carrier that is 20 steps across the whole envelope,
 * which is a granular buzz and splatter either side of the channel. The timer
 * reload register is 16 bits per half period, so the last order of magnitude of
 * resolution is there for the taking: the modulator writes CHx_RELOAD directly,
 * one count per 0.1 % of duty, and leaves the Zephyr driver to configure the
 * channel (frequency, prescaler, pin, polarity) and to set the coarse carrier
 * with pwm_set_dt().
 */
#define APC_CHANNEL 3

/* Audio the codec hands over per frame: one millisecond at 48 kHz */
#define TX_BLOCK 48

/* Duty values in flight between the audio worker and the pacing timer: 8 ms,
 * eight times the block period, enough for the worker to be late by several
 * frames without the envelope stalling. */
#define DUTY_RING 64

/* Key-down ramp: 128 steps of 125 us, 16 ms, the same shape as the key-up */
#define RAMP_STEPS 128
#define RAMP_STEP_US 125

/*
 * The pacing timer is GPT channel 0, the same block the APC PWM lives in: it
 * counts the AP peripheral clock (100 MHz) and its overflow interrupt is
 * dispatched by the GPT handler that the PWM driver already installs. The HAL
 * is used directly instead of the Zephyr counter driver because that driver and
 * the PWM driver cannot be built together on this SoC (pwm_csk6.c only defines
 * the GPT init function when the counter driver is off), and what is needed
 * here is one channel, one period and one callback.
 */
#define PACE_CHANNEL 0
#define PACE_CLOCK_HZ BOARD_BOOTCLOCKRUN_AP_CMN_CFG_PCLK_CLK

static struct amTx mod;
static void *paceTimer;            /* GPT0_TIMER(), for the pacing channel */
static volatile uint16_t ring[DUTY_RING];
static volatile uint8_t ringHead;  /* written by the audio worker */
static volatile uint8_t ringTail;  /* written by the pacing interrupt */
static volatile uint16_t lastDuty; /* what the APC line holds */
static volatile bool keyed;
static bool pathOwned;             /* the txProcessor is installed */
static bool timerRunning;
static uint32_t apcPeriod = 1000;  /* counts per APC PWM period */

/* Shown once if the pacing timer is missing: see air_tx_init() */
static bool warned;

/**
 * Read the APC PWM period back from the timer, in counts, and keep it.
 *
 * The devicetree asks for 100 kHz with a prescaler of 1, which is 1000 counts
 * on this SoC, but the reload register is the truth the envelope is expressed
 * in, and reading it costs nothing. It is read when the transmitter is keyed
 * rather than at init, because the channel only holds a period once the Zephyr
 * PWM driver has been given one.
 */
static void apcRefreshPeriod(void)
{
    GPT_PWM_RESOURCES *res = (GPT_PWM_RESOURCES *)GPT0_PWM();
    uint32_t reload = res->gpt_resources->reg->CHx_RELOAD[APC_CHANNEL];
    uint32_t hi = (reload >> 16) & 0xFFFF;
    uint32_t lo = reload & 0xFFFF;

    if ((hi > 0) && (lo > 0) && ((hi + lo) < 0xFFFF))
        apcPeriod = hi + lo;
}

/**
 * Write a duty cycle to the APC channel, in counts of the PWM period.
 *
 * Mirrors what HAL_GPT_SetPWMFreqDuty() does with its integer percentage, with
 * the resolution the hardware has: the reload register holds the high and the
 * low time of the period in its two halves, and the shadow load makes the new
 * value take effect at the next period boundary instead of in the middle of
 * one, which would be a glitch in the envelope.
 *
 * @param counts: high time, in counts, 0 to apcPeriod - 1.
 */
static void apcWrite(uint16_t counts)
{
    GPT_PWM_RESOURCES *res = (GPT_PWM_RESOURCES *)GPT0_PWM();
    GPT_RegDef *reg = res->gpt_resources->reg;
    uint32_t hi = counts;
    uint32_t lo = apcPeriod - counts;

    if (hi < 1)
        hi = 1;
    if (lo < 1)
        lo = 1;
    if (hi > 1)
        hi--;
    if (lo > 1)
        lo--;

    reg->CHx_RELOAD[APC_CHANNEL] = (hi << 16) | lo;
    reg->SHADOW_LOAD = 0x101u << APC_CHANNEL;

    lastDuty = counts;
}

/**
 * Pacing tick, in interrupt context at AMTX_DUTY_RATE: take the next envelope
 * value and put it on the APC line. An empty ring holds the last value instead
 * of jumping to the carrier, which would be a click in the middle of a
 * syllable; the key-down ramp and the transmit watchdog are what end a
 * transmission, not a late audio frame.
 *
 * @param event: GPT event, unused: this channel only overflows.
 * @param param: user parameter, unused.
 */
static void paceTick(uint32_t event, void *param)
{
    (void)event;
    (void)param;

    if (!keyed)
        return;

    if (ringTail != ringHead) {
        uint16_t v = ring[ringTail];
        ringTail = (uint8_t)((ringTail + 1) % DUTY_RING);
        apcWrite(v);
    }
}

/**
 * Push one duty value from the audio worker.
 *
 * Only the pacing interrupt moves ringTail: it is the consumer, and the worker
 * used to move it too when the ring was full.  That write is a lost update --
 * the interrupt can advance the index between the worker's `next == ringTail`
 * test and its write, and the worker then moves the index back over a value
 * that was already consumed: the envelope repeats a step or skips one (a click
 * or a chirp) and, at unkey, the ramp can start from a level the PA never had.
 *
 * A full ring therefore overwrites the newest value instead of dropping the
 * oldest: being full means the timer is more than 8 ms behind, so the oldest
 * sample is already the past and the value the audio block just produced is
 * the one worth keeping.
 *
 * @param v: value.
 */
static void ringPush(uint16_t v)
{
    const uint8_t next = (uint8_t)((ringHead + 1) % DUTY_RING);

    if (next == ringTail) {
        ring[(uint8_t)((ringHead + DUTY_RING - 1) % DUTY_RING)] = v;
        return;
    }

    ring[ringHead] = v;
    ringHead = next;
}

/**
 * Processing of the microphone on its way to the BK4819, in the audio worker:
 * the voice becomes the envelope, and the chip is left with silence so that the
 * carrier it transmits is not frequency modulated.
 *
 * @param samples: first sample of the block.
 * @param count: samples in the block.
 * @param stride: distance between samples of the channel.
 */
static void txProcess(int16_t *samples, size_t count, size_t stride)
{
    static int16_t mono[TX_BLOCK];
    uint16_t duty[TX_BLOCK / AMTX_DECIM + 1];
    size_t n = (count > TX_BLOCK) ? TX_BLOCK : count;

    for (size_t i = 0; i < n; i++)
        mono[i] = samples[i * stride];

    size_t k = am_tx_process(&mod, mono, n, duty);

    for (size_t i = 0; i < k; i++)
        ringPush(duty[i]);

    /*
     * The BK4819 gets silence, not the voice: its modulator is a varactor and
     * what this transmission asks of it is a carrier with no deviation at all.
     * Zeroing the block here is also what makes the mode independent of the
     * chip's own audio filters, compander and ALC, all of which would shape the
     * envelope if it went through them.
     */
    for (size_t i = 0; i < count; i++)
        samples[i * stride] = 0;

    /* Without a pacing timer the block rate is all there is: hold the newest */
    if ((!timerRunning) && (k > 0))
        apcWrite(duty[k - 1]);
}

int air_tx_init(void)
{
    if (!device_is_ready(pwm_rf_apc.dev))
        return -ENODEV;

    struct amTxConfig cfg = am_tx_defaultConfig((uint16_t)(apcPeriod / 3));
    am_tx_init(&mod, &cfg);

    /*
     * The pacing timer: GPT channel 0, no pin, no output, one period and one
     * callback. Its overflow reaches this file through the GPT interrupt
     * handler the PWM driver installs for the whole block.
     */
    paceTimer = GPT0_TIMER();
    if (paceTimer == NULL)
        return -ENOTSUP;

    const uint32_t ticks = PACE_CLOCK_HZ / AMTX_DUTY_RATE;

    if (HAL_GPT_TimerPowerControl(paceTimer, CSK_POWER_FULL) != CSK_DRIVER_OK)
        return -ENOTSUP;

    const uint32_t control = CSK_GPT_TIMER_32_BIT_TIMER
                           | CSK_GPT_TIMER_CLKSRC_PCLK | CSK_GPT_TIMER_CLKDIV_1
                           | CSK_GPT_TIMER_RUNMODE_REPEAT
                           | CSK_GPT_TIMER_COUNTER_DOWN;

    if (HAL_GPT_TimerControl(paceTimer, control, PACE_CHANNEL) != CSK_DRIVER_OK)
        return -ENOTSUP;

    if (HAL_GPT_RegisterTimerCallback(paceTimer, PACE_CHANNEL,
                                      (CSK_GPT_SignalEvent_t)paceTick)
        != CSK_DRIVER_OK)
        return -ENOTSUP;

    if (HAL_GPT_SetTimerPeriodByCount(paceTimer, PACE_CHANNEL, ticks)
        != CSK_DRIVER_OK)
        return -ENOTSUP;

    return 0;
}

/**
 * Start or stop the pacing channel.
 *
 * @param on: true to start, false to stop.
 */
static void paceRun(bool on)
{
    if (paceTimer == NULL)
        return;

    if (on) {
        if (HAL_GPT_StartTimer(paceTimer, PACE_CHANNEL) == CSK_DRIVER_OK)
            timerRunning = true;
    } else {
        HAL_GPT_StopTimer(paceTimer, PACE_CHANNEL);
        timerRunning = false;
    }
}

void air_tx_setCalibration(uint8_t carrierPct, uint8_t minPct, uint8_t maxPct,
                           uint8_t slopePct)
{
    apcRefreshPeriod();

    uint16_t carrier = (uint16_t)((apcPeriod * carrierPct) / 100);
    uint16_t min = (uint16_t)((apcPeriod * minPct) / 100);
    uint16_t max = (uint16_t)((apcPeriod * maxPct) / 100);
    uint16_t slope = (uint16_t)((apcPeriod * slopePct) / 100);

    if (min < 1)
        min = 1;
    if (max > (apcPeriod - 1))
        max = (uint16_t)(apcPeriod - 1);

    mod.cfg.dutyCarrier = carrier;
    mod.cfg.dutyMin = min;
    mod.cfg.dutyMax = max;
    mod.cfg.slope = slope;
    am_tx_setCarrier(&mod, carrier);
    am_tx_setDepth(&mod, mod.cfg.depth);
}

void air_tx_setDepth(uint8_t percent)
{
    am_tx_setDepth(&mod, percent);
}

uint8_t air_tx_depth(void)
{
    return am_tx_getDepth(&mod);
}

void air_tx_key(bool down)
{
    if (down) {
        if (keyed)
            return;

        apcRefreshPeriod();
        am_tx_reset(&mod);

        /* The line starts at zero and the modulator ramps it up from there */
        apcWrite(0);
        am_tx_keyDown(&mod);

        ringHead = 0;
        ringTail = 0;

        csk6codec_setTxProcessor(txProcess);
        pathOwned = true;
        keyed = true;

        if ((paceTimer != NULL) && (!timerRunning)) {
            paceRun(true);
            if ((!timerRunning) && (!warned)) {
                printk("AM TX: pacing timer did not start, 1 kHz envelope\n");
                warned = true;
            }
        }
        return;
    }

    if (!keyed)
        return;

    /*
     * Key down. The ramp is generated here and not in the audio worker,
     * because by the time the mode unkeys the microphone path is already being
     * closed, and a ramp that depends on audio blocks would never finish: the
     * carrier would be cut by the PA switch instead of by the envelope. 128
     * steps of 125 us is 16 ms, the same shape as the key-up, and it is the
     * only place in the transmit path where the RTX thread blocks.
     *
     * The level the ramp starts from is lastDuty, which the pacing interrupt
     * writes: it is read with interrupts off, right after `keyed` is cleared,
     * so the ramp starts from what the APC line was really holding and not
     * from a value the interrupt changed in between.
     */
    unsigned int lockKey = irq_lock();
    keyed = false;
    uint16_t start = lastDuty;
    irq_unlock(lockKey);

    if (pathOwned) {
        csk6codec_setTxProcessor(NULL);
        pathOwned = false;
    }

    for (int i = RAMP_STEPS; i >= 0; i--) {
        apcWrite((uint16_t)(((uint32_t)start * (uint32_t)i) / RAMP_STEPS));
        k_busy_wait(RAMP_STEP_US);
    }

    apcWrite(0);

    paceRun(false);

    ringHead = 0;
    ringTail = 0;
    lastDuty = 0;
    am_tx_reset(&mod);
}

bool air_tx_busy(void)
{
    return keyed;
}

void air_tx_abort(void)
{
    /*
     * Stop the pacing interrupt first: it is the only other writer of the ring
     * indices and of lastDuty, and it returns as soon as `keyed` is false.
     * Resetting the indices under the same lock is what makes the next keying
     * start from a clean ring instead of from whatever a tick left behind.
     */
    unsigned int lockKey = irq_lock();
    keyed = false;
    ringHead = 0;
    ringTail = 0;
    irq_unlock(lockKey);

    apcWrite(0);
    am_tx_reset(&mod);

    if (pathOwned) {
        csk6codec_setTxProcessor(NULL);
        pathOwned = false;
    }

    paceRun(false);
}
