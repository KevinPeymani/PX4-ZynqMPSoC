/****************************************************************************
 *
 *   Copyright (c) 2026 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file init.c
 *
 * AMD KV260 RPU specific early startup code.
 */

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/init.h>

#include <nuttx/board.h>
#include <nuttx/serial/uart_rpmsg_raw.h>

#include <sys/mount.h>
#include <syslog.h>

#include "board_config.h"
#include "zynqmp_rpu_rptun.h"

/* Called by the chip boot code */

void zynqmp_rpu_board_initialize(void)
{
}

/* Called by drivers_initialize(): registers the /dev/ttyRPMSG0 console and the /dev/ttyRPMSG1 MAVLink link */

void rpmsg_serialrawinit(void)
{
	uart_rpmsg_raw_init("linux", "RPMSG0", 4096, true);
	uart_rpmsg_raw_init("linux", "RPMSG1", 4096, false);
}

/* Starts the RPMsg link after the work queues and before NSH opens the RPMsg console */

void board_late_initialize(void)
{
	int ret = zynqmp_rpu_rptun_initialize();

	if (ret < 0) {
		syslog(LOG_ERR, "[boot] FAILED to start rptun (%d)\n", ret);

	} else {
		syslog(LOG_INFO, "[boot] rptun started\n");
	}
}

__EXPORT void board_peripheral_reset(int ms)
{
	UNUSED(ms);
}

__EXPORT void board_on_reset(int status)
{
	UNUSED(status);
}

__EXPORT int board_app_initialize(uintptr_t arg)
{
	UNUSED(arg);

	/* Boot progress goes to the syslog UART, since the console needs the RPMsg link */

	syslog(LOG_INFO, "[boot] app init\n");

	/* There is no flash storage driver yet: keep parameters and dataman in
	 * RAM, under CONFIG_BOARD_ROOT_PATH.
	 */

	if (mount(NULL, "/tmp", "tmpfs", 0, NULL) < 0) {
		syslog(LOG_ERR, "[boot] FAILED to mount /tmp\n");

	} else {
		syslog(LOG_INFO, "[boot] /tmp mounted\n");
	}

	px4_platform_init();

	px4_platform_configure();

	syslog(LOG_INFO, "[boot] px4 init done\n");

	return OK;
}
