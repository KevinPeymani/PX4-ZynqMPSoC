/****************************************************************************
 * boards/arm/zynqr5/kria-kv260/scripts/ddr.ld
 *
 * Linker script for the Kria KV260 R5F running NuttX via Linux remoteproc.
 *
 * Linux reserves a 2 MB DDR carveout at 0x1ED00000 (rproc_0_reserved in
 * system-user.dtsi).  The remoteproc driver loads ELF PT_LOAD segments
 * directly into this region, so VMA == LMA for all sections (no copy
 * required).  The size is set by the devicetree, not here: growing this
 * script beyond the DT node silently truncates the loaded image.
 *
 * Memory layout:
 *
 *   ATCM  0x00000000 – TCM trampoline vector table (R5F view; remoteproc
 *                       driver maps this to APU physical 0xFFE00000)
 *
 *   DDR   0x1ED00000 – NuttX vector table (VBAR target)
 *         0x1ED00020 – .text, .rodata, .data, .bss
 *         ...
 *         0x1EEFFFFF – top of carveout
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

    /* Must match the size of DT node rproc@1ed00000 exactly.
     * Vrings and message buffers live in separate DT nodes above 0x1EF00000
     * and must stay outside this region -- Linux writes them concurrently:
     *   0x1EF00000  rpu0vdev0vring0  16 KB
     *   0x1EF04000  rpu0vdev0vring1  16 KB
     *   0x1EF08000  rpu0vdev0buffer  1 MB  */
    ddr  (rwx) : ORIGIN = 0x1ED00000, LENGTH = 0x00200000
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

    .vectors 0x1ED00000 :
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
        KEEP(*(.init_array .init_array.*))
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

    /* Resource table consumed by Linux remoteproc to discover vrings.
     * KEEP() prevents --gc-sections from discarding the section; Linux
     * finds it by ELF section name ".resource_table". */

    .resource_table :
    {
        KEEP(*(.resource_table))
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

    /* g_idle_topstack is defined in arm_head.S as a .rodata word containing
     * IDLE_STACK_TOP = _ebss + CONFIG_IDLETHREAD_STACKSIZE.  Reading it in C
     * gives the correct idle-stack top address.  Do NOT add an absolute linker
     * symbol override here — that redirects the symbol to uninitialized DDR,
     * making C reads return garbage.
     *
     * Memory layout within the 2 MB carveout:
     *   [.text/.data/.bss]  : at ORIGIN(ddr), grows up to _ebss
     *   [idle stack]        : [_ebss, _ebss + CONFIG_IDLETHREAD_STACKSIZE)
     *   [heap]              : [g_idle_topstack, 0x1EEFFE00)  (~1.79 MB)
     *   [debug scratchpad]  : [0x1EEFFE00, 0x1EEFFFFF]       (512 B)
     * Vrings/buffers live in a separate DT carveout above 0x1EF00000.
     *
     * The scratchpad is DDR that up_allocate_heap() withholds from the heap so
     * the A53 can read R5F state with devmem.  It is the primary debug channel
     * on this board: UART1 belongs to the Linux console, UART0 has no free MIO
     * pins, and syslog-over-rpmsg is unavailable during early boot and
     * whenever rpmsg itself is what has failed.
     *
     * Two properties follow from it being outside every ELF segment, and both
     * have caused wasted debugging:
     *   - it is never zero-initialised, so a word nothing writes holds
     *     residual DRAM content, and values persist across remoteproc restarts
     *   - there is no allocator, so a word claimed by two writers produces
     *     readings that look like erratic hardware
     *
     * Allocation map -- update this when adding a word, and check it first:
     *   0x1EEFFE00  heap start                    board src/init.c
     *   0x1EEFFE04  heap size                     board src/init.c
     *   0x1EEFFE10  IPI_ISR as last read by poll  zynqr5_rptun.c
     *   0x1EEFFE14  polls that saw bit 24 set     zynqr5_rptun.c
     *   0x1EEFFE18  ipi_check() poll count        zynqr5_rptun.c
     *   0x1EEFFE1C  TRIG writes issued by poll    zynqr5_rptun.c
     *   0x1EEFFE20  IPI_IMR (offset 0x14)         zynqr5_rptun.c
     *   0x1EEFFE80  ns-announce counters (8 w)    openamp rpmsg_virtio.c
     *   0x1EEFFF00  arm_serialinit reached        zynqr5_serial.c
     *   0x1EEFFF04  last setup() uartbase         zynqr5_serial.c
     *   0x1EEFFF08  setup() call count            zynqr5_serial.c
     *   0x1EEFFF1C  0xDB marker: survived
     *               up_enable_irq(IPI)            zynqr5_rptun.c
     *   0x1EEFFF20  rptun init entry count        zynqr5_rptun.c
     *   0x1EEFFF24  0xDB marker: rptun_init       zynqr5_rptun.c
     *   0x1EEFFF28  0xDB marker: irq_attach       zynqr5_rptun.c
     *   0x1EEFFF2C  0xDB marker: IPI_IER          zynqr5_rptun.c
     *   0x1EEFFF30  0xDB marker: counters zeroed  zynqr5_rptun.c
     *   0x1EEFFF34  0xDB marker: entered          zynqr5_rptun.c
     *   0x1EEFFF38  uptime seconds                zynqr5_timerisr.c
     *               COLLIDES with board_late_initialize marker in src/init.c
     *   0x1EEFFF3C  board_late_initialize entered board src/init.c
     *   0x1EEFFF40  UART RX ISR hits              zynqr5_serial.c
     *               COLLIDES with rptun.c markers at 0x1EEFFF40-0x1EEFFF64
     *   0x1EEFFF44  UART TX ISR hits              zynqr5_serial.c
     *   0x1EEFFF48  UART last ISR value           zynqr5_serial.c
     *   0x1EEFFF4C  UART last RX byte             zynqr5_serial.c
     *   0x1EEFFF50  UART last IRQ number          zynqr5_serial.c
     *   0x1EEFFF54  rptun progress markers        drivers/rptun/rptun.c
     *   0x1EEFFF68  timer checkpoint C            zynqr5_timerisr.c
     *   0x1EEFFF6C  timer checkpoint B            zynqr5_timerisr.c
     *   0x1EEFFF70  timer checkpoint A            zynqr5_timerisr.c
     *   0x1EEFFF74  free (was: GIC/CPSR snapshots taken by rptun.c's spin
     *               loop -- removed, it read GICC_IAR from thread context and
     *               would steal interrupts from their real handlers)
     *   0x1EEFFF98  GICD_ISENABLER[2]             zynqr5_timerisr.c
     *   0x1EEFFF9C  GICD_ITARGETSR[17]            zynqr5_timerisr.c
     *   0x1EEFFFA0  GICD_TYPER                    zynqr5_irq.c
     *   0x1EEFFFA4  GICD_CTLR                     zynqr5_irq.c
     *   0x1EEFFFA8  GICC_RPR                      zynqr5_irq.c
     *               COLLIDES with nx_start marker in zynqr5_start.c
     *   0x1EEFFFAC  GICC_PMR                      zynqr5_irq.c
     *   0x1EEFFFB0  arm_boot 0xA001 markers       zynqr5_start.c
     *   0x1EEFFFB4  GICD_ISENABLER read-back      zynqr5_irq.c
     *   0x1EEFFFB8  board_app_initialize marker   board src/init.c
     *   0x1EEFFFC0  IPI snapshot (4 w)            zynqr5_rptun.c
     *               +0x08 COLLIDES with rproc state in rptun.c
     *               +0x0C COLLIDES with 0xBEEF markers in zynqr5_irq.c
     *   0x1EEFFFD0  rptun phase / GICD_IIDR       shared, see zynqr5_rptun.c
     *   0x1EEFFFD4  rptun_worker entry count      drivers/rptun/rptun.c
     *   0x1EEFFFD8  rptun_thread state            drivers/rptun/rptun.c
     *   0x1EEFFFDC  timer liveness counter        zynqr5_timerisr.c
     *   0x1EEFFFE0  rptun lifecycle step          zynqr5_rptun.c
     *   0x1EEFFFE4  kicks sent to A53             zynqr5_rptun.c
     *   0x1EEFFFE8  IPI IRQs taken                zynqr5_rptun.c
     *   0x1EEFFFEC  first notify() vqid           zynqr5_rptun.c
     *   0x1EEFFFF0  rptun_thread outer heartbeat  drivers/rptun/rptun.c
     *               (moved here from 0x1EEFFFEC, which it shared with the
     *               first-notify vqid above)
     */

    /* Debug sections */

    .stab 0 : { *(.stab) }
    .stabstr 0 : { *(.stabstr) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_info 0 : { *(.debug_info) }
    .debug_line 0 : { *(.debug_line) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_aranges 0 : { *(.debug_aranges) }
}
