/****************************************************************************
 * boards/amd/kria-kv260/nuttx-config/include/board.h
 *
 * Board definitions for the AMD Kria KV260 Vision AI Starter Kit,
 * R5F (RPU) side, running PX4 via Linux remoteproc.
 *
 * Clocks are already configured by the APU/FSBL when arm_boot() is called;
 * no PLL or clock init is needed here.
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

#ifndef __BOARDS_AMD_KRIA_KV260_NUTTX_CONFIG_INCLUDE_BOARD_H
#define __BOARDS_AMD_KRIA_KV260_NUTTX_CONFIG_INCLUDE_BOARD_H

#include <nuttx/config.h>

/* CPU frequency *************************************************************
 *
 * The R5F runs at 533 MHz (set by the APU before releasing the RPU).
 */

#define BOARD_CPU_FREQUENCY  533000000  /* 533 MHz */

/* LPD reference clock *******************************************************
 *
 * TTC and UART reference clock from the LPD (Low-Power Domain).
 * CNTFRQ is written to 100 MHz by the APU firmware; zynqr5_timerisr.c
 * reads it from CP15, so this is informational.
 */

#define BOARD_LPD_FREQUENCY  100000000  /* 100 MHz */

/* TTC (Triple Timer Counter) ************************************************
 *
 * TTC1 counter 0 (0xFF120000): system tick, 100 Hz interval timer.
 * TTC2 counter 0 (0xFF130000): HRT (Phase 4), configured at 1 MHz via ÷100.
 */

#define BOARD_TTC_SYSCLK_FREQUENCY   BOARD_LPD_FREQUENCY  /* 100 MHz input */

/* UART **********************************************************************
 *
 * UART1 (0xFF010000): connected to the KV260 USB-UART bridge → /dev/ttyUSB1.
 *   Used as the NuttX NSH console.
 * UART0 (0xFF000000): available for MAVLink (Phase 6) → /dev/ttyUSB0.
 *   115200 8N1 from 100 MHz reference clock.
 */

#define BOARD_UART_FREQUENCY  BOARD_LPD_FREQUENCY  /* 100 MHz ref */

#endif /* __BOARDS_AMD_KRIA_KV260_NUTTX_CONFIG_INCLUDE_BOARD_H */
