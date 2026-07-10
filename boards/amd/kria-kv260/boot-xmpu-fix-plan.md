# BOOT.BIN XMPU/XPPU Fix: Enable RPU GIC Writes

## What's Broken and Why

The RPU GIC registers at `0xF9000000` (GICD) and `0xF9001000` (GICC) are write-protected.
NuttX calls `arm_gic0_initialize()` + `arm_gic_initialize()` without fault, but all writes
are silently discarded — confirmed 2026-07-09:

```sh
# Before fix:
sudo devmem 0xF9000108 32 0x82   # write GICD_ISENABLER[2]
sudo devmem 0xF9000108 32        # → 0x00000000 (not written)
sudo devmem 0xF9000000 32        # → 0x00000000 (GICD_CTLR disabled)
```

Consequence:
- **TTC timer ISR (IRQ 71)**: GIC never enables it → `TIMER_CNT` stays 0, NSH timing is wrong
- **UART RX ISR (IRQs 53/54)**: GIC never enables it → NSH keyboard does not work
- **IPI ISR (IRQ 65)**: **intentionally NOT fixed here** — LPD_XPPU also blocks R5F writes
  to `IPI_ISR` (0xFF310010), so enabling the GIC ISR for IPI causes a fatal data abort.
  IPI delivery uses `zynqr5_rptun_ipi_check()` spin-poll permanently.

NuttX ISR code is already committed and correct (`zynqr5_timerisr.c`, `zynqr5_serial.c`).
The NuttX GIC init correctly targets 0xF9000000 (fixed in chip.h Stage 01). The only
remaining blocker is the BOOT.BIN not unlocking write access.

---

## Current BOOT.BIN State

The board runs the **PetaLinux-built BOOT.BIN** from:
`Petalinux-KV260/xilinx-kv260-starterkit-2024.2/`

The PetaLinux FSBL's `XFsbl_HookBeforeHandoff()` is **empty** — no XMPU or XPPU
configuration at all. The echo-vitis FSBL (`echo-vitis/platform/zynqmp_fsbl/xfsbl_hooks.c`)
does have XMPU_FPD hooks but targets the wrong GIC addresses (APU GIC at 0xF9010000,
not the RPU GIC at 0xF9000000).

---

## Part 1 — Diagnose: Which Protection Is Blocking 0xF9000000?

### 1a. Scan LPD_XPPU apertures from Linux

The `xppu-gic-unlock.sh` script (same directory as this file) scans all 80 LPD_XPPU
apertures and dumps their current permission words:

```sh
# On the board (as root):
sudo bash /path/to/xppu-gic-unlock.sh
```

Look for an aperture with a non-zero value that maps near `0xF9000000`. The LPD_XPPU
at base `0xFF980000` has apertures starting at `+0x100` (4 bytes each, 4 KB pages).
The aperture index for `0xF9000000` depends on the LPD peripheral page table in UG1085
Table 16-4 (LPD XPPU Aperture Assignments).

### 1b. Verify the block with a direct devmem write test

```sh
# Write a test value to GICD_ISENABLER[2]:
sudo devmem 0xF9000108 32 0x00000082
# Read it back:
sudo devmem 0xF9000108 32
# If 0x00000000: still blocked. If 0x00000082: the current BOOT.BIN already allows it.
```

---

## Part 2 — Temporary Fix (devmem, no reboot required)

Once the LPD_XPPU aperture index N is identified from the scan:

```sh
APERTURE_BASE=$(( 0xFF980000 + 0x100 ))
N=<aperture index>               # from scan output
ADDR=$(printf "0x%08X" $(( APERTURE_BASE + N * 4 )))
CURRENT=$(sudo devmem $ADDR 32)
# Bit 0 = APU, bit 2 = RPU0, bit 19 = enable
NEW=$(printf "0x%08X" $(( CURRENT | (1<<0) | (1<<2) | (1<<19) )))
sudo devmem $ADDR 32 $NEW
```

Then immediately verify GIC writes work:

```sh
sudo devmem 0xF9000108 32 0x00000082
sudo devmem 0xF9000108 32          # must read 0x00000082
sudo devmem 0xF9000000 32          # GICD_CTLR — expect 0x00000001 after NuttX init
```

Then reload the firmware and check breadcrumbs:

```sh
echo stop | sudo tee /sys/class/remoteproc/remoteproc0/state
sleep 2
echo start | sudo tee /sys/class/remoteproc/remoteproc0/state
sleep 3
sudo devmem 0x1ED3FFDC 32   # TIMER_CNT  — expect ~3000 (TTC at 1 kHz, 3 s)
sudo devmem 0x1ED3FFE8 32   # RPTUN_ISR — expect 0 (IPI uses spin-poll, ISR disabled)
sudo devmem 0x1ED3FFA4 32   # GICD_CTLR snapshot — expect 0x00000001
sudo devmem 0x1ED3FFE0 32   # RPTUN_STEP — expect 0x02 (init done)
```

