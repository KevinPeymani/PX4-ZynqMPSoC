/****************************************************************************
 * boards/arm/zynqr5/kria-kv260/scripts/ddr.ld
 *
 * Linker script for the Kria KV260 R5F running NuttX via Linux remoteproc.
 *
 * Linux reserves a 1 MB DDR carveout at 0x3ED00000.  The remoteproc
 * driver loads ELF PT_LOAD segments directly into this region, so
 * VMA == LMA for all sections (no copy required).
 *
 * Memory layout:
 *
 *   ATCM  0x00000000 – TCM trampoline vector table (R5F view; remoteproc
 *                       driver maps this to APU physical 0xFFE00000)
 *
 *   DDR   0x3ED00000 – NuttX vector table (VBAR target)
 *         0x3ED00020 – .text, .rodata, .data, .bss
 *         ...
 *         0x3EDFFFFF – top of carveout
 *
 * NuttX heap:  from _ebss to end of DDR region.
 * NuttX stacks: allocated from the heap by the scheduler.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 ****************************************************************************/

MEMORY
{
    /* ATCM: R5F reset vector lives here (0x0 from R5F's perspective).
     * remoteproc maps p_paddr=0x0 to APU physical 0xFFE00000. */
    atcm (rwx) : ORIGIN = 0x00000000, LENGTH = 64K

    ddr  (rwx) : ORIGIN = 0x3ED00000, LENGTH = 0x00100000
}

ENTRY(_tcm_start)

SECTIONS
{
    /* TCM trampoline — loaded to ATCM by remoteproc, executed on R5F reset */

    .tcm_vectors 0x00000000 :
    {
        KEEP(*(.tcm_vectors))
    } > atcm

    /* DDR: NuttX vector table — target of the TCM trampoline at 0x0.
     * _vector_start and _vector_end are defined inside arm_vectortab.o
     * (armv7-r/arm_vectortab.S); do NOT redefine them here.  Redefining
     * them as linker-script symbols would satisfy the reference in
     * zynqr5_tcmstart.o without pulling arm_vectortab.o from libarch.a,
     * leaving the DDR vector table empty (--gc-sections silently drops it). */

    .vectors 0x3ED00000 :
    {
        KEEP(*(.vectors))
    } > ddr

    .text :
    {
        _stext = ABSOLUTE(.);
        *(.text .text.*)
        *(.fixup)
        *(.gnu.warning)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.t.*)
        *(.glue_7)
        *(.glue_7t)
        *(.got)
        *(.gcc_except_table)
        *(.gnu.linkonce.r.*)
        _etext = ABSOLUTE(.);
    } > ddr

    .init_section :
    {
        _sinit = ABSOLUTE(.);
        *(.init_array .init_array.*)
        _einit = ABSOLUTE(.);
    } > ddr

    .ARM.extab :
    {
        *(.ARM.extab*)
    } > ddr

    __exidx_start = ABSOLUTE(.);
    .ARM.exidx :
    {
        *(.ARM.exidx*)
    } > ddr
    __exidx_end = ABSOLUTE(.);

    /* VMA == LMA (remoteproc ELF loader places .data in DDR directly),
     * so _eronly == _sdata.  arm_head.S still performs the copy loop,
     * which is a harmless no-op when src == dst.
     */

    _eronly = ABSOLUTE(.);

    .data :
    {
        _sdata = ABSOLUTE(.);
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        CONSTRUCTORS
        . = ALIGN(4);
        _edata = ABSOLUTE(.);
    } > ddr

    .bss :
    {
        _sbss = ABSOLUTE(.);
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = ABSOLUTE(.);
    } > ddr

    /* Remainder of the carveout is available as NuttX heap */

    g_idle_topstack = ORIGIN(ddr) + LENGTH(ddr);

    /* Debug sections */

    .stab 0 : { *(.stab) }
    .stabstr 0 : { *(.stabstr) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_info 0 : { *(.debug_info) }
    .debug_line 0 : { *(.debug_line) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_aranges 0 : { *(.debug_aranges) }
}
