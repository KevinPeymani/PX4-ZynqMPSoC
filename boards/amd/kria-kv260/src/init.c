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

#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/config.h>
#include <nuttx/board.h>
#include <arch/board/board.h>

#include <px4_platform_common/init.h>
#include <drivers/drv_board_led.h>

#ifdef CONFIG_ZYNQR5_RPTUN
#  include "zynqr5_rptun.h"
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
#ifdef CONFIG_ZYNQR5_RPTUN
	zynqr5_rptun_initialize();
#endif
}

/****************************************************************************
 * LED stubs — KV260 PS LEDs are not R5F-accessible without a GPIO driver.
 ****************************************************************************/

__EXPORT void led_init(void)
{
}

__EXPORT void led_on(int led)
{
	(void)led;
}

__EXPORT void led_off(int led)
{
	(void)led;
}

__EXPORT void led_toggle(int led)
{
	(void)led;
}

/****************************************************************************
 * Name: board_peripheral_reset
 ****************************************************************************/

__EXPORT void board_peripheral_reset(int ms)
{
	(void)ms;
}

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
