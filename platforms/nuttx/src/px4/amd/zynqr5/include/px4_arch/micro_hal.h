#pragma once

#include <px4_platform/micro_hal.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/irq.h>
#include <stdbool.h>

__BEGIN_DECLS

#define PX4_SOC_ARCH_ID             PX4_SOC_ARCH_ID_UNUSED

/* Zynq UltraScale+ MPSoC does not have bbsram */
#define PX4_BBSRAM_SIZE             0
#define PX4_HF_GETDESC_IOCTL        0

/* CPU unique ID: 96-bit (3 × 32-bit words) */
#define PX4_CPU_UUID_BYTE_LENGTH                12
#define PX4_CPU_UUID_WORD32_LENGTH              (PX4_CPU_UUID_BYTE_LENGTH/sizeof(uint32_t))
#define PX4_CPU_MFGUID_BYTE_LENGTH              PX4_CPU_UUID_BYTE_LENGTH
#define PX4_CPU_UUID_WORD32_UNIQUE_H            2
#define PX4_CPU_UUID_WORD32_UNIQUE_M            1
#define PX4_CPU_UUID_WORD32_UNIQUE_L            0
#define PX4_CPU_UUID_WORD32_FORMAT_SIZE         (PX4_CPU_UUID_WORD32_LENGTH-1+(2*PX4_CPU_UUID_BYTE_LENGTH)+1)
#define PX4_CPU_MFGUID_FORMAT_SIZE              ((2*PX4_CPU_MFGUID_BYTE_LENGTH)+1)

/* No SPI/I2C bus drivers yet */
#define PX4_BUS_OFFSET              0
#define PX4_NUMBER_I2C_BUSES        1
#define BOARD_I2C_BUS_CLOCK_INIT    {100000}

/* GPIO — not wired from RPU in Phase 1; inline stubs satisfy linkage */
static inline int px4_arch_configgpio(uint32_t pinset) { (void)pinset; return 0; }
static inline void px4_arch_unconfiggpio(uint32_t pinset) { (void)pinset; }
static inline bool px4_arch_gpioread(uint32_t pinset) { (void)pinset; return 0; }
static inline void px4_arch_gpiowrite(uint32_t pinset, bool value) { (void)pinset; (void)value; }
static inline int px4_arch_gpiosetevent(uint32_t pinset, bool r, bool f, bool e, xcpt_t fp, void *a)
{ (void)pinset; (void)r; (void)f; (void)e; (void)fp; (void)a; return -1; }

#define px4_cache_aligned_data()
#define px4_cache_aligned_alloc malloc

__END_DECLS
