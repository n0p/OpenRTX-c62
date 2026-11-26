/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Memory map of the QEMU mps2-an505 (Cortex-M33, 4 MB of RAM at 0x10000000),
 * where the bare-metal cost harnesses under tests/platform run.  The stack
 * grows down from _estack, 256 KB above the end of .bss.
 */

ENTRY(Reset_Handler)

MEMORY
{
  RAM (rwx) : ORIGIN = 0x10000000, LENGTH = 4M
}

SECTIONS
{
  .text : {
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > RAM

  .ARM.exidx : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > RAM

  _sidata = LOADADDR(.data);
  .data : {
    . = ALIGN(4);
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM

  .bss : {
    . = ALIGN(4);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  __bss_start__ = _sbss;
  __bss_end__ = _ebss;
  __end__ = _ebss;
  end = _ebss;

  . = ALIGN(8);
  . = . + 0x40000;
  _estack = .;

  /DISCARD/ : { *(.note*) *(.comment) }
}
