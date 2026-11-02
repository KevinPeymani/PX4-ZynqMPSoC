/****************************************************************************
 * boards/arm/zynqmp-rpu/kv260-rpu/scripts/ddr-lockstep.ld
 *
 * SPDX-License-Identifier: Apache-2.0
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
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* Runs the image from DDR, for programs too large for the 256 KB of
 * lockstep TCM.  The R5 resets to 0x00000000 (low vectors), so the vector
 * table stays in TCM; its entries load absolute addresses into the PC, so
 * they reach handlers anywhere in DDR.  Everything else, including the idle
 * stack and the heap after .bss, is in the 32 MiB DDR carve-out that the
 * device tree reserves for this core (rproc0).  CONFIG_RAM_START and
 * CONFIG_RAM_SIZE must describe that carve-out.
 */

MEMORY
{
  tcm (rwx) : ORIGIN = 0x00000000, LENGTH = 256K
  ddr (rwx) : ORIGIN = 0x3c000000, LENGTH = 32M
}

EXTERN(_vector_start)
ENTRY(_vector_start)

SECTIONS
{
    .vectors : {
        KEEP(*(.vectors))
    } > tcm

    /* Linux remoteproc finds the resource table by this section name, so it
     * must stay a separate output section.  KEEP stops --gc-sections from
     * dropping it, since no code references it until rptun is added.
     */

    .resource_table : {
        . = ALIGN(8);
        KEEP(*(.resource_table))
        . = ALIGN(8);
    } > tcm

    .text : {
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

    .init_section : {
        _sinit = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        _einit = ABSOLUTE(.);
    } > ddr

    .ARM.extab : {
        *(.ARM.extab*)
    } > ddr

    __exidx_start = ABSOLUTE(.);
    .ARM.exidx : {
        *(.ARM.exidx*)
    } > ddr
    __exidx_end = ABSOLUTE(.);

    _eronly = ABSOLUTE(.);

    .data : {
        _sdata = ABSOLUTE(.);
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        CONSTRUCTORS
        . = ALIGN(4);
        _edata = ABSOLUTE(.);
    } > ddr

    .bss : {
        _sbss = ABSOLUTE(.);
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = ABSOLUTE(.);
    } > ddr

}
