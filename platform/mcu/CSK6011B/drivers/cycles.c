/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file cycles.c
 * @brief Free-running cycle counter of the CSK6011B's Cortex-M33.
 *
 * The per-mode statistics (core/mode_stats.h) measure how long demodulating a
 * block costs by reading this counter around the call. DWT.CYCCNT counts core
 * cycles at the CPU clock (300 MHz, CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC), so a
 * count is 3.3 ns and one 30 ms block is 9 million of them: the wrap at 2^32
 * (14.3 s) is never in the way of a measurement, and the readers subtract in
 * 32-bit arithmetic, which handles it anyway.
 *
 * The counter needs TRCENA in DEMCR: it is switched on at the first read, so
 * nothing has to be initialised in the boot path.
 */

#include <stdint.h>

#include <zephyr/arch/arm/aarch32/cortex_m/cmsis.h>
#include <zephyr/kernel.h>

#include "core/mode_stats.h"

#ifdef CONFIG_C62_MODE_STATS

uint32_t modeStats_platformCycles(void)
{
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0) {
        /*
         * A core without a cycle counter (DWT_CTRL.NOCYCCNT) leaves CYCCNT at
         * zero: the statistics then report a cost of zero, which is honest,
         * instead of counting something else.
         */
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }

    return DWT->CYCCNT;
}

#endif /* CONFIG_C62_MODE_STATS */