> **Note:** The temporary devmem fix resets after a power cycle. The FSBL fix in Part 3
> makes it permanent.

---

## Part 3 — Permanent Fix: PetaLinux FSBL Hook

The PetaLinux FSBL hook lives in:
`Petalinux-KV260/xilinx-kv260-starterkit-2024.2/project-spec/meta-user/`

There are two ways to add the hook.

### Option A — bbappend recipe (recommended, survives rebuilds)

Create:
`project-spec/meta-user/recipes-bsp/embeddedsw/fsbl-firmware_%.bbappend`

with content:
```
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://xfsbl_hooks.c"
```

Then place the patched `xfsbl_hooks.c` at:
`project-spec/meta-user/recipes-bsp/embeddedsw/files/xfsbl_hooks.c`

### Option B — Direct edit of the build tree (fragile, lost on clean)

Edit the built copy directly:
`build/tmp-xilinx-k26-kv-cortexa53-fsbl/work/cortexa53-xilinx-elf/clockps/2024.2+git/git/lib/sw_apps/zynqmp_fsbl/src/xfsbl_hooks.c`

### The Patch (applies to either option)

Replace the empty `XFsbl_HookBeforeHandoff` body with:

```c
u32 XFsbl_HookBeforeHandoff(u32 EarlyHandoff)
{
    u32 Status = XFSBL_SUCCESS;

    /*
     * Grant RPU0 (MID 0x1B) read+write access to the RPU GIC registers
     * via XMPU_FPD.  The RPU GIC sits in the FPD address space:
     *   0xF9000000-0xF9000FFF  GICD (GIC Distributor)
     *   0xF9001000-0xF9001FFF  GICC (GIC CPU Interface for RPU0)
     *
     * Without this, arm_gic_initialize() writes are silently discarded:
     * GICD_CTLR, GICD_ISENABLER, GICC_CTLR all remain 0, disabling all
     * GIC-routed interrupts (TTC timer, UART RX/TX).
     *
     * IPI_ISR (0xFF310010, LPD_XPPU) remains blocked — IPI delivery uses
     * spin-poll (zynqr5_rptun_ipi_check) instead of GIC ISR.
     *
     * STRT/END store bits [27:0] of the address shifted right by 12.
     * MSTR: MSK[31:16] | ID[15:0].  MSK=0x03FF matches 10-bit MID.
     * CONFIG: REGNNS(3)|WRALWD(2)|RDALWD(1)|EN(0) = 0xF
     */
#define XMPU_FPD_BASE      0xFD5D0000U
#define XMPU_R_STRT(n)    (XMPU_FPD_BASE + 0x100U + (n) * 0x10U + 0x0U)
#define XMPU_R_END(n)     (XMPU_FPD_BASE + 0x100U + (n) * 0x10U + 0x4U)
#define XMPU_R_MSTR(n)    (XMPU_FPD_BASE + 0x100U + (n) * 0x10U + 0x8U)
#define XMPU_R_CFG(n)     (XMPU_FPD_BASE + 0x100U + (n) * 0x10U + 0xCU)

#define RPU0_MID          0x001BU
#define RPU0_MID_MASK     0x03FFU
#define RPU0_MSTR_VAL     ((RPU0_MID_MASK << 16U) | RPU0_MID)
#define XMPU_CFG_OPEN     0x0000000FU  /* EN | RDALWD | WRALWD | REGNNS */

    /* Region 14: RPU GICD 0xF9000000-0xF9000FFF (4 KB) */
    Xil_Out32(XMPU_R_STRT(14), 0x000F9000U);
    Xil_Out32(XMPU_R_END(14),  0x000F9000U);
    Xil_Out32(XMPU_R_MSTR(14), RPU0_MSTR_VAL);
    Xil_Out32(XMPU_R_CFG(14),  XMPU_CFG_OPEN);

    /* Region 15: RPU GICC 0xF9001000-0xF9001FFF (4 KB) */
    Xil_Out32(XMPU_R_STRT(15), 0x000F9001U);
    Xil_Out32(XMPU_R_END(15),  0x000F9001U);
    Xil_Out32(XMPU_R_MSTR(15), RPU0_MSTR_VAL);
    Xil_Out32(XMPU_R_CFG(15),  XMPU_CFG_OPEN);

    return Status;
}
```

