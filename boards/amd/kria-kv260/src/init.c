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
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <nuttx/board.h>
#include <nuttx/config.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mm/mm.h>

#include <drivers/drv_board_led.h>
#include <px4_platform_common/defines.h>
#include <px4_platform_common/init.h>

/* RSC_TRACE log ring, defined alongside the resource table in
 * arch/arm/src/zynqr5/zynqr5_rsc.c.  Registered as a SYSLOG channel from
 * zynqr5_board_initialize() below, before nx_start(), so early-boot and
 * assertion output is captured. */
void zynqr5_trace_init(void);
int zynqr5_trace_register(void);

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

/* NOTE (2026-07-30): simulator_sih is deliberately NO LONGER started from
 * board_app_initialize().  rcS starts it itself, from the SYS_HITL == 2 branch
 * that this board's airframe (1100_rc_quad_x_sih.hil) selects -- see
 * ROMFS/px4fmu_common/init.d/rcS, "start the simulator in hardware if needed".
 * Starting it here as well was redundant, and it inverted PX4's intended
 * ordering: sih came up at priority 180 before rcS had run a single line, so
 * every parameter command rcS spawns -- all of which run at priority 100 and
 * are waitpid()-ed -- competed with a 200 Hz simulation loop for the single R5
 * core for the whole of boot.  The visible symptom was rcS stalling partway
 * through (just after the px4io block) and never reaching commander.  The
 * duplicate start was also self-evident in the boot log as
 * "ERROR [module] Task already running" at exactly rcS's own sih line.
 *
 * The original justification for calling it from here -- that it was unclear
 * whether rcS ran at all on a board with no working console -- no longer holds:
 * the RSC_TRACE ring shows rcS executing well past the HITL branch.
 *
 * The historical note below is kept because it explains why sih is pinned to
 * priority 180 on this board, which is still true and still load-bearing.
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
	/* The RSC_TRACE ring occupies [0x1EEF7E00, 0x1EEFFE00) and the debug
	 * scratchpad [0x1EEFFE00, 0x1EEFFFFF]; the heap must stop below both.
	 * TRACE_DA in arch/arm/src/zynqr5/zynqr5_rsc.c has to agree with this. */
	uintptr_t end = 0x1EEF7E00u;

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
	/* Bring the trace ring up first: it is the only log sink that works
	 * without the allocator or the scheduler, so anything that goes wrong
	 * after this point has somewhere to be reported. */
	zynqr5_trace_init();

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

	/* Second rpmsg tty, registered as /dev/console: this is how PX4's own
	 * PX4_ERR/PX4_INFO output gets off the board at all.
	 *
	 * Until now there was no such path.  stdout/stderr went to /dev/console ==
	 * UART0, which has no MIO pin assigned on this carrier, so the flight
	 * stack's diagnostics were written to a dead port.  Neither of the other
	 * two plausible channels carries them either: syslog-over-rpmsg is wired
	 * up correctly but only ever sees NuttX kernel syslog() calls, and MAVLink
	 * STATUSTEXT only carries explicit mavlink_log_* calls.  Module start
	 * failures were therefore completely silent, and the only diagnostic left
	 * was inferring which modules were alive from which parameter prefixes
	 * appeared over MAVLink.
	 *
	 * Registered as a plain tty, NOT as /dev/console (isconsole=false), and
	 * only stdout/stderr are moved onto it; stdin stays on the dead UART0.
	 *
	 * The original reason given here -- that a readable stdin makes
	 * nsh_session() return on EOF, so the init task exits and is respawned --
	 * was wrong on both counts.  NuttX has no mechanism to respawn
	 * CONFIG_INIT_ENTRYPOINT (nxtask_create for it appears only in
	 * nx_start_application(), called once), and with CONFIG_NSH_CLE=y
	 * nsh_session() cannot return on a read failure at all: the CLE branch
	 * `continue`s rather than breaking.  The "31958 boots in 60 s" that
	 * motivated it was a measurement artifact -- the Linux-side reader was
	 * replaying stale bytes out of this tty's 4096-byte transmit ring, so the
	 * same ~100 lines of boot log were being counted over and over.  A
	 * breadcrumb counting entries to this function reads exactly 1.
	 *
	 * Keeping stdin off this endpoint is still the right call, just for a
	 * duller reason: a shell reading a descriptor nobody writes to has no
	 * business on the same channel as the diagnostic log.
	 *
	 * Two ordering constraints, both load-bearing:
	 *
	 *   - It must come *after* the RPMSG0 call above.  Endpoint addresses are
	 *     handed out sequentially from 0x400, and the Linux bridge hardcodes
	 *     EPT_DST = 0x401 for "rpmsg-ttyRPMSG0"; registering this first would
	 *     shift MAVLink to 0x402 and break the bridge.  This one lands on
	 *     0x402.
	 *   - It must happen here, in board_app_initialize(), rather than later.
	 *     nsh_initialize() runs boardctl(BOARDIOC_INIT) -- and hence this
	 *     function -- before nsh_consolemain() reaches nsh_initscript(), which
	 *     is where rcS actually executes.  Registering the console here is
	 *     therefore early enough to capture rcS and everything it starts,
	 *     which is the whole point.
	 *
	 * Deliberately a separate endpoint rather than setting isconsole on
	 * RPMSG0: that one carries the MAVLink byte stream, and interleaving
	 * console text into it would corrupt every frame.
	 */

	/* Point stdout and stderr at the RSC_TRACE ring.
	 *
	 * This replaces a second uart_rpmsg endpoint (/dev/ttyPX4LOG) that used to
	 * carry them.  That endpoint wedged after a few kilobytes and then replayed
	 * its transmit ring, which is worse than having no console at all: the
	 * replayed text was counted as fresh output and produced a "boot loop" that
	 * did not exist, plus a phantom count of NSH parse errors.  Removing it also
	 * returns its 2 x 16 KB of ring buffers to the heap.
	 *
	 * /dev/trace is a lock-free memcpy into DDR that Linux reads out-of-band
	 * under /sys/kernel/debug/remoteproc, so it cannot wedge, cannot block a
	 * writer, and does not need anything on the Linux side to be draining it.
	 *
	 * As before, only fds 1 and 2 move; stdin stays on the dead UART0 console
	 * so NSH's session loop still blocks on a descriptor that never produces
	 * input.  And as before this must happen here, in board_app_initialize(),
	 * because boardctl(BOARDIOC_INIT) runs before nsh_consolemain() reaches
	 * nsh_initscript() -- so rcS and everything it starts inherit these.
	 */

	if (zynqr5_trace_register() == 0) {
		int tfd = open("/dev/trace", O_WRONLY);

		if (tfd >= 0) {
			dup2(tfd, 1);
			dup2(tfd, 2);

			if (tfd > 2) {
				close(tfd);
			}

			/* Unbuffered.  /dev/trace is a memcpy so this costs nothing, and
			 * it removes two real diagnostic hazards: stdout is otherwise
			 * fully buffered (CONFIG_STDIO_BUFFER_SIZE, no line buffering
			 * since nothing calls setvbuf), so NSH's script echo lagged the
			 * execution frontier by exactly one line -- which produced a wrong
			 * conclusion about where rcS stops -- and stderr was never flushed
			 * at all, silently swallowing NSH's error messages. */

			setvbuf(stdout, NULL, _IONBF, 0);
			setvbuf(stderr, NULL, _IONBF, 0);

			dprintf(1, "\n[boot] kria-kv260 R5F: stdout/stderr -> RSC_TRACE\n");
		}
	}

