# XMPU_FPD Unlock — ATF BL31 patch (PetaLinux 2024.2)

## Root cause

The XMPU_FPD configuration registers (0xFD5D0000) reside in the **FPD Secure
aperture** — they are TrustZone EL3-only and inaccessible from Linux EL1
regardless of any lock bit.  `gic_setup_r5.py --diagnose` gets SIGBUS here
because the MMU/interconnect blocks the access at the TrustZone level, not
because of a software lock.

The XMPU_FPD itself controls which AXI master IDs can reach which FPD
addresses.  The stock KV260 PetaLinux 2024.2 (BOOT.BIN) configures it such
that RPU master IDs (0x1B/0x1C) cannot access the GIC aperture
(0xF9010000–0xF902FFFF), causing synchronous external aborts when R5F
tries to read GICD or GICC.

The only path to change XMPU_FPD is from EL3 (ATF BL31) before the
handoff to non-secure world.

---

## Patch: add an XMPU_FPD region for RPU0 → GICC in ATF BL31

### 1. Get the source

```bash
git clone --depth 1 -b xilinx-v2024.2 \
    https://github.com/Xilinx/arm-trusted-firmware.git atf-2024.2
cd atf-2024.2
```

### 2. Apply the region patch

Edit `plat/xilinx/zynqmp/bl31_zynqmp_setup.c`.  Add after the existing
`bl31_platform_setup()` body, before the `return` or at the end of the
initialization sequence:

```c
/* -----------------------------------------------------------------------
 * Allow RPU0 (AXI master ID 0x1B) read+write access to the GIC CPU
 * interface (0xF9020000–0xF9021FFF) so interrupt-driven R5F firmware
 * can acknowledge interrupts via GICC_IAR.
 *
 * XMPU_FPD region layout (each region = 0x10 bytes, starting at 0x100):
 *   [0x00] RSTART  = PA_start >> 12
 *   [0x04] REND    = PA_end   >> 12  (inclusive)
 *   [0x08] RMASTERS = (1) | (MID<<4) | (MASK<<16)
 *   [0x0C] RCONFIG  = RDALLOWED|WRALLOWED|NSCHECKTYPE|REGIONENBL
 * ----------------------------------------------------------------------- */
#define XMPU_FPD_BASE   UL(0xFD5D0000)
#define GICC_PA_START   UL(0xF9020000)
#define GICC_PA_END     UL(0xF9021FFF)
#define RPU0_MID        0x1BU

static void zynqmp_xmpu_fpd_open_gicc_for_rpu(void)
{
    uintptr_t region = XMPU_FPD_BASE + 0x100U; /* use region 0 */

    /* Skip if already configured (e.g. warm boot) */
    if (mmio_read_32(region + 0x0CU) & BIT(4)) {
        return;
    }

    mmio_write_32(region + 0x00U, (uint32_t)(GICC_PA_START >> 12));
    mmio_write_32(region + 0x04U, (uint32_t)(GICC_PA_END   >> 12));
    mmio_write_32(region + 0x08U,
                  (1U) |
                  ((RPU0_MID & 0x3FU) << 4U) |
                  (0x3FU << 16U));          /* exact MID match */
    mmio_write_32(region + 0x0CU,
                  BIT(0) |   /* RDALLOWED  */
                  BIT(1) |   /* WRALLOWED  */
                  BIT(3) |   /* bypass NS check */
                  BIT(4));   /* REGIONENBL */
}
```

Call it at the end of `bl31_platform_setup()`:

```c
void bl31_platform_setup(void)
{
    /* ... existing code ... */
    zynqmp_xmpu_fpd_open_gicc_for_rpu();
}
```

### 3. Build BL31

```bash
# aarch64 cross-compiler (adjust path to your Xilinx toolchain)
export CROSS_COMPILE=aarch64-none-elf-

make PLAT=zynqmp RESET_TO_BL31=1 bl31
# output: build/zynqmp/release/bl31/bl31.elf
```

### 4. Rebuild BOOT.BIN

You need the original BOOT.BIN components.  Copy the SD card's BOOT.BIN to
the host and unpack with Xilinx `bootgen`:

```bash
# List partitions
bootgen -read BOOT.BIN

# Typical KV260 BOOT.BIN BIF (adjust partition offsets from bootgen output):
cat > rebuild.bif << 'EOF'
the_ROM_image:
{
    [bootloader, destination_cpu=a53-0] fsbl.elf
    [pmufw_image] pmufw.elf
    [destination_cpu=a53-0, exception_level=el-3, trustzone] bl31.elf    /* <- our patched BL31 */
    [destination_cpu=a53-0, exception_level=el-2] u-boot.elf
}
EOF

bootgen -image rebuild.bif -arch zynqmp -o BOOT_new.BIN -w
```

If you don't have the individual ELF components, extract them from the
existing BOOT.BIN using `bootgen -read` and `dd` at the reported offsets.

### 5. Deploy

```bash
# Mount the KV260 SD card BOOT partition and replace
cp BOOT_new.BIN /mnt/sdcard/BOOT.BIN
umount /mnt/sdcard
# Reboot the KV260
```

---

## Verify after reboot

```bash
# Run the GIC routing (GICD side — doesn't crash R5F)
sudo python3 gic_setup_r5.py --route-only

# Stop current R5F firmware
echo stop | sudo tee /sys/class/remoteproc/remoteproc0/state

# Deploy interrupt-driven NuttX firmware (Phase 3)
sudo cp nuttx /lib/firmware/rproc-ff9a0000.rf5ss:r5f@0-fw

# Now enable the interrupt and start
sudo python3 gic_setup_r5.py   # full configure — enables GICD_ISENABLER
echo start | sudo tee /sys/class/remoteproc/remoteproc0/state
```

Watch UART1 (`/dev/ttyUSB1` on host): if there are no synchronous abort
crashes and the tick output continues, GICC is accessible.

dmesg should show no `xmpu-fpd: access violation` lines.

---

## Fallback: polled TTC (current working state)

If the ATF patch is deferred, the firmware continues to work in polled
mode.  `gic_setup_r5.py --route-only` can still be applied (safe) to
pre-position GICD for when the patch is eventually deployed.  The polled
TTC path is sufficient for Phase 4 (HRT driver) and Phase 5 (PX4 boot).