> **Note:** If the LPD_XPPU scan (Part 1) finds that the block is in LPD_XPPU rather than
> XMPU_FPD, replace the XMPU_FPD code above with the LPD_XPPU aperture write instead:
> ```c
> /* LPD_XPPU aperture N (from scan) — unlock RPU GIC for RPU0 */
> #define LPD_XPPU_APERT(n)  (0xFF980100U + (n) * 4U)
> u32 perm = Xil_In32(LPD_XPPU_APERT(N));
> perm |= (1U << 19) | (1U << 2) | (1U << 0);  /* enable | RPU0 | APU */
> Xil_Out32(LPD_XPPU_APERT(N), perm);
> ```

---

## Part 4 — Build and Flash BOOT.BIN

```sh
cd /home/kevin/dev/Petalinux-KV260/xilinx-kv260-starterkit-2024.2

# Option A (bbappend): normal build flow
petalinux-build -c fsbl-firmware
petalinux-package --boot --fsbl images/linux/zynqmp_fsbl.elf \
                  --u-boot images/linux/u-boot.elf \
                  --pmufw images/linux/pmufw.elf \
                  --fpga images/linux/*.bit \
                  --force \
                  --out images/linux/BOOT.BIN

# Flash to SD card (assumes card is at /dev/sdX)
sudo bash ../flash_sd.sh --boot-only /dev/sdX
# or:
sudo bash ../flash_sd.sh --boot-only /dev/sdX --bin images/linux/BOOT.BIN
```

Power-cycle the board (BOOT.BIN changes require a full power cycle, not just remoteproc restart).

---

## Part 5 — Verify After BOOT.BIN Flash

```sh
# Confirm GIC writes now stick
sudo devmem 0xF9000108 32 0x00000082   # write GICD_ISENABLER[2]
sudo devmem 0xF9000108 32             # must read 0x00000082 — if still 0, fix failed

# Load firmware
echo stop | sudo tee /sys/class/remoteproc/remoteproc0/state
sleep 2
echo start | sudo tee /sys/class/remoteproc/remoteproc0/state
sleep 3

# TTC ISR running?
sudo devmem 0x1ED3FFDC 32   # TIMER_CNT — expect ~3000 at 1 kHz after 3 s

# rpmsg ping still works?
sudo python3 ~/rpmsg_ping_test.py 10

# NSH keyboard (connect ttyUSB0 at 115200, try typing)
```

---

## Part 6 — NuttX Changes After BOOT.BIN Fix

The NuttX side is already committed. Once BOOT.BIN is fixed and GIC writes work,
the following ISRs will activate automatically — no NuttX code changes needed:

| ISR | IRQ | NuttX file | Action |
|-----|-----|-----------|--------|
| TTC timer | 71 | `zynqr5_timerisr.c:185` | `up_enable_irq(71)` already called |
| UART0 RX/TX | 53 | `zynqr5_serial.c:244` | `up_enable_irq(irq)` already called |
| UART1 RX/TX | 54 | `zynqr5_serial.c:244` | `up_enable_irq(irq)` already called |

**IPI (IRQ 65) remains disabled** — `up_enable_irq(IPI_IRQ)` is intentionally absent from
`zynqr5_rptun_initialize()`. Enabling it would fire the ISR which must write `IPI_ISR`
(0xFF310010) — that write is blocked by a **separate** LPD_XPPU aperture for the IPI
registers, causing a fatal data abort. Spin-poll is the permanent IPI solution.

### Optional: switch rptun from spin-poll to blocking sem

Once TTC ISR is confirmed working (scheduler ticks), rptun_thread can block on
`nxsem_wait_uninterruptible` instead of spinning:

In `platforms/nuttx/NuttX/nuttx/drivers/rptun/rptun.c`, inside the `#ifdef CONFIG_ZYNQR5_RPTUN` block of `rptun_thread()`, the spin-poll loop can be replaced by the standard `nxsem_wait_uninterruptible(&priv->semrx)` — but only after confirming that `zynqr5_rptun_ipi_check()` is being called from the IPI ISR (or an alternative wake path). With spin-poll removed, the IPI notification path must be replaced by an alternative (e.g., a periodic timer callback or the IPI ISR if IPI_ISR writes are unlocked separately).

---

## Summary Checklist

- [ ] Run `xppu-gic-unlock.sh` scan — confirm blocking aperture and which unit (LPD_XPPU or XMPU_FPD)
- [ ] Try temporary devmem unlock → verify `GICD_ISENABLER` write sticks
- [ ] Reload firmware → confirm `TIMER_CNT` increments (~1000/s)
- [ ] Add FSBL hook to PetaLinux meta-user layer
- [ ] `petalinux-build -c fsbl-firmware && petalinux-package --boot`
- [ ] Flash BOOT.BIN: `flash_sd.sh --boot-only /dev/sdX`
- [ ] Power-cycle board; confirm `TIMER_CNT` increments without manual devmem patch
- [ ] Confirm `rpmsg_ping_test.py 10` still passes (spin-poll rpmsg unaffected)
- [ ] Confirm NSH keyboard works on ttyUSB0
