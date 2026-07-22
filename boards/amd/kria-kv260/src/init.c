/****************************************************************************
 * boards/amd/kria-kv260/src/init.c
 *
 * PX4 board initialization for the AMD Kria KV260 R5F.
 *
 * zynqr5_board_initialize() is called from arm_boot() before nx_start().
 * board_late_initialize() is called after the scheduler starts.
 * board_app_initialize() is called from NSH via boardctl(BOARDIOC_INIT, 0).
 *
 ****************************************************************************/

#include "board_config.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <syslog.h>

#include <arch/board/board.h>
#include <nuttx/board.h>
#include <nuttx/config.h>
#include <nuttx/mm/mm.h>

#include <drivers/drv_board_led.h>
#include <px4_platform_common/init.h>

/* Linker-exported symbol — arm_head.S .rodata word = _ebss +
 * CONFIG_IDLETHREAD_STACKSIZE */
extern uintptr_t g_idle_topstack;

#ifdef CONFIG_ZYNQR5_RPTUN
#include "zynqr5_rptun.h"
#endif

#ifdef CONFIG_RPMSG_UART
#include <nuttx/serial/uart_rpmsg.h>
#endif

/* simulator_sih's builtin entry point (see
 * src/modules/simulation/simulator_sih/sih.cpp). Called directly from
 * board_app_initialize() below instead of relying on rc.board_defaults/rcS.
 *
 * SCHEDULING BLOCKER (2026-07-21, RESOLVED 2026-07-22): nothing at task
 * level — not nsh_main, not board_app_initialize, not this call — used to
 * ever run on this board. Root cause, fully bisected via the debug
 * scratchpad: the rptun kthread (drivers/rptun/rptun.c, CONFIG_RPTUN_PRIORITY
 * = 224) spin-polled the IPI register in a `while (nxsem_trywait(...) != OK)`
 * loop and called sched_yield() every iteration as its only "give up the
 * CPU" mechanism. sched_yield() only yields to *same-priority* ready tasks;
 * with nothing else at priority 224, it was a no-op, so the rptun thread
 * never relinquished the CPU at all. CONFIG_INIT_PRIORITY (nsh_main, and
 * therefore everything downstream including this function) is 100 — a
 * strictly lower priority — so it never got a single tick of CPU time.
 *
 * Fixed with two changes, both required:
 *   1. drivers/rptun/rptun.c: replaced sched_yield() with a real
 *      nxsig_usleep(1 ms) in both CONFIG_ZYNQR5_RPTUN spin-poll loops, so
 *      rptun actually gives up the CPU between polls.
 *   2. src/modules/simulation/simulator_sih/sih.cpp: sih was spawned at
 *      SCHED_PRIORITY_MAX (this module's default on every board), which on
 *      this target outranks rptun/HPWORK's priority-224 convention outright.
 *      Once change #1 unblocked nsh_main enough to spawn sih, sih at
 *      SCHED_PRIORITY_MAX starved rptun's IPC wake path right back (0/10
 *      rpmsg pings) — a priority inversion, not a problem with change #1
 *      itself. Fixed via a CONFIG_ZYNQR5_RPTUN-scoped compile-time override
 *      pinning sih to CONFIG_RPTUN_PRIORITY - 44 (180) on this board only;
 *      every other SIH-using target keeps SCHED_PRIORITY_MAX unchanged.
 *
 * Verified on hardware 2026-07-22: remoteproc0 reaches "running",
 * rpmsg_ping_test.py is 10/10 (avg ~39 ms RTT — up from ~0.2 ms with the old
 * always-spinning loop, a deliberate latency/correctness tradeoff, see
 * ZYNQR5_RPTUN_POLL_USEC in rptun.c), and sih's debug breadcrumb (see
 * sih.cpp) advances at ~200 Hz as expected. */
#ifdef CONFIG_MODULES_SIMULATION_SIMULATOR_SIH
extern int simulator_sih_main(int argc, char *argv[]);
#endif

