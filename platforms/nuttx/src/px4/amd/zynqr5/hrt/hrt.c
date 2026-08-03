/****************************************************************************
 * platforms/nuttx/src/px4/amd/zynqr5/hrt/hrt.c
 *
 * HRT for the Zynq R5F.
 *
 * Timekeeping: a dedicated TTC counter (CONFIG_ZYNQR5_TTC_HRT), giving
 * sub-microsecond resolution.  Without it this falls back to CLOCK_MONOTONIC,
 * which NuttX backs with the system tick -- 1 ms granularity at
 * CONFIG_USEC_PER_TICK=1000.  That fallback is only adequate for bring-up: PX4
 * timestamps every uORB message with hrt_absolute_time(), so a 1 ms clock
 * quantises every dt the estimator and controllers differentiate, gives every
 * perf counter a 1 ms floor, and makes scheduling slices shorter than a tick
 * round to zero -- which is why "top" reported 0.000% for every task and the
 * idle time under-accumulated into a bogus ~98.5% load.
 *
 * Callout queue: still a single NuttX wdog, so callback *delivery* is still
 * quantised to the system tick even when the clock is not.  Moving the queue
 * onto a TTC match interrupt is the remaining half of this work.
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

#ifdef CONFIG_ZYNQR5_TTC_HRT
#  include <arch/zynqr5/irq.h>
#  include "arm_internal.h"

/* Register map for one TTC counter; see arch/arm/src/zynqr5/zynqr5_timerisr.c
 * for the full layout.  N is the counter index within the TTC.
 */

#define HRT_TTC_BASE      ((uint32_t)CONFIG_ZYNQR5_TTC_HRT_BASE)
#define HRT_TTC_N         (CONFIG_ZYNQR5_TTC_HRT_COUNTER)

#define HRT_CLK_CTRL      (HRT_TTC_BASE + 0x00U + (HRT_TTC_N) * 4)
#define HRT_CNT_CTRL      (HRT_TTC_BASE + 0x0CU + (HRT_TTC_N) * 4)
#define HRT_CNT_VALUE     (HRT_TTC_BASE + 0x18U + (HRT_TTC_N) * 4)
#define HRT_INT_STATUS    (HRT_TTC_BASE + 0x54U + (HRT_TTC_N) * 4)
#define HRT_INT_ENABLE    (HRT_TTC_BASE + 0x60U + (HRT_TTC_N) * 4)

#define HRT_CTRL_DIS      (1U << 0)   /* 1 = counter halted */
#define HRT_CTRL_INT      (1U << 1)   /* 1 = interval mode, 0 = free running */
#define HRT_CTRL_RST      (1U << 4)   /* restart counter */

#define HRT_CLK_PS_EN     (1U << 0)
#define HRT_CLK_PS_VAL(v) (((v) & 0xfU) << 1)

#define HRT_INTERVAL      (HRT_TTC_BASE + 0x24U + (HRT_TTC_N) * 4)

#define HRT_CTRL_MATCH    (1U << 3)
#define HRT_INT_INTERVAL  (1U << 0)   /* counter reached INTERVAL and reset */

/* Interval mode, exactly as the system tick uses (zynqr5_timerisr.c), rather
 * than free-running.  This is the second attempt: free-running with the counter
 * masked to 16 bits and the overflow interrupt did not work, and the failure was
 * instructive -- hrt_absolute_time() advanced for one 42 ms period and then
 * froze permanently, so mavlink emitted a single burst of 15 messages at t=0 and
 * went silent.  The counter is wider than 16 bits, so masking it produced a
 * sawtooth while no 16-bit overflow interrupt ever fired to advance the base;
 * the monotonicity clamp then pinned the clock at the first peak forever.
 *
 * Interval mode sidesteps the question entirely: the counter is bounded by the
 * INTERVAL register regardless of its physical width, and the interval interrupt
 * is the same one the tick has used reliably for weeks.
 *
 * 62500 counts at 1.5625 MHz is exactly 40 000 us, so the accumulator advances
 * by a whole number of microseconds and needs no fractional carry.  It also
 * stays under 65536, so this remains correct even if the counter really is
 * 16 bits.
 */

#define HRT_INTERVAL_TICKS 62500U

/* Counter frequency = input / 2^(PRESCALE_LOG2 + 1).  The prescaler only
 * divides by powers of two, so an exact 1 MHz count is impossible from a
 * 100 MHz input: 100e6 = 2^8 * 5^8, and dividing by 2^(N+1) always leaves a
 * factor of 25.  At the default N=5 the counter runs at 1.5625 MHz and
 * microseconds convert exactly as count * 16 / 25 -- an integer ratio, so no
 * error accumulates.  Both constants are derived here rather than hardcoded so
 * a different prescaler or input clock stays correct.
 */

#define HRT_CLK_DIV       (1ULL << (CONFIG_ZYNQR5_TTC_HRT_PRESCALE_LOG2 + 1))
#define HRT_CNT_FREQ      ((uint64_t)CONFIG_ZYNQR5_TTC_FREQ / HRT_CLK_DIV)

