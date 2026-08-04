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

    /* Hot code relocated into ATCM.
     *
     * This R5F executes from external DDR with the MPU disabled (the boot stub
     * in zynqr5_tcmstart.S clears SCTLR.M) and both caches off, so every
     * instruction fetch from .text is an uncached DDR access.  The TCMs are the
     * one memory on this part that is fast regardless: they are tightly coupled
     * and zero-wait-state, and unlike DDR their latency does not depend on the
     * MPU or the caches being configured.  That makes TCM placement worthwhile
     * on its own, not merely a stand-in for enabling the caches.
     *
     * Only 104 B of the 64 KB ATCM was in use (.tcm_vectors), so the budget
     * here is ~65 KB.  Keep this list to code that is hot *per call* rather
     * than merely large; the win scales with call frequency, not size.
     *
     * Calls between here and DDR .text span ~490 MB, far beyond the ±32 MB
     * reach of ARM's bl, so the linker synthesises long-branch veneers.  Those
     * veneers live in DDR and are themselves fetched uncached, which is why a
     * function is only worth moving if its body is longer than the single
     * indirect branch a call now costs.
     *
     * Placed before .text so these input sections are claimed here rather than
     * by the *(.text .text.*) wildcard below. */

    .tcm_text : ALIGN(8)
    {
        _stcm_text = ABSOLUTE(.);

        /* PX4 hrt: hrt_absolute_time() is called on essentially every uORB
         * publish, perf counter and timeout check in the system.
         * Measured alone: load 92.5% -> 90.9%, sih min cycle 518 -> 459 us. */
        *hrt.c.obj(.text .text.*)

        /* Ekf::predictCovariance(), 15 KB and the single largest term in the
         * "ekf2: EKF update 2532 us avg" that costs ~30% of this core at
         * 120 Hz.
         *
         * Named by its mangled section rather than by object file because
         * ekf2 is a UNITY build: every EKF source lands in one
         * modules__ekf2_unity.cpp.obj (~200 KB, far too big for ATCM), so
         * *covariance.cpp.obj matches nothing in the final link even though
         * that object exists on disk.  -ffunction-sections is what makes this
         * granularity available; if the signature ever changes, this rule
         * silently stops matching, so verify with:
         *   arm-none-eabi-nm -C <elf> | grep predictCovariance
         * and expect an ATCM (0x0000xxxx) address.
         *
         * The other ~130 KB of EKF math cannot fit here at any granularity.
         * That ceiling is why the MPU/cache work matters more than this
         * section does.
         *
         * WITHHELD pending bisect -- see the note below:
         *   *(.text._ZN3Ekf17predictCovarianceERKN9estimator9imuSampleE)
         */

        /* BISECT: everything below is withheld pending a per-item test.
         * Adding these five groups at once (sih.cpp, the two uORB objects, ten
         * kernel objects, and predictCovariance) produced an R5 that loaded
         * and reported "running" but never serviced rpmsg -- Linux logged
         * repeated "Failed to kick remote" and the bridge saw
         * r5->gcs 0 msgs while the uplink arrived normally.  ATCM was 50288 B
         * of 65536, and both TCM banks are 0x10000 in the devicetree, so this
         * was not a capacity overflow.  Re-add one group at a time; a board
         * that only speaks through the transport this code brings up gives no
         * other way to attribute the fault.
         *
         *   *sih.cpp.obj(.text .text.*)
         *   *uORBDeviceNode.cpp.obj(.text .text.*)
         *   *uORBManager.cpp.obj(.text .text.*)
         *   *arm_doirq.o(.text .text.*)
         *   *irq_dispatch.o(.text .text.*)
         *   *arm_switchcontext.o(.text .text.*)
         *   *sched_addreadytorun.o(.text .text.*)
         *   *sched_removereadytorun.o(.text .text.*)
         *   *sched_processtimer.o(.text .text.*)
         *   *sched_resumescheduler.o(.text .text.*)
         *   *sched_lock.o(.text .text.*)
         *   *lib_memcpy.o(.text .text.*)
         *   *lib_memset.o(.text .text.*)
         */

        _etcm_text = ABSOLUTE(.);
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
     * finds it by ELF section name ".resource_table".
     *
     * Aligned and padded to 1 KB so it can be given its own MPU region.
     * Linux writes the RSC_VDEV status field here while this core polls it, so
     * once the D-cache is on (CONFIG_ZYNQR5_DCACHE) this must be mapped
     * non-cacheable -- otherwise the R5 can spin forever on a stale cached
     * copy, which presents as a firmware that loads, reports "running", and
     * never announces its rpmsg channels.
     *
     * An MPU region must be a power of two based on its own alignment, and
     * this table is only 208 bytes: unaligned and mid-.data (it sat at
     * 0x1EE60C58) it could not be carved out at all.  1 KB is the smallest
     * convenient region that satisfies both rules.  The symbols are exported
     * so zynqr5_start.c does not have to hardcode an address that moves
     * whenever .data changes size. */

    .resource_table : ALIGN(1024)
    {
        _sresource_table = ABSOLUTE(.);
        KEEP(*(.resource_table))
        . = ALIGN(1024);
        _eresource_table = ABSOLUTE(.);
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
     *   0x1EEFFF78  0x5C71A5A5 magic: MPU/I-cache zynqr5_start.c
     *               code ran on THIS boot (the scratchpad is never zeroed)
     *   0x1EEFFF7C  SCTLR read back after enable  zynqr5_start.c
     *               bit0 M, bit2 C (must be 0), bit12 I, bit17 BR
     *               measured 0x00E71879 = MPU on, I-cache on, D-cache off
     *   0x1EEFFF80  MPU regions programmed        zynqr5_start.c
     *               (count << 16) | log2(size); 0x00020014 = 2 x 1 MB
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
