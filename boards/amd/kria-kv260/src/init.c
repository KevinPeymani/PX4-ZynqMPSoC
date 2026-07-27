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

#ifdef CONFIG_SYSLOG_RPMSG
#include <nuttx/syslog/syslog_rpmsg.h>
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

/* Started directly for the same reason as simulator_sih_main() below:
 * this board's rc.serial (auto-generated from CONFIG_BOARD_SERIAL_TEL1,
 * see ROMFS/px4fmu_common/init.d/rcS) issues a plain "mavlink start ..."
 * NSH command line, which depends on PX4's module-name-to-entrypoint
 * dispatch table actually being wired up for NSH text commands — a path
 * nothing else on this board has ever exercised (every other module,
 * including SIH, is started via a direct C call like this one). A debug
 * breadcrumb at the top of mavlink_main() never fired when relying on
 * rc.serial alone, confirming that dispatch path — not anything
 * mavlink-specific — was the actual gap. */
#ifdef CONFIG_MODULES_MAVLINK
extern int mavlink_main(int argc, char *argv[]);
#endif

/****************************************************************************
 * Name: up_allocate_heap
 *
 * Description:
 *   Initialize the primary NuttX heap region (Region 0).  A second region
 *   is added by arm_addregion() below.
 *
 *   This override exists because the generic arm_allocateheap.c sizes the
 *   heap from CONFIG_RAM_END, which on this board runs past the end of the
 *   remoteproc carveout and into memory Linux is using concurrently.  A heap
 *   that overruns here does not fault -- the RPU has no MMU and the XMPU is
 *   unconfigured -- it silently corrupts live rpmsg traffic, so the upper
 *   bound has to be stated explicitly.
 *
 *   Layout of the 2 MB rproc@1ed00000 carveout:
 *     [.text/.data/.bss]  : 0x1ED00000 .. _ebss
 *     [idle stack]        : [_ebss, g_idle_topstack)       (2 KB)
 *     [heap region 0]     : [g_idle_topstack, 0x1EEFFE00)  (~1.79 MB)
 *     [debug scratchpad]  : [0x1EEFFE00, 0x1EEFFFFF]       (512 B)
 *
 *   Immediately above, in separate DT nodes and NOT part of this carveout:
 *     [vring0 TX]         : [0x1EF00000, 0x1EF04000)       (16 KB)
 *     [vring1 RX]         : [0x1EF04000, 0x1EF08000)       (16 KB)
 *     [rpu0vdev0buffer]   : [0x1EF08000, 0x1F008000)       (1 MB)
 *
 *   rpu0vdev0buffer is the live rpmsg message-buffer pool that this core and
 *   Linux exchange traffic through.  It must never be handed to the heap; see
 *   arm_addregion() for the bug that resulted when it was.
 ****************************************************************************/

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
	uintptr_t start = g_idle_topstack; /* _ebss + CONFIG_IDLETHREAD_STACKSIZE */
	uintptr_t end = 0x1EEFFE00u;       /* debug scratch area starts here */

	uintptr_t size = (end > start) ? (end - start) : 0;

	*heap_start = (void *)start;
	*heap_size = size;

	/* Publish the values actually handed to the memory manager.  start is
	 * derived from g_idle_topstack, which depends on link-time section sizes,
	 * so it cannot be predicted from the source: reading it back is the only
	 * way to confirm the heap really ends at 0x1EEFFE00 and has not grown into
	 * the scratchpad as .bss expanded.  These two words live inside the
	 * scratchpad itself, below every other breadcrumb, so writing them costs no
	 * heap.  See the address map in nuttx-config/scripts/script.ld. */
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
 *   Region 1 is placed on the RPU1 carveout, which needs explaining because it
 *   looks like an unrelated core's memory -- it is.  There is no free DDR
 *   adjacent to Region 0: the addresses directly above it belong to
 *   rpu0vdev0buffer, the live rpmsg message-buffer pool.  Region 1 was
 *   originally placed there, and any malloc() that spilled out of Region 0
 *   then wrote into buffers Linux was actively reading -- corrupting IPC
 *   rather than faulting, since the RPU has no MMU.
 *
 *   The RPU1 nodes are the closest thing to genuinely free DDR reachable
 *   without editing the devicetree or BOOT.BIN.  Nothing in this tree targets
 *   R5F-1; remoteproc1 is never started, and all four nodes carry no-map so
 *   Linux will never hand the memory to a process either:
 *
 *     rproc@3ef00000            0x3ef00000  256 KB  (no firmware loaded)
 *     rpu1vdev0vring0@3ef40000  0x3ef40000   16 KB  (unused)
 *     rpu1vdev0vring1@3ef44000  0x3ef44000   16 KB  (unused)
 *     rpu1vdev0buffer@3ef48000  0x3ef48000    1 MB  (unused)
 *
 *   This placement is only valid while RPU1 stays idle.  Loading any firmware
 *   onto R5F-1 via remoteproc1 must move Region 1 first, or it reproduces the
 *   original corruption against RPU1's carveout instead of RPU0's.
 ****************************************************************************/

