# DDR Access Control & Cache Coherency Between RPU0 (NuttX) and APU (Linux)

## Problem this explains

R5-side debug breadcrumbs at the top of the firmware's DDR carveout
(`0x1ED3FFE0`-`0x1ED3FFFF`) are only *partially* visible from Linux via
`devmem`: some 4-byte words in a 32-byte cache line read the live value the
R5 wrote (e.g. `RPTUN_DBG_STEP`), while a neighboring word in the *same*
cache line (`RPTUN_DBG_PHASE`) reads a stale/poison value (`0xFFFFFFFF`)
that the R5 never wrote. Investigated 2026-07-10; root cause traced to a
stale Linux devicetree reservation, not an R5-side bug. See analysis below.

---

## Three independent access-control layers

### Layer 1 — XMPU_DDR (hardware, FSBL-configured, gates both cores)

- 6 XMPU_DDR instances (one per DDR AXI port), registers at
  `0xFD000000 + N*0x10000` for `N=0..5`. Each has 16 regions
  (`R00`..`R15`), 4 KB granularity (region base/limit store `addr >> 12`).
- Per-region permission register reset value is `0x00000008`
  (`EN=0, RDALWD=0, WRALWD=0` — region disabled). Global `CTRL` register
  reset value is `0x00000003` (`DEFRDALWD=1, DEFWRALWD=1`).
- **Net effect when no region is enabled: DDR is open read+write to every
  master**, because the default-allow bits govern.
- This project's FSBL (`px4-vitis`) never calls `psu_protection()` (the
  function exists in generated `psu_init_gpl.c` but is unreferenced), and
  `psu_ddr_xmpu0_data()`..`psu_ddr_xmpu5_data()` are comment-only stubs
  that write nothing. **DDR XMPU isolation is not configured at all in
  this build** — DDR is currently wide open. This is a red herring for
  any "R5 can't reach address X" symptom; if that happens, suspect the
  address itself (e.g. the DDR1 vs DDR3 XMPU_FPD story below), not DDR
  isolation.
- The only real XMPU/XPPU write in this FSBL is in `xfsbl_hooks.c`
  (`XFsbl_HookBeforeHandoff()`), which opens `XMPU_FPD` regions covering
  `0xF9010000`-`0xF9021FFF` (the **APU** GIC-400 block) for master ID
  **RPU0 = `0x1B`**. That master-ID value (`0x1B`) is the only one locally
  confirmed; RPU1/APU numeric master IDs were not independently verified.
- Historical note: an earlier BOOT.BIN revision *did* block RPU0 from the
  DDR3 range (`0x30000000`-`0x3FFFFFFF`), forcing the firmware to move
  from `0x3ED00000` to `0x1ED00000` (DDR1 range). That FSBL revision no
  longer matches what's in `px4-vitis` today (current one is a no-op for
  DDR XMPU) — the *original* blocking cause may be gone, but see
  Layer 3 below: the *side effect* of that address move was never fully
  cleaned up.

### Layer 2 — ARMv7-R MPU (R5-local, NuttX-side)

- NuttX unconditionally disables the R5's own MPU at boot
  (`zynqr5_tcmstart.S`, clears `SCTLR.M`) — a deliberate workaround for a
  possible FSBL-configured restriction, not a designed flat-region setup.
- D-cache is globally disabled for this core
  (`arch/arm/src/armv7-r/arm_head.S`, `#define CPU_DCACHE_DISABLE 1`).
- **Net effect: every R5 DDR access is a direct, uncached, unmapped store
  straight to DRAM.** There is no R5-side caching or MPU restriction to
  worry about — any staleness bug is on the Linux/A53 side.

### Layer 3 — Linux devicetree reserved-memory (the actual bug source)

Both the DT source and the currently-built image still use the **old**
`0x3ED00000` carveout, while NuttX has run at `0x1ED00000` since the DDR1
relocation:

- Source: `Petalinux-KV260/xilinx-kv260-starterkit-2024.2/project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`
  ```dts
  rpu0vdev0vring0: rpu0vdev0vring0@3ed40000 { no-map; reg = <0x0 0x3ed40000 0x0 0x4000>; };
  rpu0vdev0vring1: rpu0vdev0vring1@3ed44000 { no-map; reg = <0x0 0x3ed44000 0x0 0x4000>; };
  rpu0vdev0buffer: rpu0vdev0buffer@3ed48000 { no-map; reg = <0x0 0x3ed48000 0x0 0x100000>; };
  rproc_0_reserved: rproc@3ed00000        { no-map; reg = <0x0 0x3ed00000 0x0 0x40000>; };
  ```
- Compiled artifact (`images/linux/system.dtb`) confirmed via `strings` to
  contain the same `3ed*` node names — the stale source was not hand-patched
  in this build output.
