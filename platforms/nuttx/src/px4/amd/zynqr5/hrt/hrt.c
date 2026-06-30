/****************************************************************************
 * platforms/nuttx/src/px4/amd/zynqr5/hrt/hrt.c
 *
 * Phase 1 HRT for Zynq R5F: uses CLOCK_MONOTONIC for timekeeping and
 * a single NuttX wdog for the callout queue.
 *
 * Phase 4 will replace this with a TTC2-based 1 MHz HRT.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <queue.h>

#include <nuttx/irq.h>
#include <nuttx/clock.h>
#include <nuttx/wdog.h>

#include <drivers/drv_hrt.h>

/* latency histogram required by drv_hrt.h */
const uint16_t latency_bucket_count = LATENCY_BUCKET_COUNT;
const uint16_t latency_buckets[LATENCY_BUCKET_COUNT] = { 1, 2, 5, 10, 20, 50, 100, 1000 };
__EXPORT uint32_t latency_counters[LATENCY_BUCKET_COUNT + 1];

/* Single global callout queue + wdog */
static sq_queue_t g_callout_queue;
static struct wdog_s g_hrt_wdog;

/* forward declarations */
static void hrt_call_invoke(void);
static void hrt_call_enter(struct hrt_call *entry);
static void hrt_wdog_schedule(void);

/**
 * hrt_absolute_time – microseconds since system start.
 * Uses CLOCK_MONOTONIC which NuttX backs with the system tick counter.
 * Resolution is limited to USEC_PER_TICK (~10 ms at 100 Hz) for Phase 1;
 * Phase 4 will replace with TTC2 at 1 MHz.
 */
hrt_abstime hrt_absolute_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (hrt_abstime)ts.tv_sec * 1000000ULL +
	       (hrt_abstime)ts.tv_nsec / 1000ULL;
}

void hrt_store_absolute_time(volatile hrt_abstime *t)
{
	irqstate_t flags = enter_critical_section();
	*t = hrt_absolute_time();
	leave_critical_section(flags);
}

/* Wdog callback: process all due entries then reschedule for the next one. */
static void hrt_wdog_callback(wdparm_t arg)
{
	(void)arg;
	hrt_call_invoke();
	hrt_wdog_schedule();
}

/* Schedule the wdog to fire when the queue head becomes due. */
static void hrt_wdog_schedule(void)
{
	struct hrt_call *next = (struct hrt_call *)(void *)sq_peek(&g_callout_queue);

	if (!next) {
		return;
	}

	hrt_abstime now  = hrt_absolute_time();
	sclock_t ticks;

	if (next->deadline <= now) {
		ticks = 1;

	} else {
		ticks = (sclock_t)USEC2TICK(next->deadline - now);

		if (ticks < 1) {
			ticks = 1;
		}
	}

	wd_start(&g_hrt_wdog, ticks, hrt_wdog_callback, 0);
}

/* Fire all overdue entries, then reschedule repeating ones. */
static void hrt_call_invoke(void)
{
	struct hrt_call *call;

	while (true) {
		call = (struct hrt_call *)(void *)sq_peek(&g_callout_queue);

		if (!call || hrt_absolute_time() < call->deadline) {
			break;
		}

		sq_remfirst(&g_callout_queue);

		hrt_callout callout = call->callout;
		void *arg = call->arg;

		if (call->period != 0) {
			call->deadline += call->period;
			hrt_call_enter(call);

		} else {
			call->deadline = 0;
		}

		if (callout) {
			callout(arg);
		}
	}
}

/* Insert entry into the queue sorted by deadline (earliest first). */
static void hrt_call_enter(struct hrt_call *entry)
{
	struct hrt_call *call;
	struct hrt_call *next;

	call = (struct hrt_call *)(void *)sq_peek(&g_callout_queue);

	if (!call || entry->deadline < call->deadline) {
		sq_addfirst(&entry->link, &g_callout_queue);

	} else {
		do {
			next = (struct hrt_call *)(void *)sq_next(&call->link);

			if (!next || entry->deadline < next->deadline) {
				sq_addafter(&call->link, &entry->link, &g_callout_queue);
				break;
			}
		} while ((call = next) != NULL);
	}

	/* Reschedule wdog if this entry is now the head. */
	if ((struct hrt_call *)(void *)sq_peek(&g_callout_queue) == entry) {
		hrt_wdog_schedule();
	}
}

static void hrt_call_internal(struct hrt_call *entry, hrt_abstime deadline,
			      hrt_abstime interval, hrt_callout callout,
			      void *arg)
{
	irqstate_t flags = enter_critical_section();

	if (entry->deadline != 0) {
		sq_rem(&entry->link, &g_callout_queue);
	}

	entry->deadline = deadline;
	entry->period   = interval;
	entry->callout  = callout;
	entry->arg      = arg;

	hrt_call_enter(entry);
	leave_critical_section(flags);
}

void hrt_call_after(struct hrt_call *entry, hrt_abstime delay,
		    hrt_callout callout, void *arg)
{
	hrt_call_internal(entry, hrt_absolute_time() + delay, 0, callout, arg);
}

void hrt_call_at(struct hrt_call *entry, hrt_abstime calltime,
		 hrt_callout callout, void *arg)
{
	hrt_call_internal(entry, calltime, 0, callout, arg);
}

void hrt_call_every(struct hrt_call *entry, hrt_abstime delay,
		    hrt_abstime interval, hrt_callout callout, void *arg)
{
	hrt_call_internal(entry, hrt_absolute_time() + delay, interval, callout, arg);
}

void hrt_cancel(struct hrt_call *entry)
{
	irqstate_t flags = enter_critical_section();
	sq_rem(&entry->link, &g_callout_queue);
	entry->deadline = 0;
	leave_critical_section(flags);
}

void hrt_init(void)
{
	sq_init(&g_callout_queue);
}

/* up_perf_* stubs required by CONFIG_RPTUN_PING.
 * Phase 4 will replace these with TTC2 cycle-counter readings. */

uint32_t up_perf_getfreq(void)
{
	return 1000000u; /* 1 MHz nominal; avoids div-by-zero in rptun_ping */
}

uint32_t up_perf_gettime(void)
{
	return (uint32_t)hrt_absolute_time();
}

void up_perf_convert(uint32_t elapsed, struct timespec *ts)
{
	ts->tv_sec  = elapsed / 1000000u;
	ts->tv_nsec = (elapsed % 1000000u) * 1000u;
}
