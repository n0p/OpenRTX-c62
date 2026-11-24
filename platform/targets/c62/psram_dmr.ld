/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DMR state in PSRAM, in a section of its own.
 *
 * The DMR package keeps its large, rarely touched state out of SRAM: the
 * demodulator's sample ring, the call log, the voice burst queue, the slot
 * state machine, the data reassembly buffers, the Tier III engine and, on the
 * transmit build, OpenDMR's decoder tables.  They are far too big for the
 * C62's 256 KB of SRAM but are written and read once per burst (or once per
 * 20 ms frame), never inside a sample or symbol loop, so the slower memory
 * costs nothing measurable.
 *
 * They live here, not in .psram_section, for one reason: the PSRAM region also
 * holds the CSK6 system heap (modules/hal/listenai/csk6_cm33/zephyr/heap), and
 * a section cannot be zeroed without also hitting whatever else shares it.
 * As a separate output section this one has exact bounds, so
 * platform_init_csk6() can zero it at boot.  NOLOAD: the loader neither copies
 * nor zeroes it.
 *
 * Every module here still rearms its own state in its reset function, which is
 * what runs when the mode is opened; the boot clear is what makes the section
 * deterministic (zeros, not whatever the PSRAM controller's calibration left).
 */
SECTION_PROLOGUE(.psram_dmr, (NOLOAD),)
{
    . = ALIGN(32);
    __psram_dmr_start = .;
    KEEP(*(.psram_dmr))
    KEEP(*(".psram_dmr.*"))
    __psram_dmr_end = .;
} GROUP_LINK_IN(PSRAMAP)
