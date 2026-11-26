/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"
#include <pthread.h>
#include "core/ui.h"
#include "core/state.h"
#include "core/threads.h"

#ifdef __ZEPHYR__
#include <zephyr/kernel.h>
#endif
#include "core/graphics.h"
#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "interfaces/radio.h"
#include "core/event.h"
#include "rtx/rtx.h"
#include <string.h>
#include "core/utils.h"
#include "core/input.h"
#include "core/backup.h"
#include "core/gps.h"
#include "core/voicePrompts.h"
#include "core/blackbox.h"

#if defined(PLATFORM_TTWRPLUS)
#include "pmu.h"
#endif

/* Mutex for concurrent access to RTX state variable */
pthread_mutex_t rtx_mutex;

/*
 * Liveness of the UI thread.
 *
 * The RTX thread, the audio worker and the protocol thread can all be working
 * while the UI thread is stuck: that is what the aeronautical-AM freeze looked
 * like from outside (receiver working, protocol answering, keyboard and screen
 * dead). The main thread feeds the watchdog, so a UI thread that never comes
 * back used to leave the radio like that until the battery was pulled. It is
 * stamped here, once per pass, and the main loop stops feeding the watchdog
 * when the stamps stop: the reset is the safe state, because the PA enable
 * lines are off until a transmission keys them.
 */
static volatile uint32_t uiThreadBeat;
static const uint32_t    UI_THREAD_STALL_MS = 3000;

/**
 * \internal Thread managing user input and UI
 */
