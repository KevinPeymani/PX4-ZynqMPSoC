/****************************************************************************
 * boards/amd/kria-kv260/src/board_config.h
 *
 * PX4 board configuration for the AMD Kria KV260 R5F.
 *
 * This header maps PX4 abstractions (serial ports, LEDs, HRT timer) to the
 * actual ZynqMPSoC hardware.  GPIO mux macros are intentionally absent:
 * the R5F has no mux driver yet; peripheral enable is handled by NuttX
 * chip drivers and the APU-side device tree.
 *
 ****************************************************************************/

#pragma once

#include <px4_platform_common/px4_config.h>
#include <nuttx/compiler.h>
#include <stdint.h>

/* Board identity ************************************************************/

#define BOARD_NAME "KRIA-KV260"

/* Serial port assignments ***************************************************
 *
 * /dev/ttyS0 → UART0 (0xFF000000) — NSH console / debug (ttyUSB0)
 * /dev/ttyS1 → UART1 (0xFF010000) — available for MAVLink / Linux side (ttyUSB1)
 *
 * UART0 is used for the R5 NSH console to avoid conflicting with U-Boot and
 * Linux which both use UART1.  ttyUSB0 = R5, ttyUSB1 = A53.
 */

#define SERIAL_PORT_CONSOLE   "/dev/ttyS0"   /* UART0 — debug/NSH */
#define SERIAL_PORT_TEL1      "/dev/ttyS1"   /* UART1 — MAVLink */

/* HRT (High-Resolution Timer) ***********************************************
 *
 * Phase 4 will implement hrt.c using TTC2 counter 0 (0xFF130000) at 1 MHz
 * (100 MHz LPD ÷ 100 prescaler).  These constants are consumed by that
 * driver; they are defined here but unused until Phase 4 is wired in.
 */

#define HRT_TTC_BASE         0xFF130000UL   /* TTC2 base address */
#define HRT_TTC_COUNTER      0             /* counter 0 within TTC2 */
#define HRT_TTC_INPUT_FREQ   100000000UL   /* 100 MHz LPD clock */
#define HRT_TTC_TARGET_FREQ  1000000UL     /* 1 MHz → 1 µs resolution */

/* LEDs **********************************************************************
 *
 * The KV260 PS-controlled LEDs are not memory-mapped from the R5F side.
 * Use the drv_board_led.h defaults; led_on/led_off in init.c are no-ops.
 */

#define BOARD_HAS_CONTROL_STATUS_LEDS  1
#define BOARD_OVERLOAD_LED             LED_RED
#define BOARD_ARMED_STATE_LED          LED_BLUE

/* Battery / ADC *************************************************************
 *
 * No ADC driver yet. Declare 0 bricks so board_common.h skips ADC channel
 * references entirely. Remove when an ADC driver is added in Phase 7.
 */

#define BOARD_NUMBER_BRICKS     0
#define BOARD_BATTERY1_V_DIV    (float)0.0f   /* no ADC */
#define BOARD_BATTERY1_A_PER_V  (float)0.0f

/* DMA **********************************************************************/

/* There is no DMA pool on the R5F, so BOARD_DMA_ALLOC_POOL_SIZE is deliberately
 * left undefined rather than defined as 0.  Every user of it is guarded by
 * "#if defined(BOARD_DMA_ALLOC_POOL_SIZE)", so defining it to zero advertised a
 * pool that has no implementation -- board_dma_alloc.c compiles its body only
 * under CONFIG_GRAN, which this board does not set -- and turned the first
 * caller into a link error instead of a no-op.  "top" was that first caller
 * (print_load.cpp calls board_get_dma_usage under exactly this guard).
 * Undefined selects the stubs in px4_platform/board_dma_alloc.h, which is what
 * "no DMA allocator" is actually spelled as.
 */

#include <px4_platform_common/board_common.h>