#if CONFIG_MM_REGIONS > 1
void arm_addregion(void)
{
	/* umm_addregion() rather than kmm_addregion(): CONFIG_BUILD_FLAT means
	 * there is one unified heap.  736 KB spans the rproc node, both RPU1
	 * vrings, and part of rpu1vdev0buffer -- safe only because no firmware runs
	 * on R5F-1, so none of those nodes has an owner. */
	umm_addregion((FAR void *)0x3EF00000u, 0x000B8000u);
}
#endif

/****************************************************************************
 * Name: zynqr5_board_initialize
 *
 * Description:
 *   Called by arm_boot() after FPU and UART early init, before nx_start().
 *   The scheduler is not running, so nothing here may block.
 ****************************************************************************/

__EXPORT void zynqr5_board_initialize(void)
{
	*((volatile uint32_t *)0x1EEFFFB8u) = 0xB00710A1u;  /* reached board init */

#ifdef CONFIG_SYSLOG_RPMSG
	/* Allocate the syslog-over-rpmsg buffer here, which no other NuttX board
	 * does, because syslog_rpmsg_init() -- called later from
	 * drivers_initialize() -- never sets priv->buffer itself.  Its
	 * device_created callback is guarded by "if (priv->buffer && ...)", so with
	 * no buffer the "rpmsg-syslog" endpoint is silently never created and
	 * syslog output disappears with no error anywhere.  Only
	 * syslog_rpmsg_init_early() sets priv->buffer, and it has to run before
	 * nx_start(), which is why this cannot move to board_late_initialize().
	 *
	 * The buffer is static rather than malloc'd because the heap is not yet
	 * initialised at this point in boot.
	 */
	static char g_syslog_rpmsg_buf[4096];
	syslog_rpmsg_init_early(g_syslog_rpmsg_buf, sizeof(g_syslog_rpmsg_buf));
#endif
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   Called by nx_start_task() after the scheduler is running
 *   (CONFIG_BOARD_LATE_INITIALIZE=y).  This is the earliest point at which
 *   rptun may be brought up, since rptun_initialize() creates a thread and
 *   takes semaphores.
 ****************************************************************************/

__EXPORT void board_late_initialize(void)
{
	/* Bracket the rptun bring-up: it is the one call here that can hang
	 * (it waits on the A53) or fault, and it happens before any console or
	 * syslog output is possible. */

	*((volatile uint32_t *)0x1EEFFF3Cu) = 0xCA110001u; /* entered */
	__asm__ volatile("dsb sy" ::: "memory");
#ifdef CONFIG_ZYNQR5_RPTUN
	zynqr5_rptun_initialize();
#endif
	*((volatile uint32_t *)0x1EEFFF38u) = 0xCA110002u; /* returned */
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

#ifdef CONFIG_RPMSG_UART
/* drivers_initialize() calls this unconditionally under CONFIG_RPMSG_UART
 * (drivers/drivers_initialize.c) — every board enabling that option must
 * provide it (see boards/sim/sim/sim/src/sim_bringup.c for the reference
 * implementation, which calls uart_rpmsg_init() directly from here).
 * This board instead calls uart_rpmsg_init() manually from
 * board_app_initialize(), deliberately timed to run after rptun's ping
 * channel has already claimed address 0x400 (see the comment there) — so
 * this hook has nothing left to do. */
void rpmsg_serialinit(void)
{
}
#endif

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
	 * rpmsg_ping_test.py is back to 10/10.
	 *
	 * Tried growing this ring 4096 -> 16384/32768 (2026-07-24) as a
	 * mitigation for PX4's mavlink parameter-dump bug (mavlink_parameters.cpp
	 * advances its send index before checking the send succeeded, so a
	 * momentarily-full TX buffer permanently drops that parameter). Reverted:
	 * under real QGroundControl load, kernel logs showed
	 * "zynqmp-ipi-mbox ... Failed to kick remote" / "Try increasing
	 * MBOX_TX_QUEUE_LEN" bursts, and the parameter loss became *worse* and
	 * non-deterministic (different params missing each attempt, sometimes
	 * the whole dump failing) instead of the original deterministic
	 * COM_DL_LOSS_T/NAV_DLL_ACT-only loss. The actual bottleneck under real
	 * load looks like it's the Linux-side IPI mailbox queue depth
	 * (MBOX_TX_QUEUE_LEN, zynqmp-ipi-mailbox kernel driver), not this
	 * R5F-side ring -- investigate there before retrying this lever. */
	uart_rpmsg_init("a53", "RPMSG0", 4096, false);
#endif

#ifdef CONFIG_MODULES_MAVLINK
	{
		/* See the mavlink_main() declaration above for why this is
		 * started here instead of relying on rc.serial's NSH command
		 * line. Minimal flags — no -b/-m/-r override, just the device
		 * — to first confirm bytes flow at all before matching
		 * rc.serial's full param-driven invocation. */
		char arg0[] = "mavlink";
		char arg1[] = "start";
		char arg2[] = "-d";
		char arg3[] = "/dev/ttyRPMSG0";
		char *mavlink_argv[] = { arg0, arg1, arg2, arg3, NULL };
		mavlink_main(4, mavlink_argv);
	}
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
