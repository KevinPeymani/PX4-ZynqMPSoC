#include <nuttx/config.h>
#include <arch/board/board.h>

/* No critical-section monitor implementation yet for Zynq R5F.
 * CONFIG_SCHED_CRITMONITOR and CONFIG_SCHED_IRQMONITOR require
 * up_critmon_gettime() and up_critmon_convert() which need a
 * dedicated hardware counter. Implement in Phase 4 using TTC2.
 */
#if defined(CONFIG_SCHED_CRITMONITOR) || defined(CONFIG_SCHED_IRQMONITOR)
#error "Critical section monitor requires up_critmon_gettime() implementation for Zynq R5F"
#endif