void *ui_threadFunc(void *arg)
{
    (void) arg;

    kbd_msg_t   kbd_msg;
    rtxStatus_t rtx_cfg = { 0 };
    bool        sync_rtx = true;
    long long   time     = 0;

    // Load initial state and update the UI
    ui_saveState();
    ui_updateGUI();

    // Keep the splash screen for one second  before rendering the new UI screen
    sleepFor(1u, 0u);
    gfx_render();

    while(state.devStatus != SHUTDOWN)
    {
        time = getTick();
        uiThreadBeat++;

        /*
         * Liveness for the post-mortem recorder (docs/BLACKBOX.md): this is
         * the thread whose freeze used to leave a radio that still received
         * and still answered the cable, so it is the one the supervisor has to
         * be able to see stop.
         */
        bb_beat(BB_BEAT_UI);

        if(input_scanKeyboard(&kbd_msg))
        {
            ui_pushEvent(EVENT_KBD, kbd_msg.value);
        }

        pthread_mutex_lock(&state_mutex);   // Lock r/w access to radio state
        ui_updateFSM(&sync_rtx);            // Update UI FSM
        ui_saveState();                     // Save local state copy
        pthread_mutex_unlock(&state_mutex); // Unlock r/w access to radio state

        vp_tick();                           // continue playing voice prompts in progress if any.

        // A quick menu setter, the remote control or any other code outside
        // the FSM may need the new configuration to reach the radio.
        if(ui_rtxSyncRequest)
        {
            ui_rtxSyncRequest = false;
            sync_rtx = true;
        }

        // If synchronization needed take mutex and update RTX configuration
        if(sync_rtx)
        {
            pthread_mutex_lock(&rtx_mutex);
            rtx_cfg.opMode      = state.channel.mode;
            rtx_cfg.bandwidth   = state.channel.bandwidth;
            rtx_cfg.rxFrequency = state.channel.rx_frequency;
            /*
             * Transmit frequency and power come from state.c, not from the
             * channel directly: on a VFO the channel carries neither, and a
             * driver handed 0 Hz refuses to key without saying so and one
             * handed 0 mW keys the PA into nothing (docs/TX_SAFETY.md).
             */
            rtx_cfg.txFrequency = state_txFrequency();
            rtx_cfg.txPower     = state_txPowerMw();
            rtx_cfg.sqlLevel    = state.settings.sqlLevel;
            rtx_cfg.rxToneEn    = state.channel.fm.rxToneEn;
            rtx_cfg.rxTone      = ctcss_tone[state.channel.fm.rxTone];
            rtx_cfg.txToneEn    = state.channel.fm.txToneEn;
            rtx_cfg.txTone      = ctcss_tone[state.channel.fm.txTone];
            rtx_cfg.toneEn      = state.tone_enabled;

            // Enable Tx if channel allows it and we are in UI main screen
            rtx_cfg.txDisable = state.channel.rx_only || state.txDisable;

            // Copy new M17 CAN, source and destination addresses
            rtx_cfg.can = state.settings.m17_can;
            rtx_cfg.canRxEn = state.settings.m17_can_rx;
            strncpy(rtx_cfg.source_address,      state.settings.callsign, 10);
            strncpy(rtx_cfg.destination_address, state.settings.m17_dest, 10);

            /*
             * DMR identity and call destination.  The radio ID goes in the
             * source field of every link control and data header, so it comes
             * from the settings and not from the channel; the destination is
             * the settings' default, which a stored channel overrides through
             * chstore_applyRtx().  Colour code and timeslot are per channel and
             * live in the same union as the FM tones, so they are only read
             * when the channel really is a DMR one.
             */
            rtx_cfg.dmrSourceId = settings_dmrIdValid(state.settings.dmr_id)
                                ? state.settings.dmr_id : 0;
            rtx_cfg.dmrDestType = state.settings.dmr_dest_type;
            rtx_cfg.dmrDestId = state.settings.dmr_dest_id;
            rtx_cfg.dmrTimeslot = settings_dmrTimeslotValid(
                                      state.settings.dmr_timeslot)
                                ? state.settings.dmr_timeslot : 1;

            /*
             * The colour code is a channel property and the UI shows it read
             * only.  The timeslot is not: it comes from the settings (the
             * quick menu changes it), and a stored channel's own slot reaches
             * there through chstore_applySettings() when the channel is
             * selected.  One source of truth, so the menu always wins.
             */
            if (state.channel.mode == OPMODE_DMR)
                rtx_cfg.dmrColourCode = state.channel.dmr.rxColorCode;

            pthread_mutex_unlock(&rtx_mutex);

            rtx_configure(&rtx_cfg);
            sync_rtx = false;
        }

        // Update UI and render on screen, if necessary
        if(ui_updateGUI() == true)
        {
            gfx_render();
        }

        // 40Hz update rate for keyboard and UI
        time += 25;
        sleepUntil(time);
    }

    ui_terminate();
    gfx_terminate();

    return NULL;
}

/**
 * \internal Thread managing the device and update the global state variable.
 */