/****************************************************************************
 * Name: up_allocate_heap
 *
 * Description:
 *   Initialize the primary NuttX heap region (Region 0).  A second region
 *   is added by arm_addregion() below.
 *
 *   Layout (the *actual* 2 MB rproc@1ed00000 remoteproc carveout, per the
 *   live Linux devicetree — grown from 256 KB on 2026-07-21 for PX4 module
 *   headroom; NOT the separate 1 MB rpu0vdev0buffer DT node, confirmed
 *   below):
 *     [.text/.data/.bss]  : 0x1ED00000 .. _ebss
 *     [idle stack]        : [_ebss, g_idle_topstack)       (2 KB)
 *     [heap region 0]     : [g_idle_topstack, 0x1EEFFE00)  (1876876 B =
 *                                                            ~1.79 MB,
 *                                                            confirmed via
 *                                                            the 0x1EEFFE04
 *                                                            breadcrumb on
 *                                                            hardware
 *                                                            2026-07-21; was
 *                                                            ~44 KB before
 *                                                            the grow)
 *     [debug scratchpad]  : [0x1EEFFE00, 0x1EEFFFFF]       (512 B)
 *     [vring0 TX]         : [0x1EF00000, 0x1EF04000)       (16 KB, DT fixed)
 *     [vring1 RX]         : [0x1EF04000, 0x1EF08000)       (16 KB, DT fixed)
 *     [rpu0vdev0buffer]   : [0x1EF08000, 0x1F008000)       (1 MB, DT fixed,
 *                                                            LIVE rpmsg
 *                                                            message-buffer
 *                                                            pool — NuttX
 *                                                            must NOT touch
 *                                                            this; see
 *                                                            arm_addregion())
 *
 *   The default arm_allocateheap.c uses CONFIG_RAM_END which reaches past
 *   the carveout into the rpmsg vring region; hence this override.
 ****************************************************************************/

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
	uintptr_t start = g_idle_topstack; /* _ebss + CONFIG_IDLETHREAD_STACKSIZE */
	uintptr_t end = 0x1EEFFE00u;       /* debug scratch area starts here */

	uintptr_t size = (end > start) ? (end - start) : 0;

	*heap_start = (void *)start;
	*heap_size = size;

	/* DBG (2026-07-21, Task B): log the actual values fed to kumm_initialize()
	 * so they can be compared on hardware against the intended constant
	 * 0x1EEFFE00 boundary.  0x1EEFFE00/04 are unused by every other
	 * breadcrumb in this codebase (verified via `grep -rn 1EEFFE`) and sit
	 * inside the debug scratchpad itself, not the heap. */
	*((volatile uint32_t *)0x1EEFFE00u) = (uint32_t)start;
	*((volatile uint32_t *)0x1EEFFE04u) = (uint32_t)size;
	__asm__ volatile("dsb sy" ::: "memory");
}

/****************************************************************************
 * Name: arm_addregion
 *
 * Description:
 *   Add the second DDR heap region to the kernel memory manager.
 *   Called by arm_initialize() when CONFIG_MM_REGIONS > 1.
 *
 *   Region 1 previously lived at [0x1ED48000, 0x1EE00000) — entirely inside
 *   what was then the live Linux DT reserved-memory node rpu0vdev0buffer
 *   (0x1ED48000-0x1EE48000, 1 MB, no-map), which is the actual OpenAMP/
 *   rpmsg shared message-buffer pool that Linux and this R5F actively
 *   exchange rpmsg traffic through via rptun/libmetal.  Any malloc() that
 *   spilled past Region 0 was landing inside live rpmsg buffer memory —
 *   a self-inflicted heap/IPC overlap.  Fixed (2026-07-10, before the
 *   2026-07-21 carveout grow below) by relocating Region 1 to the rpu1
 *   (second R5 core) carveout instead:
 *
 *   NOTE (2026-07-21): the addresses in the paragraph above are historical —
 *   rpu0vdev0buffer has since moved to 0x1EF08000-0x1F008000 as part of
 *   growing rproc_0_reserved from 256 KB to 2 MB (see up_allocate_heap()
 *   above and system-user.dtsi).  Region 1 itself is unaffected: it still
 *   lives on the unused rpu1 carveout below, unrelated to RPU0's growth.
 *
 *     rproc@3ef00000            0x3ef00000  256 KB  (code/data, unused —
 *                                                     no firmware loaded)
 *     rpu1vdev0vring0@3ef40000  0x3ef40000   16 KB  (unused)
 *     rpu1vdev0vring1@3ef44000  0x3ef44000   16 KB  (unused)
 *     rpu1vdev0buffer@3ef48000  0x3ef48000    1 MB  (unused)
 *
 *   This project only loads NuttX onto R5F-0 (remoteproc0); remoteproc1
 *   (r5f@1) is offline and nothing in this tree targets it.  All four rpu1
 *   nodes carry no-map, so Linux's normal allocator will never hand this
 *   memory to a process; it is the closest thing to "genuinely free" DDR
 *   reachable without touching the devicetree/BOOT.BIN.  Region 1 keeps its
 *   original size (736 KB) — only the physical location moved — to avoid
 *   changing already-verified heap capacity/behavior.
 *
 *   CAVEAT: if this board is ever extended to load a second R5F image onto
 *   RPU1 via remoteproc1, this Region 1 placement MUST move again first —
 *   it would collide with RPU1's own code/vring/buffer carveout exactly
 *   the way the original bug collided with RPU0's buffer pool.
 ****************************************************************************/

#if CONFIG_MM_REGIONS > 1
void arm_addregion(void)
{
	/* CONFIG_BUILD_FLAT: single unified heap, use umm_addregion.
	 * rpu1 (unused second R5 core) carveout: 0x3EF00000 + 736 KB. */
	umm_addregion((FAR void *)0x3EF00000u, 0x000B8000u);
}
#endif