#endif

#if defined(CONFIG_FS_RPMSGFS) && defined(CONFIG_ZYNQR5_RPTUN)
	/* Real, file-backed storage for parameters and sensor calibration.
	 *
	 * The R5 has no storage of its own, so PX4's parameter store was volatile:
	 * every boot logged "parameter storage empty, initializing" and calibration
	 * had to be redone.  The stopgap was the A53 mavlink bridge snooping
	 * PARAM_SET/CAL_* off the MAVLink stream and replaying it ~30 s after boot,
	 * which only ever captured what happened to cross MAVLink.
	 *
	 * rpmsgfs (fs/rpmsgfs) forwards POSIX file operations over the rpmsg link
	 * that already carries MAVLink, and px4-rpmsgfs-server on the A53 (see the
	 * Petalinux repo) executes them against a real directory on the Linux
	 * rootfs.  Mounting it on PX4_STORAGEDIR is all it takes for `param save`
	 * and `param load` to work through their ordinary path -- nothing in PX4
	 * needs a board-specific case.
	 *
	 * Three things about the placement of this call are load-bearing:
	 *
	 *   - It must come *after* uart_rpmsg_init() above.  Endpoint addresses are
	 *     handed out sequentially from 0x400 and the rpmsgfs client claims one
	 *     as soon as it binds, so mounting first would shift MAVLink off the
	 *     0x401 the Linux bridge hardcodes.  This lands on 0x402.
	 *
	 *   - It must be here, in board_app_initialize(), not in rcS.  rcS's own
	 *     `param select` is the first thing to touch the mount, so the mount has
	 *     to exist before boardctl(BOARDIOC_INIT) returns.
	 *
	 *   - It costs nothing if the A53 daemon is not up.  rpmsgfs_bind() never
	 *     talks to the remote; only the first path operation does.  timeout= is
	 *     that operation's retry budget, in 10 ms units, so timeout=100 is ten
	 *     attempts -- and each attempt costs one
	 *     CONFIG_FS_RPMSGFS_BIND_TIMEOUT_MS (250 ms), for about 2.6 s of
	 *     one-time delay with no server at all.  The two numbers have to be
	 *     sized together; see the defconfig comment for why the obvious pairing
	 *     is five minutes.  The outcome without a server is volatile parameters,
	 *     which is the old behaviour, not a boot that never finishes.
	 *
	 * No new shared-memory region is involved: every rpmsgfs message travels in
	 * the existing rpmsg buffer pool, which zynqr5_cache_enable() already maps
	 * Strongly-Ordered.  Nothing in the rptun path does cache maintenance, so
	 * anything that did need its own region would have to be added there.
	 */
	{
		/* Served by px4-rpmsgfs-server on the A53.  Its systemd unit creates
		 * this directory; keep the two in step. */
		static const char *const opts = "fs=/var/lib/px4,cpu=a53,timeout=100";
		int ret = nx_mount(NULL, PX4_STORAGEDIR, "rpmsgfs", 0, opts);

		if (ret < 0) {
			dprintf(2, "[boot] rpmsgfs mount of %s failed (%d): parameters will not persist\n",
				PX4_STORAGEDIR, ret);

		} else {
			/* Prove the A53 server is actually there, and unmount if it is not.
			 *
			 * rpmsgfs_bind() never contacts the remote, so a successful mount
			 * says nothing about whether anything is listening.  Leaving a
			 * mount that can never answer is what broke the "daemon stopped"
			 * case on hardware: rcS then drove PX4's parameter storage through
			 * a filesystem whose every operation failed, and the flight stack
			 * died with a data abort in the heap allocator.
			 *
			 * Do it as one bounded probe here instead, because "there is no
			 * storage" is a state this board has always handled correctly: an
			 * unmounted PX4_STORAGEDIR gives PX4 exactly the ENOENT it saw for
			 * months before rpmsgfs existed, so `param load-or-init` reports an
			 * empty store, uses firmware defaults, and boot completes.  That
			 * matters more than salvaging a late-starting daemon -- reaching a
			 * flyable state must not depend on A53 userspace.  (If the daemon
			 * is started late, restart the R5 to pick up storage.)
			 *
			 * The probe deliberately targets a name that does not exist.  A
			 * live server answers -ENOENT, which is a *successful* round trip;
			 * a missing one cannot answer at all and fails the transport.  Both
			 * outcomes are distinguishable only because rpmsgfs_open()/stat()
			 * now report the server's real errno -- upstream collapsed every
			 * failure to -EBADF, which also made a first boot with no
			 * parameters.bson yet look like a corrupt store to PX4.
			 *
			 * statfs() would be the tidier probe but cannot be used: rpmsgfs
			 * caches it and, before the fix in this tree, stamped its magic
			 * even on failure, so it reports success forever afterwards.
			 */
			struct stat st;

			ret = nx_stat(PX4_STORAGEDIR "/.rpmsgfs-probe", &st, 1);

			if (ret == 0 || ret == -ENOENT) {
				dprintf(1, "[boot] rpmsgfs: %s <- a53:/var/lib/px4\n", PX4_STORAGEDIR);

			} else {
				nx_umount2(PX4_STORAGEDIR, 0);
				dprintf(2, "[boot] rpmsgfs: no A53 server (%d); %s unmounted, "
					"parameters are volatile this boot\n", ret, PX4_STORAGEDIR);
			}
		}
	}
