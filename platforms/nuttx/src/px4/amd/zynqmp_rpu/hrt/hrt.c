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
 * @file hrt.c
 *
 * High-resolution timer for the ZynqMP RPU on TTC2 counter 1: the free-running
 * NuttX timebase gives the time, its Match 1 interrupt runs the callouts.
 */

#include <px4_platform_common/px4_config.h>
#include <drivers/drv_hrt.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/queue.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/board/board.h>

#include "arm_internal.h"
#include "hardware/zynqmp_rpu_ttc.h"
#include "zynqmp_rpu_timebase.h"

#if CONFIG_ZYNQMP_RPU_TIMEBASE_TTC != 2 || CONFIG_ZYNQMP_RPU_TIMEBASE_COUNTER != 1
#  error "The HRT needs the timebase on TTC2 counter 1 (IRQ 75)"
#endif

#define HRT_COUNTS_PER_US   (BOARD_TTC_CLK_FREQUENCY / 1000000)
#define HRT_TIMER_IRQ       ZYNQMP_RPU_IRQ_TTC2_1

#define HRT_REG(off)        (ZYNQMP_RPU_TTC2_BASE + XTTCPS_CNT_OFFSET(off, 1))
#define HRT_MATCH           HRT_REG(XTTCPS_MATCH_0_OFFSET)
#define HRT_ISR             HRT_REG(XTTCPS_ISR_OFFSET)
#define HRT_IER             HRT_REG(XTTCPS_IER_OFFSET)

/* Deadlines are at least HRT_INTERVAL_MIN ahead so the match is never missed,
 * at most HRT_INTERVAL_MAX so the ISR keeps the wrap extension current.
 */

#define HRT_INTERVAL_MIN    50
#define HRT_INTERVAL_MAX    50000

static struct sq_queue_s callout_queue;

/* Counter values of the last programmed match and of the last ISR entry */

static uint32_t latency_baseline;
static uint32_t latency_actual;

const uint16_t latency_bucket_count = LATENCY_BUCKET_COUNT;
const uint16_t latency_buckets[LATENCY_BUCKET_COUNT] = {1, 2, 5, 10, 20, 50, 100, 1000};
__EXPORT uint32_t latency_counters[LATENCY_BUCKET_COUNT + 1];

static void hrt_call_internal(struct hrt_call *entry, hrt_abstime deadline, hrt_abstime interval,
			      hrt_callout callout, void *arg);
static void hrt_call_enter(struct hrt_call *entry);
static void hrt_call_reschedule(void);
static void hrt_call_invoke(void);
static void hrt_latency_update(void);

/* 64-bit count from the 32-bit counter. Called with the critical section held. */

static uint64_t hrt_counts(void)
{
	static uint64_t base;
	static uint32_t last;

	uint32_t count = zynqmp_rpu_timebase();

	if (count < last) {
		base += UINT64_C(1) << 32;
	}

	last = count;

	return base + count;
}

static int hrt_tim_isr(int irq, void *context, void *arg)
{
	latency_actual = zynqmp_rpu_timebase();

	/* Reading ISR clears it */

	if (getreg32(HRT_ISR) & XTTCPS_IXR_MATCH_0_MASK) {
		hrt_latency_update();
		hrt_call_invoke();
		hrt_call_reschedule();
	}

	return OK;
}

hrt_abstime hrt_absolute_time(void)
{
	irqstate_t flags = px4_enter_critical_section();
	uint64_t counts = hrt_counts();
	px4_leave_critical_section(flags);

	return counts / HRT_COUNTS_PER_US;
}

void hrt_store_absolute_time(volatile hrt_abstime *time)
{
	irqstate_t flags = px4_enter_critical_section();
	*time = hrt_absolute_time();
	px4_leave_critical_section(flags);
}

void hrt_init(void)
{
	sq_init(&callout_queue);

	irqstate_t flags = px4_enter_critical_section();

	hrt_call_reschedule();

	/* Clear the match bits latched while IER was 0 */

	getreg32(HRT_ISR);
	putreg32(XTTCPS_IXR_MATCH_0_MASK, HRT_IER);

	irq_attach(HRT_TIMER_IRQ, hrt_tim_isr, NULL);
	up_enable_irq(HRT_TIMER_IRQ);

	px4_leave_critical_section(flags);
}

void hrt_call_after(struct hrt_call *entry, hrt_abstime delay, hrt_callout callout, void *arg)
{
	hrt_call_internal(entry, hrt_absolute_time() + delay, 0, callout, arg);
}

