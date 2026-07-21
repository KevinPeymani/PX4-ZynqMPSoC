# BOOT.BIN XMPU/XPPU Fix: Enable RPU GIC Writes

> ## ✅ RESOLVED (2026-07-21) — GIC writes stick; TTC ISR confirmed firing via GIC
>
> Built and flashed the Part 0 isolated-bit probe (`zynqr5_irq.c`) to
> `/lib/firmware/nuttx.elf` and reloaded via remoteproc. Live board readback:
>
> | Breadcrumb | Value | Meaning |
> |---|---|---|
> | `0x1ED3FFB4` (isolated ISENABLER[2] readback) | `0x00000082` | our probe's bit 1 (IRQ 65) stuck; bit 7 (IRQ 71/TTC) also set (carried over from a prior boot — GIC state persists across remoteproc stop/start) |
> | `0x1ED3FFA4` (GICD_CTLR) | `0x00000001` | distributor enabled (NONSECENAB) — **no longer reads 0** |
> | `0x1ED3FFAC` (GICC_PMR) | `0x000000F8` | priority mask programmed — **no longer reads 0** |
> | `0x1ED3FFA8` (GICC_RPR, post-active-clear) | `0x000000FF` | idle, correct |
> | `0x1ED3FFA0` (GICD_TYPER) | `0x0000FC25` | matches known-good value (sanity check) |
>
> **The TTC ISR is demonstrably firing through the GIC, not polling.**
> `zynqr5_timer_poll()` is a compiled-out no-op and `up_idle()` only does
> `wfi` (see `zynqr5_timerisr.c`) — the *only* writer of the `TIMER_CNT`
> breadcrumb (`0x1ED3FFDC`) is `zynqr5_timer_interrupt()`, the actual GIC
> ISR. It incremented ~5300 in a 2 s window on live hardware, and the three
> ISR checkpoints (entry / before-sched / after-sched at `0x1ED3FF70` /
> `...6C` / `...68`) advance in lockstep across repeated reads — the full
> ISR body is running to completion, repeatedly, right now. `rpmsg_ping_test.py
> 10` still passes 10/10 (no regression from these changes).
>
> **The entire premise of this document — "GIC writes are silently
> discarded" — no longer holds on current hardware.** The 2026-07-09 finding
> was real at the time, but something in the many small fixes landed since
> (chip.h address correction, the stale-active-interrupt clear added to
> `up_irqinitialize()`, ordering/barrier additions) evidently fixed the
> underlying issue as a side effect, and nobody re-ran the base case before
> escalating to XPPU/XMPU/security-banking theories. **Do not pursue Parts
> 1–4 (XMPU_FPD / FSBL patching) — there is currently no evidence any
> protection unit is involved.** The dormant FSBL patch
> (`recipes-bsp/embeddedsw/files/0001-fsbl-xmpu-rpu-gic-unlock.patch`) should
> stay unapplied.
>
> **New, much smaller anomaly to chase next:** the TTC fires at roughly
> 2.6 kHz, not the ~100 Hz (`CONFIG_USEC_PER_TICK`=10 ms) it's configured
> for — check `TTC_INTERVAL_VAL`/prescaler actually landing in the TTC's own
> registers (a *different* peripheral than the GIC; this bug is independent
> of everything above).
>
> **UART RX follow-up (2026-07-21):** while verifying the GIC fix with UART RX
> interrupts, found the NuttX console was misconfigured onto UART1
> (`CONFIG_ZYNQR5_UART1_CONSOLE=y`) — but Linux's own device tree has
> `/axi/serial@ff010000` (UART1) as `status="okay"` and its actual console
> (`ttyPS1`/`serial1`), while `/axi/serial@ff000000` (UART0) is
> `status="disabled"` in Linux — i.e. UART0 is the one actually free for the
> R5F. Fixed `nsh/defconfig` to `CONFIG_ZYNQR5_UART0_CONSOLE=y`.
>
> That surfaced a second, unrelated problem: after the fix, breadcrumbs
> confirmed `zynqr5_setup()` genuinely runs with `base=0xFF000000` (UART0),
> issuing `putreg32(UART_CR_RXEN|UART_CR_TXEN, 0xFF000000)` — but
> `devmem 0xFF000000 32` still reads back `0`. This isn't the GIC/security
> issue from above (different peripheral, different symptom: the *write
> itself* apparently never latches, no fault). Checked
> `psu_init_gpl.c`: it explicitly enables `CRL_APB_UART1_REF_CTRL` (`CLKACT`
> bit, offset `0xFF5E0078`) for UART1, but **never writes
> `CRL_APB_UART0_REF_CTRL` (offset `0xFF5E0074`) at all** — UART0's
> reference clock was likely never enabled by this hardware design, since
> Linux doesn't use it. An unclocked APB peripheral can accept a posted bus
> write with no error while the register never actually latches — that
> matches the symptom exactly.
>
> **Not yet applied — needs explicit go-ahead before touching CRL_APB.**
> [[feedback-crl-apb-reset]] documents a prior incident where a bare-literal
> write to a different CRL_APB register (`RST_LPD_TOP`) cleared unrelated
> reset bits and hung Linux, requiring a power cycle. Any UART0 clock-enable
> attempt must read-modify-write `0xFF5E0074` (`CLKACT` is bit 24 per the
> UART1 register's bit layout — verify against UG1085 before writing, don't
> assume the two registers are laid out byte-identically without checking).
>
> **UART0 pin search (2026-07-21): no MIO slot available on this SOM —
> deferred, don't retry without a new plan.** Checked every MIO pair Vivado's
> UART0 dropdown offered (it only offers fixed groups, not arbitrary ranges —
> a ZU+ IOU-mux restriction) against both the Vivado PS config
> (`kria_starter_kit.bd`) and the live board's `/sys/kernel/debug/gpio` +
> devicetree `status`. Every single MIO pin 0–77 on this board is already
> committed:
>
> | MIO range | In use for |
> |---|---|
> | 0–5 | QSPI boot flash |
> | 6, 9–11 | SPI1 → the onboard **TPM** |
> | 7–8 | LED1/LED2 (heartbeat, vbus_det) |
> | 12 | `FWUEN` firmware-update-enable strap |
> | 13–23 | **eMMC** (DAT0-7/CMD/CLK/RST) |
> | 24–25 | I2C1 (also drives the USB hub's config) |
> | 26 | PMU GPI0 |
> | 27–30 | DisplayPort AUX |
> | 31 | PMU GPI5 |
> | 35 | PMU GPO3 |
> | 36–37 | UART1 (Linux console, `ttyPS1`) |
> | 38 | Ethernet PHY reset |
> | 39–51 | **SD card slot** (incl. card-detect/power-enable) |
> | 52–63 | **USB0** — ULPI bus to the onboard Microchip USB hub chip driving the board's physical USB-A ports (confirmed live via `lsusb`) |
> | 64–77 | Ethernet (ENET3) + MDIO |
>
> Also ruled out: repurposing USB0's MIO pins does **not** give "UART over a
> USB cable" even in principle — MIO 52–63 is a digital ULPI bus to a
> fixed-function PHY/hub chip, not a passthrough; a UART signal driven into
> those pins wouldn't reach the physical USB connector at all, and would
> break the board's real USB ports for nothing.
>
> **The only remaining path to a physically-reachable UART0 is EMIO** —
> route it through the PL fabric to a free carrier-board pin (e.g. a PMOD,
> if genuinely unused). That needs actual PL synthesis/implementation (not
> just a PS-config regen) and was **not attempted** — deferred by explicit
> decision on 2026-07-21, since GIC interrupt delivery was already proven
> independently via the TTC ISR test. Vivado block-design changes for this
> were discarded, not saved — the `.bd`/`.xpr` are unmodified.
>
> **Code hygiene note:** two DDR breadcrumb addresses are unintentionally
> shared by unrelated debug writes — `0x1ED3FFCC` (zynqr5_irq.c's "PHASE"
> marker vs. rptun's `DBG_IPI_SNAP+12`) and `0x1ED3FFDC` (zynqr5_timerisr.c's
> `TIMER_CNT` vs. rptun's "up_idle heartbeat" zero-init). Harmless today
> because of write ordering, but worth giving each its own slot before it
> causes real confusion.
>
> Original XPPU/XMPU reassessment kept below for history — superseded by the
> above.

> ## ⚠️ Reassessment (2026-07-20) — read this before doing Part 3
>
> Research this session found real problems with the plan below:
>
> 1. **The FSBL-hook patch in Part 3 is not standard practice and is explicitly
>    discouraged by Xilinx.** XAPP1320 ("Isolation Methods in Zynq UltraScale+
>    MPSoCs") states plainly that reconfiguring protection-unit registers at
>    runtime "is not recommended for safe or secure systems," and that the
>    supported mechanism is the Vivado **PS Configuration → Isolation
>    Configuration** wizard, which is what *generates* `psu_lpd_xppu_data()` /
>    `psu_fpd_xmpu_data()` in `psu_init_gpl.c` — not hand-written `Xil_Out32`
>    calls in `XFsbl_HookBeforeHandoff()`.
> 2. **Two "official-looking" alternatives are dead ends — don't chase them:**
>    the PMU firmware `pm_cfg_obj.c` config object has no GIC-address entries
>    at all (verified directly against `Xilinx/embeddedsw` source — it only
>    covers IPI, memory, and standard peripherals), and `petalinux-config`'s
>    PMU Firmware submenu has no way to point at a custom config object either
>    (per UG1144, it only exposes compiler flags).
> 3. **This project's own Vivado hardware design never configures XPPU or XMPU
>    at all.** `psu_lpd_xppu_data()`, `psu_fpd_xmpu_data()`, and
>    `psu_ocm_xmpu_data()` in
>    `Petalinux-KV260/xilinx-kv260-starterkit-2024.2/project-spec/hw-description/psu_init_gpl.c`
>    are empty stubs — comments listing aperture names only, `return 1;`, zero
>    real register writes. XPPU/XMPU are believed to be **permissive by
>    default** unless a design opts into the Isolation wizard. That means the
>    original diagnosis — "LPD_XPPU/XMPU_FPD blocks it" — is not actually
>    confirmed; an unconfigured protection unit shouldn't be blocking anything.
> 4. **The RPU GIC's address range (`0xF9000000`–`0xF9001FFF`) does not even
>    appear in the LPD_XPPU aperture list.** `psu_lpd_xppu_data()`'s own
>    comments enumerate every LPD_XPPU aperture by name and address (UART0,
>    IPI_0/1/2, TTC0-3, GPIO, ...) — all of them `0xFF...`-prefixed. There is
>    no aperture anywhere near `0xF9000000`. IPI channels genuinely are
>    XPPU-gated apertures (that diagnosis, in [[project-xppu-ipi-isr-abort]],
>    still stands) — the RPU's own GIC registers are not in that list at all.
> 5. **There's an unresolved internal contradiction worth checking first.** The
>    2026-07-07 test found `GICD_ISENABLER[2]` reads back as `0` after
>    `arm_gic_initialize()` — i.e. "all writes ignored." But IRQ 65 (the IPI
>    interrupt) lives in exactly that register, bit 1 (`65 / 32 = 2`,
>    `65 % 32 = 1`). The 2026-07-10 finding requires that exact bit to have
>    gotten set — the ISR only fires (and then crashes on the `IPI_ISR` write)
>    if the GIC actually dispatched IRQ 65, which requires `GICD_ISENABLER[2]`
>    bit 1 to be 1. If that register were truly always-zero, IRQ 65 could
>    never fire via the GIC path at all. These two observations don't fit
>    together under a blanket "every write to this range is discarded" theory.
>
> **Leading alternative hypothesis:** GIC group/security-state banking, not an
> external protection unit. PL390/GICv1 splits some registers/bits by security
> group (Secure vs Non-secure, Group0 vs Group1) — a write to a Secure-gated
> field from a Non-secure context is architecturally RAZ/WI (silently
> ignored), which produces exactly the observed symptom (write "succeeds" with
> no fault, read-back shows the old value) without needing XPPU or XMPU
> involved at all. It would also explain the contradiction above: `GICD_CTLR`
> (global enable, often Secure-only for one of its bits) staying at 0 while
> individual per-IRQ `ISENABLER` bits (assigned to Group1/Non-secure) still
> take effect. **This hasn't been verified either — it's a hypothesis, not a
> new confirmed root cause.** But it's cheap to check (pure software, no
> board/BOOT.BIN changes) and should be ruled out before touching Vivado or
> the FSBL. See **Part 0** below.
>
> Corroborating negative result: neither Zephyr's Cortex-R5 UltraScale+ port
> nor the RTEMS RPU BSP documentation mentions ever hitting an XPPU/XMPU block
> on RPU GIC access. If this were a routine ZU+ gotcha, you'd expect it to
> show up in at least one of those.
>
> **Bottom line:** treat Part 1–4 below as what to do only *after* Part 0 (new,
> below) rules out the security-state hypothesis. Don't jump straight to the
> FSBL patch in Part 3.

---

## Part 0 progress (2026-07-21)

- **GICD_TYPER decoded from the existing breadcrumb** (`devmem 0x1ED3FFA0 32` →
  `0x0000FC25`): bit 10 (`SecurityExtn`) = **1**. The RPU GICD at `0xF9000000`
  genuinely implements ARM Security Extensions — its registers really are
  Secure/Non-secure banked. This doesn't yet prove banking is *why* writes
  read back 0 (that also requires knowing which state the R5 is in), but it
  removes "this GIC has no security extensions to bank" as a possible
  counter-argument.
- **Checked whether the R5's security state is set anywhere in the Vivado/FSBL
  sources already on disk — it isn't.** `psu_init_gpl.c` only programs
  TrustZone *classification* for bus masters (`LPD_SLCR_SECURE_SLCR_ADMA_TZ`,
  `FPD_SLCR_SECURE_SLCR_{DPDMA,PCIE,GDMA}_TZ`, `LPD_SLCR_SECURE_SLCR_USB_TZ`)
  — i.e. "is this DMA/peripheral's bus traffic marked secure," nothing about
  which security state the RPU *core* itself boots into. No register write
  anywhere in that file targets the `RPU` block at `0xFF9A0000`, and
  `psu_init_gpl.h` doesn't even define registers in that range. The FSBL hook
  files present (`xfsbl_hooks.c`) are empty, as previously noted.
  **Conclusion: R5 Secure vs Non-secure state cannot be determined from static
  analysis of this board's generated sources — it needs a runtime probe.**
- Found `project-spec/meta-user/recipes-bsp/embeddedsw/files/0001-fsbl-xmpu-rpu-gic-unlock.patch`
  already sitting in the PetaLinux tree — this is Part 3's XMPU_FPD-region
  patch, written before the reassessment above, still asserting the
  now-doubted "XMPU_FPD blocks it" framing in its commit message. It is not
  wired into the build (no matching `.bbappend`), so it's inert. **Don't
  apply it** until Part 0's runtime probe actually implicates XMPU_FPD —
  right now nothing points at XMPU_FPD specifically.

## Part 0 — Rule out GIC secure/non-secure banking (do this first)

Before touching any protection unit, check what security state the R5 core is
actually in when `up_irqinitialize()` runs, and whether that plausibly
explains partial-write behavior:

- What mode/security state is the R5F executing in at the point
  `arm_gic0_initialize()` / `arm_gic_initialize()` run? Cortex-R5 supports
  Security Extensions (Monitor mode) — check whether NuttX's boot code ever
  switches state, or assumes Secure state without setting it up.
- Re-run the write test from a bit-by-bit angle instead of assuming
  "everything in the range is blocked": after `arm_gic_initialize()`, write
  and read back `GICD_ISENABLER[2]` (`0xF9000108`) for a Group1/IPI-relevant
  bit specifically, separately from `GICD_CTLR` (`0xF9000000`, global enable).
  If `ISENABLER` writes stick but `CTLR`'s enable bit doesn't, that's strong
  evidence for the security-banking hypothesis over an external protection
  unit (which would normally block the whole aperture uniformly, not
  register-by-register).
- Check `GICD_TYPER`/`GICC_IIDR` and any `ICDDCR`/security-configuration
  register PL390 exposes for a Non-secure-view vs Secure-view distinction.
- Only if this comes back inconclusive (all registers uniformly refuse
  writes regardless of security state) does Part 1's XPPU/XMPU diagnostic
  below become the more likely explanation again.

---

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

**Run Part 0 first.** Everything below assumes the security-state hypothesis
was checked and ruled out.

### 1a. Scan LPD_XPPU apertures from Linux

The `xppu-gic-unlock.sh` script (same directory as this file) scans all 80 LPD_XPPU
apertures and dumps their current permission words:

```sh
# On the board (as root):
sudo bash /path/to/xppu-gic-unlock.sh
```

Look for an aperture with a non-zero value that maps near `0xF9000000`. **Expect this
scan to find nothing relevant.** `psu_lpd_xppu_data()` in this board's own
Vivado-generated `psu_init_gpl.c` enumerates every LPD_XPPU aperture by name/address
(UART0, IPI_0/1/2, TTC0-3, GPIO, IOU_SLCR, ...) — all `0xFF...`-prefixed. None of them
are anywhere near `0xF9000000`. If the scan comes back empty, that's not a failed
diagnostic — it's evidence that `0xF9000000` was never an LPD_XPPU-covered address
in the first place, and the real mechanism is something else (see the reassessment
at the top of this document).

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

> **Not recommended as a first move.** Per the reassessment at the top of this
> document, Xilinx's own app note (XAPP1320) explicitly discourages hand-writing
> protection-unit register pokes into an FSBL hook for exactly this kind of
> change, and the standard supported path is regenerating the Vivado hardware
> design through the **PS Configuration → Isolation Configuration** wizard
> instead (which is what would populate `psu_lpd_xppu_data()` /
> `psu_fpd_xmpu_data()` for real, rather than leaving them as empty stubs).
> Whether that wizard even exposes the RPU-GIC as a selectable resource is
> unconfirmed. Keep this section as a documented fallback only, and only reach
> for it after Part 0 and Part 1 have actually pinned down that XMPU_FPD is
> the mechanism at fault — don't apply it speculatively.

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

- [ ] **Part 0 first:** check R5 security/mode state at GIC-init time; test
      `GICD_ISENABLER[2]` write-then-readback separately from `GICD_CTLR` —
      rule out GIC secure/non-secure banking before assuming XPPU/XMPU
- [ ] Only if Part 0 is inconclusive: run `xppu-gic-unlock.sh` scan — expect
      it to find nothing near `0xF9000000` (see Part 1 note); if truly
      inconclusive, escalate to Vivado's Isolation Configuration wizard before
      the FSBL hook, not instead of it
- [ ] Try temporary devmem unlock → verify `GICD_ISENABLER` write sticks
- [ ] Reload firmware → confirm `TIMER_CNT` increments (~1000/s)
- [ ] (Fallback only, not first choice) Add FSBL hook to PetaLinux meta-user layer
- [ ] `petalinux-build -c fsbl-firmware && petalinux-package --boot`
- [ ] Flash BOOT.BIN: `flash_sd.sh --boot-only /dev/sdX`
- [ ] Power-cycle board; confirm `TIMER_CNT` increments without manual devmem patch
- [ ] Confirm `rpmsg_ping_test.py 10` still passes (spin-poll rpmsg unaffected)
- [ ] Confirm NSH keyboard works on ttyUSB0
