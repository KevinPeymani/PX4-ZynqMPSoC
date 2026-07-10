# LPD_XPPU Fix: Grant RPU0 Write Access to RPU GIC

## Problem

LPD_XPPU silently discards all writes to the RPU GIC aperture
(`0xF9000000–0xF9001FFF`) from both RPU0 and APU.

**Confirmed 2026-07-09:**
```
devmem 0xF9000000 32          # GICD_CTLR = 0x00000000 (disabled, writes ignored)
devmem 0xF9001004 32          # GICC_PMR  = 0x00000000 (no IRQs pass through)
devmem 0xF9000108 32 0x000082 # write attempt
devmem 0xF9000108 32          # GICD_ISENABLER[2] = 0x00000000 (not written)
```

Until this is fixed: TTC ISR, IPI ISR, and UART RX ISR are all wired in
NuttX code (via `irq_attach` + `up_enable_irq`) but GIC never routes them.
Spin-poll fallback in `up_idle()` keeps rpmsg working.

---

## Step 1 — Identify the XPPU Aperture for 0xF9000000

LPD_XPPU base: `0xFF980000`. Apertures start at `+0x100`, one 32-bit word
each, covering successive 4 KB pages in the LPD address map.

### Option A — Grep the Xilinx FSBL / BSP headers

In the Vitis/SDK workspace, search for the aperture index:

```sh
grep -r "0xF9000" \
  <vitis-workspace>/platform/zynqmp_fsbl/zynqmp_fsbl_bsp/psu_cortexa53_0/libsrc/xilpm*/
```

Or check `xppu_hw.h` / `psu_init_gpl.c` for a table of LPD_XPPU aperture
assignments.

### Option B — Scan aperture registers from Linux

```sh
for i in $(seq 0 63); do
  addr=$(printf "0xFF98%04X" $((0x100 + i * 4)))
  val=$(sudo devmem $addr 32)
  echo "aperture[$i] $addr: $val"
done
```

Apertures with a non-zero value are currently configured. Cross-reference
the LPD memory map (UG1085 table 2-3) to find which aperture covers
`0xF9000000`.

---

## Step 2 — Determine the Required Permission Bits

Each LPD_XPPU aperture word has per-master enable bits (UG1085 §16.8):

| Bit | Master             |
|-----|--------------------|
| 0   | APU (Cortex-A53)   |
| 2   | RPU_0 (R5F-0) ← **add this** |
| 3   | RPU_1 (R5F-1)      |
| 4   | PMU                |
| 19  | Enable aperture    |

Read the current aperture value and OR in bit 2 (RPU_0) and bit 19 (enable):

```sh
# Example: aperture N currently = 0x00080001 (APU only + enable)
# After fix it should be 0x00080005 (APU + RPU0 + enable)
```

---

## Step 3 — Apply the Fix

### Option A — Temporary (survives remoteproc stop/start, not reboot)

From Linux, write directly via devmem once FSBL has run:

```sh
APERTURE_ADDR=0xFF9800xx          # fill in from Step 1
CURRENT=$(sudo devmem $APERTURE_ADDR 32)
NEW=$(printf "0x%08X" $(( CURRENT | 0x00080004 )))  # add RPU0 bit + ensure enable
sudo devmem $APERTURE_ADDR 32 $NEW
```

Then verify GIC writes work:

```sh
sudo devmem 0xF9000108 32 0x00000082   # enable IRQ 65 + 71
sudo devmem 0xF9000108 32              # should read 0x00000082
```

If GICD_ISENABLER reads back the written value, the fix is confirmed.

### Option B — Permanent (FSBL, survives reboot)

In `<vitis-workspace>/platform/zynqmp_fsbl/xfsbl_hooks.c`, inside
`XFsbl_HookBeforeHandoff()`:

```c
/* Grant RPU0 write access to the RPU GIC (0xF9000000-0xF9001FFF).
 * LPD_XPPU aperture N covers this range (N from psu_init or TRM table). */
#define LPD_XPPU_BASE       0xFF980000UL
#define LPD_XPPU_APERTURE_N (LPD_XPPU_BASE + 0x100 + (N) * 4)  /* fill in N */
#define XPPU_ENABLE_BIT     (1U << 19)
#define XPPU_APU_BIT        (1U << 0)
#define XPPU_RPU0_BIT       (1U << 2)

u32 perm = Xil_In32(LPD_XPPU_APERTURE_N);
perm |= XPPU_ENABLE_BIT | XPPU_APU_BIT | XPPU_RPU0_BIT;
Xil_Out32(LPD_XPPU_APERTURE_N, perm);
```

Rebuild BOOT.BIN:

```sh
# In Vitis:
# 1. Build the platform (generates updated psu_init + FSBL)
# 2. Build the BOOT.BIN (Vitis → Create Boot Image, or xsct script)
# 3. Copy BOOT.BIN to KV260 SD card /boot/
```

---

## Step 4 — Verify on the Board

After applying the fix (Option A or B), reload the R5F firmware and check:

```sh
# Restart firmware
echo stop | sudo tee /sys/class/remoteproc/remoteproc0/state
echo start | sudo tee /sys/class/remoteproc/remoteproc0/state
sleep 3

# TTC ISR ticking?
sudo devmem 0x1ED3FFDC 32   # TIMER_CNT — should be ~3000 after 3 s

# IPI ISR firing?
sudo devmem 0x1ED3FFE8 32   # RPTUN_ISR — should be > 0

# GIC registers configured?
sudo devmem 0xF9000000 32   # GICD_CTLR — should be 1 (enabled)
sudo devmem 0xF9001004 32   # GICC_PMR  — should be 0xFF

# Ping still works?
sudo python3 /tmp/ping_test.py
```

Expected after fix:
- `TIMER_CNT` ≈ 3000 (TTC ISR running at 1 kHz)
- `RPTUN_ISR` > 0 (IPI delivered via GIC, not spin-poll)
- NSH keyboard input works (UART RX ISR active)

---

## After GIC Works

Once GIC interrupts are confirmed:

1. **Switch rptun from spin-poll to IRQ**: in `drivers/rptun/rptun.c`, revert
   `rptun_thread()` to `nxsem_wait_uninterruptible(&priv->semrx)`.  The
   `zynqr5_rptun_ipi_check()` fallback in `up_idle()` can be removed.

2. **Remove debug breadcrumbs** from `zynqr5_start.c` and `zynqr5_irq.c`
   once the system is stable.