void *main_thread(void *arg)
{
    (void) arg;

    long long time = 0;

    #if defined(CONFIG_GPS)
    const struct gpsDevice *gps = platform_initGps();
    if(gps != NULL)
        state.gpsDetected = true;
    #endif

    while(state.devStatus != SHUTDOWN)
    {
        time = getTick();

        bb_beat(BB_BEAT_MAIN);

        #if defined(PLATFORM_TTWRPLUS)
        pmu_handleIRQ();
        #endif

        // Check if power off is requested
        pthread_mutex_lock(&state_mutex);
        if(platform_pwrButtonStatus() == false)
            state.devStatus = SHUTDOWN;
        pthread_mutex_unlock(&state_mutex);

        // Run GPS task
        #if defined(CONFIG_GPS)
        gps_task(gps);
        #endif

        // Run state update task
        state_task();

        /*
         * Supervise the transmitter: unkey a PA whose owning thread is stuck in
         * a stream or in the DSP. It runs here, and not in the RTX thread, for
         * the same reason a watchdog does not live in the task it watches.
         */
        rtx_supervise();

        #if defined(PLATFORM_C62)
        /*
         * The watchdog is refreshed by this thread and nobody else: if the main
         * thread stops running, the radio is not usable and a reset is the safe
         * state (the PA enable lines are off until a transmission keys them).
         *
         * And it is refreshed only while the UI thread is alive. A UI thread
         * parked for ever leaves a radio that still receives and still answers
         * the cable, which looks like a working radio to everything except the
         * operator: no key does anything and there is no way back. Stopping the
         * feed turns that into a reset, and the reset comes up in receive.
         */
        static uint32_t lastBeat;
        static uint32_t beatTime;
        static bool     stalled;

        uint32_t beat = uiThreadBeat;
        if (beat != lastBeat) {
            lastBeat = beat;
            beatTime = (uint32_t)getTick();
            stalled = false;
        } else if ((!stalled)
                   && (((uint32_t)getTick() - beatTime) > UI_THREAD_STALL_MS)) {
            stalled = true;
            printf("UI: thread stalled %u ms, watchdog left to reset\r\n",
                   (unsigned)UI_THREAD_STALL_MS);
        }

        if (!stalled)
            platform_wdtFeed();

#ifdef C62_PC_PROTO
        /* The accessory-port necklace, in platform/targets/c62/pc_proto_c62.c.
           It only exists with the protocol: a build without it links the same
           main loop and has no port to report. */
        void c62_portNecklace(void);
        c62_portNecklace();
#endif
        #else
        (void)UI_THREAD_STALL_MS;
        #endif

        // Run this loop once every 5ms
        time += 5;
        sleepUntil(time);
    }

    /*
     * Shutdown, not a hang: the threads this supervisor watches are about to
     * stop on purpose, so stop watching before they do.
     */
    bb_suspend();
    bb_mark(BB_M_TERM);

    return NULL;
}

/**
 * \internal Thread for RTX management.
 */
void *rtx_threadFunc(void *arg)
{
    (void) arg;

    rtx_init(&rtx_mutex);

    while(state.devStatus == RUNNING)
    {
        rtx_task();

#ifdef __ZEPHYR__
        /*
         * The RTX loop has no sleep of its own: it yields only when something
         * inside blocks, and a mode whose update() never blocks keeps the CPU
         * for as long as it is the running thread at its priority.  Yielding
         * here costs nothing when nothing else of the same priority is ready
         * and hands the CPU to the UI when something is (docs/BLACKBOX.md).
         */
        k_yield();
#endif
    }

    rtx_terminate();

    return NULL;
}

/**
 * \internal This function creates all the system tasks and mutexes.
 */
void create_threads()
{
    // Create RTX state mutex
    pthread_mutex_init(&rtx_mutex, NULL);

    // Create rtx radio thread
    pthread_attr_t rtx_attr;
    pthread_attr_init(&rtx_attr);

    #ifndef __ZEPHYR__
    pthread_attr_setstacksize(&rtx_attr, RTX_THREAD_STKSIZE);
    #else
    void *rtx_thread_stack = malloc(RTX_THREAD_STKSIZE * sizeof(uint8_t));
    pthread_attr_setstack(&rtx_attr, rtx_thread_stack, RTX_THREAD_STKSIZE);
    #endif

    #ifdef _MIOSIX
    // Max priority for RTX thread when running with miosix rtos
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&rtx_attr, &param);
    #endif

    pthread_t rtx_thread;
    pthread_create(&rtx_thread, &rtx_attr, rtx_threadFunc, NULL);

    // Create UI thread
    pthread_attr_t ui_attr;
    pthread_attr_init(&ui_attr);

    #ifndef __ZEPHYR__
    pthread_attr_setstacksize(&ui_attr, UI_THREAD_STKSIZE);
    #else
    void *ui_thread_stack = malloc(UI_THREAD_STKSIZE * sizeof(uint8_t));
    pthread_attr_setstack(&ui_attr, ui_thread_stack, UI_THREAD_STKSIZE);
    #endif

    pthread_t ui_thread;
    pthread_create(&ui_thread, &ui_attr, ui_threadFunc, NULL);
}