/* Microseconds from counter ticks.
 *
 *   us = ticks * 1e6 / HRT_CNT_FREQ
 *      = ticks * HRT_US_MUL / HRT_US_DIV
 *
 * which at the defaults (100 MHz input, /64) is ticks * 64 / 100 -- exactly
 * 0.64 us per tick.  Requires a whole number of MHz in, which every ZynqMP PS
 * clock option is.
 *
 * The obvious implementation, a 64-bit multiply then a 64-bit divide, is wrong
 * for the hot path: GCC folds division by a constant into a reciprocal multiply
 * only for 32-bit operands, so a 64-bit divisor emits a call to
 * __aeabi_uldivmod -- a software division, in a function PX4 calls on every
 * uORB publish.  Verified in the disassembly of the first cut of this file.
 *
 * So the accumulation is split.  The overflow interrupt keeps the whole
 * microseconds in a 64-bit counter and carries the fractional remainder
 * separately; both updates are exact, and a 64-bit divide there costs nothing
 * because it runs at HRT_CNT_FREQ / 65536 (about 24 Hz).  hrt_absolute_time()
 * then only has to convert the sub-period part, whose numerator is bounded by
 * (65535 * HRT_US_MUL + HRT_US_DIV) and therefore fits in 32 bits -- so the
 * division that remains is 32-bit and GCC does fold it.
 */

#define HRT_US_MUL        ((uint32_t)HRT_CLK_DIV)
#define HRT_US_DIV        ((uint32_t)(CONFIG_ZYNQR5_TTC_FREQ / 1000000UL))

/* Microseconds per interval period.  Exact by construction -- see the note on
 * HRT_INTERVAL_TICKS -- so no fractional carry is needed.
 */

#define HRT_PERIOD_US ((uint32_t)(((uint64_t)HRT_INTERVAL_TICKS * HRT_US_MUL) / HRT_US_DIV))

/* Whole microseconds since start, advanced by the interval interrupt. */

static volatile uint64_t g_hrt_base_us;

/* Last value handed out.  hrt_absolute_time() clamps against this so the clock
 * can never run backwards: if the counter has wrapped but the overflow
 * interrupt has not been serviced yet (a reader inside a critical section, say),
 * the naive sum would regress.  Time stalling for an interrupt latency is
 * harmless; time going backwards makes every dt negative.
 */

static volatile uint64_t g_hrt_last_us;
#endif /* CONFIG_ZYNQR5_TTC_HRT */

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

#ifdef CONFIG_ZYNQR5_TTC_HRT

/* Overflow interrupt: advance the tick accumulator by one full counter period.
 *
 * The TTC interrupt register is clear-on-read, so reading it here both
 * acknowledges the interrupt and consumes the flag -- which is exactly why
 * hrt_absolute_time() must never read it, or it would steal the event from
 * this handler and lose 65536 ticks.
 */

static int hrt_ttc_isr(int irq, void *context, void *arg)
{
	(void)irq;
	(void)context;
	(void)arg;

	if (getreg32(HRT_INT_STATUS) & HRT_INT_INTERVAL) {
		g_hrt_base_us += HRT_PERIOD_US;
	}

	return OK;
}

static void hrt_ttc_init(void)
{
	/* Halt while reconfiguring. */

	putreg32(HRT_CTRL_DIS, HRT_CNT_CTRL);

	/* Prescaler: divide the input clock down to the counter frequency. */

	putreg32(HRT_CLK_PS_EN |
		 HRT_CLK_PS_VAL(CONFIG_ZYNQR5_TTC_HRT_PRESCALE_LOG2),
		 HRT_CLK_CTRL);

	/* Clear any stale flag (clear-on-read), then enable overflow only. */

	/* Period, then reset in interval mode. */

	putreg32(HRT_INTERVAL_TICKS, HRT_INTERVAL);
	putreg32(HRT_CTRL_INT | HRT_CTRL_RST, HRT_CNT_CTRL);

	(void)getreg32(HRT_INT_STATUS);          /* clear-on-read */
	putreg32(HRT_INT_INTERVAL, HRT_INT_ENABLE);

	g_hrt_base_us = 0;
	g_hrt_last_us = 0;

	/* Release reset, staying in interval mode, DIS clear so it runs. */

	putreg32(HRT_CTRL_INT, HRT_CNT_CTRL);

	irq_attach(CONFIG_ZYNQR5_TTC_HRT_IRQ, hrt_ttc_isr, NULL);
	up_enable_irq(CONFIG_ZYNQR5_TTC_HRT_IRQ);
}

/**
 * hrt_absolute_time - microseconds since system start, from the TTC.
 *
 * Resolution is one counter tick (0.64 us at the defaults) rather than the
 * 1 ms of the system tick.
 */
hrt_abstime hrt_absolute_time(void)
{
	irqstate_t flags = enter_critical_section();

	/* The counter is bounded by HRT_INTERVAL_TICKS, so this product stays
	 * inside 32 bits (62500 * 64 = 4e6) and the divide is a 32-bit one, which
	 * the Cortex-R5's hardware UDIV handles in the hot path.
	 */

	uint32_t sub = getreg32(HRT_CNT_VALUE) * HRT_US_MUL;

	uint64_t us = g_hrt_base_us + (sub / HRT_US_DIV);

	/* Monotonicity guard: see g_hrt_last_us.  If the counter has wrapped but
	 * hrt_ttc_isr() has not run yet, the sum above is a period behind and
	 * would step backwards.
	 */

	if (us < g_hrt_last_us) {
		us = g_hrt_last_us;

	} else {
		g_hrt_last_us = us;
	}

	leave_critical_section(flags);

	return (hrt_abstime)us;
}

#else /* !CONFIG_ZYNQR5_TTC_HRT */

/**
 * hrt_absolute_time - fallback, resolution limited to CONFIG_USEC_PER_TICK.
 */
hrt_abstime hrt_absolute_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (hrt_abstime)ts.tv_sec * 1000000ULL +
	       (hrt_abstime)ts.tv_nsec / 1000ULL;
}

#endif /* CONFIG_ZYNQR5_TTC_HRT */

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

#ifdef CONFIG_ZYNQR5_TTC_HRT
	/* Must come after the queue init: the overflow interrupt is live from
	 * here on, and anything it touches has to be ready.
	 */

	hrt_ttc_init();
#endif
}

