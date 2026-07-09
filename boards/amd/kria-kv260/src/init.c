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
 *   Constrain the NuttX heap to the space between the idle-stack top and
 *   the debug scratchpad, all within the 256 KB DDR carveout.
 *
 *   Layout:
 *     [.text/.data/.bss]  : at 0x1ED00000, up to _ebss
 *     [idle stack]        : [_ebss, g_idle_topstack)  (2 KB, grows down)
 *     [heap]              : [g_idle_topstack, 0x1ED3FE00)  (~44 KB)
 *     [debug scratchpad]  : [0x1ED3FE00, 0x1ED3FFFF]
 *
 *   The default arm_allocateheap.c uses CONFIG_RAM_END which reaches past
 *   the carveout into the rpmsg vring region; hence this override.
 ****************************************************************************/

void up_allocate_heap(void **heap_start, size_t *heap_size) {
  uintptr_t start = g_idle_topstack; /* _ebss + CONFIG_IDLETHREAD_STACKSIZE */
  uintptr_t end = 0x1ED3FE00u;       /* debug scratch area starts here */

  *heap_start = (void *)start;
  *heap_size = (end > start) ? (end - start) : 0;
}

/****************************************************************************
 * Name: zynqr5_board_initialize
 *
 * Description:
 *   Called by arm_boot() after FPU and UART early init, before nx_start().
 *   No scheduler; no blocking.  Phase 1: stub.
 ****************************************************************************/

__EXPORT void zynqr5_board_initialize(void) {
  *((volatile uint32_t *)0x1ED3FFB8u) = 0xB00710A1u;
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   Called by nx_start_task() after the scheduler is running
 *   (CONFIG_BOARD_LATE_INITIALIZE=y).  Phase 1: stub.
 ****************************************************************************/

__EXPORT void board_late_initialize(void) {
#ifdef CONFIG_ZYNQR5_RPTUN
  zynqr5_rptun_initialize();
#endif
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

__EXPORT int board_app_initialize(uintptr_t arg) {
  px4_platform_init();

  drv_led_start();
  led_on(LED_BLUE);

  return OK;
}