/****************************************************************************
 * Name: zynqr5_board_initialize
 *
 * Description:
 *   Called by arm_boot() after FPU and UART early init, before nx_start().
 *   No scheduler; no blocking.  Phase 1: stub.
 ****************************************************************************/

__EXPORT void zynqr5_board_initialize(void)
{
	*((volatile uint32_t *)0x1EEFFFB8u) = 0xB00710A1u;
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   Called by nx_start_task() after the scheduler is running
 *   (CONFIG_BOARD_LATE_INITIALIZE=y).  Phase 1: stub.
 ****************************************************************************/

__EXPORT void board_late_initialize(void)
{
	*((volatile uint32_t *)0x1EEFFF3Cu) = 0xCA110001u; /* board_late_initialize entered */
	__asm__ volatile("dsb sy" ::: "memory");
#ifdef CONFIG_ZYNQR5_RPTUN
	zynqr5_rptun_initialize();
#endif
	*((volatile uint32_t *)0x1EEFFF38u) = 0xCA110002u; /* board_late_initialize returned */
	__asm__ volatile("dsb sy" ::: "memory");
}

/****************************************************************************
 * LED stubs — KV260 PS LEDs are not R5F-accessible without a GPIO driver.
 ****************************************************************************/

__EXPORT void led_init(void) {}

__EXPORT void led_on(int led) { (void)led; }

__EXPORT void led_off(int led) { (void)led; }

__EXPORT void led_toggle(int led) { (void)led; }

/****************************************************************************
 * Name: board_peripheral_reset
 ****************************************************************************/

__EXPORT void board_peripheral_reset(int ms) { (void)ms; }

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Called by NSH via boardctl(BOARDIOC_INIT, 0).  Starts the PX4 platform
 *   layer (HRT, uORB, work queues, parameter store).
 ****************************************************************************/

__EXPORT int board_app_initialize(uintptr_t arg)
{
	px4_platform_init();

	drv_led_start();
	led_on(LED_BLUE);

#ifdef CONFIG_RPMSG_UART
	/* Second named rpmsg endpoint ("rpmsg-ttyRPMSG0"), producing
	 * /dev/ttyRPMSG0 on this (R5F) side, for MAVLink <-> A53/Linux traffic.
	 * Rides the same vdev/vring pair as the existing "rpmsg-ping" channel
	 * (VIRTIO_RPMSG_F_NS already set in zynqr5_rsc.c) — no new
	 * resource-table entry needed.
	 *
	 * Deliberately called from here (board_app_initialize, i.e. from
	 * NSH's boardctl(BOARDIOC_INIT)) rather than from
	 * zynqr5_rptun_initialize() (board_late_initialize, much earlier).
	 * Both this endpoint and "rpmsg-ping" request RPMSG_ADDR_ANY, so
	 * whichever calls rpmsg_create_ept() first gets address 0x400 and the
	 * other gets 0x401. "rpmsg-ping" MUST keep 0x400 — the Linux-side
	 * regression test (~/rpmsg_ping_test.py) hardcodes
	 * RPMSG_PING_DST=1024 (0x400) for it. rptun_dev_start() (which creates
	 * "rpmsg-ping" via a direct rptun_ping_init() call) runs inside the
	 * CONFIG_RPTUN_THREAD kthread at CONFIG_RPTUN_PRIORITY (224) — a
	 * strictly higher priority than nsh_main/board_app_initialize's
	 * CONFIG_INIT_PRIORITY (100) — so by the time this NSH-driven
	 * boardctl() call runs, dev_start has already completed and
	 * "rpmsg-ping" already holds 0x400. Calling uart_rpmsg_init() here
	 * makes rpmsg_register_callback() take its "device already active"
	 * path (walk g_rptun_priv, invoke device_created immediately) instead
	 * of registering into g_rptun_cb ahead of dev_start's broadcast —
	 * confirmed via a first attempt from zynqr5_rptun_initialize() (both
	 * before AND after its rptun_initialize() call) that both put this
	 * endpoint at 0x400 and broke rpmsg-ping (0/10 pings). Verified fixed
	 * on hardware 2026-07-22: dmesg shows "creating channel rpmsg-ping
	 * addr 0x400" then "creating channel rpmsg-ttyRPMSG0 addr 0x401", and
	 * rpmsg_ping_test.py is back to 10/10. */
	uart_rpmsg_init("a53", "RPMSG0", 4096, false);
#endif

#ifdef CONFIG_MODULES_SIMULATION_SIMULATOR_SIH
	{
		/* See the simulator_sih_main() declaration above for why this is
		 * started here instead of from rc.board_defaults. */
		char arg0[] = "simulator_sih";
		char arg1[] = "start";
		char *sih_argv[] = { arg0, arg1, NULL };
		simulator_sih_main(2, sih_argv);
	}
#endif

	return OK;
}