- NuttX side has moved on: `boards/amd/kria-kv260/nuttx-config/nsh/defconfig`
  → `CONFIG_RAM_START=0x1ED00000`; `boards/amd/kria-kv260/nuttx-config/scripts/script.ld`
  → `ddr : ORIGIN = 0x1ED00000, LENGTH = 0x00040000`.

**Why this matters:** the devicetree `reserved-memory`/`no-map` binding is
what removes a physical range from Linux's normal memory map and stops the
kernel from creating an ordinary cacheable mapping over it. If the *booted*
dtb still reserves `0x3ed00000` instead of `0x1ed00000`, then
`0x1ed00000`-`0x1ed3ffff` — where the firmware, breadcrumbs, and vrings
actually live now — is just ordinary cacheable System RAM to Linux. A
`devmem` read there goes through whatever cache state the kernel happens to
have left that page in, not a guaranteed-fresh DRAM read. This matches the
observed symptom exactly: only whichever words the kernel happened to
touch/cache read stale; neighboring words in the same line that the kernel
never separately touched read correctly.

A prior session's intended fix was a **manual, on-board, one-off patch**
(`dtc -I dtb -O dts /boot/system.dtb`, sed `3ed`→`1ed`, recompile, backup as
`/boot/system.dtb.bak_3ed`). That patch was never committed to source
control, so it does not survive a PetaLinux rebuild, and it is unverified
whether it is even still live on the current SD card.

---

## Cache coherency: is R5 in the same coherency domain as A53?

**No, not by default, and not enabled in this project.** The RPU has a
"Coherent" bit in `RPU_0_CFG`/`RPU_1_CFG` that routes its DDR traffic
through the CCI-400/ACE coherent fabric shared with the APU cluster; it is
**off by default** (per AMD support guidance), and this project's FSBL
(`xfsbl_handoff.c`) writes `RPU_0_CFG` only for `NCPUHALT`/`VINITHI`, never
the Coherent bit. So RPU0's DDR traffic here is entirely on the
**non-coherent path**, outside CCI-400 snooping.

Implication: even with the R5 D-cache off (so R5 writes land in DRAM
immediately), there is no hardware mechanism that makes the A53 cluster
automatically see that write. The A53 side's ability to observe fresh DRAM
content depends entirely on whether Linux ever cached that physical line —
which loops back to Layer 3: `no-map` correctness is not just tidy, it's
the only thing preventing Linux from caching the region in the first place.
This is also why Xilinx's own reference OpenAMP devicetree snippet
(`psu_cortexr5_0/.../openamp-linux-userspace.dtsi`) uses `no-map` with no
`dma-coherent` property — consistent with the RPU path being non-coherent
by design.

---

## Verification checklist

1. **Check what's actually booted on the board** (do this first):
   ```sh
   sudo dtc -I fs -O dts /proc/device-tree > /tmp/live.dts
   grep -A2 "rproc@\|rpu0vdev0" /tmp/live.dts
   ```
   `3ed*` → bug reproduced, patch missing/not-live. `1ed*` → patch is live,
   look elsewhere (see step 3).

2. **If still `3ed*`:** fix it at the source instead of re-patching on
   board — edit all six `3ed*` node names/addresses in `system-user.dtsi`
   to `1ed*`, rebuild `system.dtb`, redeploy. A source fix survives the
   next PetaLinux rebuild; an on-board `dtc` patch does not.

3. **If already `1ed*` but the symptom persists:**
   - Check whether `/boot/system.dtb` is actually what U-Boot's `boot.scr`
     loads, vs. a dtb baked into `image.ub` (FIT) that never got the fix.
   - `cat /proc/iomem | grep -i 1ed0` — the range must show up only as the
     reserved-memory region (or be invisible), never as generic
     "System RAM".

4. **Independent smoke test** (doesn't require decoding the dtb): write a
   known pattern via `devmem` from Linux to the breadcrumb address, restart
   only the R5 (`echo stop`/`echo start` on `remoteproc0`), and check
   whether the mismatch is one-directional (R5 write not seen by Linux —
   matches this hypothesis) or bidirectional (points elsewhere).

## What's still unconfirmed

- Whether the on-board `/boot/system.dtb` currently has the `3ed`→`1ed`
  patch applied (no SSH check performed as of this writing).
- Numeric XMPU/XPPU master-ID values for RPU1 and APU (only RPU0=`0x1B` is
  locally verified, via the working `XMPU_FPD` hook).
- Exact bit offset for `RPU_0_CFG`'s Coherent bit from a primary AMD
  register-reference doc (existence and default-off state corroborated by
  two independent sources, but no directly quotable register table).
- Whether `image.ub` bundles its own copy of `system.dtb` that could
  diverge from the standalone file inspected here.