void hrt_call_at(struct hrt_call *entry, hrt_abstime calltime, hrt_callout callout, void *arg)
{
	hrt_call_internal(entry, calltime, 0, callout, arg);
}

void hrt_call_every(struct hrt_call *entry, hrt_abstime delay, hrt_abstime interval,
		    hrt_callout callout, void *arg)
{
	hrt_call_internal(entry, hrt_absolute_time() + delay, interval, callout, arg);
}

static void hrt_call_internal(struct hrt_call *entry, hrt_abstime deadline, hrt_abstime interval,
			      hrt_callout callout, void *arg)
{
	irqstate_t flags = px4_enter_critical_section();

	/* A queued entry has a non-zero deadline */

	if (entry->deadline != 0) {
		sq_rem(&entry->link, &callout_queue);
	}

	entry->deadline = deadline;
	entry->period = interval;
	entry->callout = callout;
	entry->arg = arg;
	hrt_call_enter(entry);

	px4_leave_critical_section(flags);
}

bool hrt_called(struct hrt_call *entry)
{
	return entry->deadline == 0;
}

void hrt_cancel(struct hrt_call *entry)
{
	irqstate_t flags = px4_enter_critical_section();

	sq_rem(&entry->link, &callout_queue);
	entry->deadline = 0;

	/* Stops a periodic call from being re-entered when cancelled by its own callout */

	entry->period = 0;

	px4_leave_critical_section(flags);
}

void hrt_call_init(struct hrt_call *entry)
{
	memset(entry, 0, sizeof(*entry));
}

void hrt_call_delay(struct hrt_call *entry, hrt_abstime delay)
{
	entry->deadline = hrt_absolute_time() + delay;
}

/* Insert an entry in deadline order. Called with the critical section held. */

static void hrt_call_enter(struct hrt_call *entry)
{
	struct hrt_call *call = (struct hrt_call *)sq_peek(&callout_queue);

	if (call == NULL || entry->deadline < call->deadline) {
		sq_addfirst(&entry->link, &callout_queue);
		hrt_call_reschedule();
		return;
	}

	struct hrt_call *next;

	while ((next = (struct hrt_call *)sq_next(&call->link)) != NULL &&
	       next->deadline <= entry->deadline) {
		call = next;
	}

	sq_addafter(&call->link, &entry->link, &callout_queue);
}

/* Run the due callouts. Called from the ISR. */

static void hrt_call_invoke(void)
{
	for (;;) {
		hrt_abstime now = hrt_absolute_time();
		struct hrt_call *call = (struct hrt_call *)sq_peek(&callout_queue);

		if (call == NULL || call->deadline > now) {
			return;
		}

		sq_rem(&call->link, &callout_queue);
		hrt_abstime deadline = call->deadline;
		call->deadline = 0;

		if (call->callout != NULL) {
			call->callout(call->arg);
		}

		/* Re-enter a periodic call, unless its callout moved it with hrt_call_delay() */

		if (call->period != 0) {
			if (call->deadline <= now) {
				call->deadline = deadline + call->period;
			}

			hrt_call_enter(call);
		}
	}
}

/* Program Match 1 for the next deadline. Called with the critical section held. */

static void hrt_call_reschedule(void)
{
	for (;;) {
		hrt_abstime now = hrt_absolute_time();
		struct hrt_call *next = (struct hrt_call *)sq_peek(&callout_queue);
		hrt_abstime deadline = now + HRT_INTERVAL_MAX;

		if (next != NULL) {
			if (next->deadline <= now + HRT_INTERVAL_MIN) {
				deadline = now + HRT_INTERVAL_MIN;

			} else if (next->deadline < deadline) {
				deadline = next->deadline;
			}
		}

		latency_baseline = (uint32_t)(deadline * HRT_COUNTS_PER_US);
		putreg32(latency_baseline, HRT_MATCH);

		/* The match is an equality compare: retry if the counter already passed it */

		if ((int32_t)(zynqmp_rpu_timebase() - latency_baseline) < 0) {
			return;
		}
	}
}

static void hrt_latency_update(void)
{
	uint32_t latency = (latency_actual - latency_baseline) / HRT_COUNTS_PER_US;
	unsigned index;

	for (index = 0; index < LATENCY_BUCKET_COUNT; index++) {
		if (latency <= latency_buckets[index]) {
			latency_counters[index]++;
			return;
		}
	}

	latency_counters[index]++;
}
