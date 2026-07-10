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

/****************************************************************************
 * Name: up_allocate_heap
 *
 * Description:
 *   Initialize the primary NuttX heap region (Region 0).  A second region
 *   is added by arm_addregion() below after the OpenAMP vrings.
 *
 *   Layout (1 MB carveout 0x1ED00000–0x1EE00000):
 *     [.text/.data/.bss]  : 0x1ED00000 .. _ebss
 *     [idle stack]        : [_ebss, g_idle_topstack)       (2 KB)
 *     [heap region 0]     : [g_idle_topstack, 0x1ED3FE00)  (~44 KB)
 *     [debug scratchpad]  : [0x1ED3FE00, 0x1ED3FFFF]
 *     [vring0 TX]         : [0x1ED40000, 0x1ED44000)       (16 KB, DT fixed)
 *     [vring1 RX]         : [0x1ED44000, 0x1ED48000)       (16 KB, DT fixed)
 *     [heap region 1]     : [0x1ED48000, 0x1EE00000)       (~736 KB)
 *
 *   The default arm_allocateheap.c uses CONFIG_RAM_END which reaches past
 *   the carveout into the rpmsg vring region; hence this override.
 ****************************************************************************/

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
	uintptr_t start = g_idle_topstack; /* _ebss + CONFIG_IDLETHREAD_STACKSIZE */
	uintptr_t end = 0x1ED3FE00u;       /* debug scratch area starts here */

	*heap_start = (void *)start;
	*heap_size = (end > start) ? (end - start) : 0;
}

/****************************************************************************
 * Name: arm_addregion
 *
 * Description:
 *   Add the second DDR heap region to the kernel memory manager.
 *   Called by arm_initialize() when CONFIG_MM_REGIONS > 1.
 *
 *   Memory map (within the 1 MB remoteproc carveout 0x1ED00000-0x1EE00000):
 *     [g_idle_topstack, 0x1ED3FE00)  Region 0 — up_allocate_heap() above
 *     [0x1ED40000,      0x1ED44000)  rpu0vdev0vring0 (OpenAMP TX vring)
 *     [0x1ED44000,      0x1ED48000)  rpu0vdev0vring1 (OpenAMP RX vring)
 *     [0x1ED48000,      0x1EE00000)  Region 1 — added here (~736 KB)
 *
 *   The vring addresses must match the Linux DT reserved-memory nodes.
 *
 ****************************************************************************/

#if CONFIG_MM_REGIONS > 1
void arm_addregion(void)
{
	/* CONFIG_BUILD_FLAT: single unified heap, use umm_addregion */
	umm_addregion((FAR void *)0x1ED48000u, 0x1EE00000u - 0x1ED48000u);
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
	*((volatile uint32_t *)0x1ED3FFB8u) = 0xB00710A1u;
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
	*((volatile uint32_t *)0x1ED3FF3Cu) = 0xCA110001u; /* board_late_initialize entered */
	__asm__ volatile("dsb sy" ::: "memory");
#ifdef CONFIG_ZYNQR5_RPTUN
	zynqr5_rptun_initialize();
#endif
	*((volatile uint32_t *)0x1ED3FF38u) = 0xCA110002u; /* board_late_initialize returned */
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

	return OK;
}