#endif

	/* mavlink is intentionally NOT started here either -- rc.serial starts it,
	 * driven by parameters set in init/rc.board_defaults: MAV_0_CONFIG 101
	 * (TELEM 1, which CONFIG_BOARD_SERIAL_TEL1 maps to /dev/ttyRPMSG0),
	 * SER_TEL1_BAUD 2000000, MAV_0_MODE 5 (config) and MAV_0_RATE 100000.
	 *
	 * It used to be started directly from here because rc.serial issues a plain
	 * "mavlink start ..." NSH command line, and PX4's module-name-to-entrypoint
	 * dispatch for NSH text commands had never been exercised on this board --
	 * a breadcrumb in mavlink_main() never fired when relying on rc.serial.
	 * That is settled now: rcS runs to completion, rc.serial executes, and its
	 * mavlink start reached the module and was correctly refused with
	 * "/dev/ttyRPMSG0 already running" precisely because this call had got there
	 * first.  Keeping both meant that error on every single boot.
	 *
	 * Doing it from rc.serial is also strictly better than hardcoding argv here:
	 * the baud, mode and datarate become parameters, and mavlink now starts
	 * after the Linux-side bridge has had a chance to attach rather than writing
	 * to /dev/ttyRPMSG0 with no peer listening.
	 */

	/* simulator_sih is intentionally NOT started here — rcS starts it from its
	 * SYS_HITL == 2 branch.  See the note near the top of this file. */

	return OK;
}
