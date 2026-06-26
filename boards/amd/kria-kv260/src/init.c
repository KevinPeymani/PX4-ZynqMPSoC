/****************************************************************************
 * boards/amd/kria-kv260/src/init.c
 *
 * PX4 board initialization for the AMD Kria KV260 R5F.
 *
 * board_app_initialize() is called from NSH at startup via
 * boardctl(BOARDIOC_INIT, 0).  It kicks off px4_platform_init() which
 * starts HRT, uORB, work queues, and parameter storage.
 *
 * Phase status:
 *   Phase 1 — stub; px4_platform_init() will fail until HRT exists (Phase 4)
 *   Phase 5 — add sensor/driver startup here
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

/****************************************************************************
 * LED stubs
 *
 * The KV260 PS LEDs are not memory-mapped from the R5F without a GPIO
 * driver.  These are no-ops until Phase 7 adds GPIO support.
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

/****************************************************************************
 * Name: board_peripheral_reset
 *
 * Description:
 *   Reset board peripheral power rail.  No switchable rail exists on the
 *   KV260 R5F side; this is a no-op stub required by PX4.
 ****************************************************************************/

__EXPORT void board_peripheral_reset(int ms)
{
  (void)ms;
}

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Called by NSH via boardctl(BOARDIOC_INIT, 0) on first application
 *   startup.  Initialises the PX4 platform layer (HRT, uORB, work queues,
 *   parameter store) and performs any board-specific peripheral setup.
 *
 * Returned Value:
 *   Zero (OK) on success; negated errno on failure.
 ****************************************************************************/

__EXPORT int board_app_initialize(uintptr_t arg)
{
  /* Initialise the PX4 platform layer.
   * This starts HRT, uORB, work-queue manager, and parameter storage.
   * It will fail gracefully until the HRT driver is implemented (Phase 4). */

  px4_platform_init();

  /* Illuminate the blue LED to signal application init reached.
   * This is a no-op until GPIO is wired (Phase 7). */

  drv_led_start();
  led_on(LED_BLUE);

  return OK;
}
