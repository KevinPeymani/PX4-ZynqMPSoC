#!/usr/bin/env python3
"""
boards/amd/kria-kv260/scripts/gic_setup_r5.py

Phase 2: Prepare ZynqMP GIC for R5F (RPU0) interrupt-driven operation.

Background
----------
The polled-TTC Phase 1 firmware works around a hardware protection barrier:
XMPU_FPD blocks RPU master ID 0x1B from accessing the FPD (0xF9000000+),
which includes GICD (0xF9010000) and GICC (0xF9020000).  Any attempt by
R5F to read GICC_IAR causes a synchronous external abort.

The XMPU_FPD configuration registers (0xFD5D0000) are in the FPD Secure
aperture — they are TrustZone EL3-only and inaccessible from Linux EL1
(reading causes SIGBUS regardless of the LOCK bit).  Unlocking GICC for
R5F therefore requires an ATF BL31 patch; see xmpu_atf_smc.md.

This script handles the GIC Distributor side, which IS accessible from
Linux and must be configured regardless of the XMPU state:

  1. (GICD) Route TTC1 counter-0 (GIC SPI 71) to RPU0 CPU interface.
  2. (GICD) Set priority so the interrupt passes the GICC_PMR filter.
  3. (GICD) Mark interrupt non-secure (required for non-secure R5F ack).
  4. (GICD) Enable forwarding — ONLY after ATF patch unlocks GICC.

Modes
-----
    sudo python3 gic_setup_r5.py --diagnose    # read-only state dump
    sudo python3 gic_setup_r5.py --route-only  # steps 1-3 only (safe now)
    sudo python3 gic_setup_r5.py               # steps 1-4 (needs GICC unlocked)
    sudo python3 gic_setup_r5.py --dry-run     # print what would change

ZynqMP addresses
----------------
  GICD base : 0xF9010000
  GICC base : 0xF9020000   (TrustZone EL3-protected for RPU via XMPU_FPD)
  XMPU_FPD  : 0xFD5D0000   (FPD Memory Protection Unit — EL3-only access)

  TTC1 counter-0 interrupt: GIC SPI 71 (ZynqMP TRM Table 11-4)
  RPU0 AXI master ID      : 0x1B       (ZynqMP TRM Table 2-3)
  RPU0 GIC CPU interface  : 4          (target byte bit = 0x10)
"""

import sys
import os
import mmap
import struct
import argparse

# ---------------------------------------------------------------------------
# Physical addresses
# ---------------------------------------------------------------------------
GICD_BASE    = 0xF9010000    # GIC-400 Distributor
GICC_BASE    = 0xF9020000    # GIC-400 CPU Interface (banked per CPU)
XMPU_FPD    = 0xFD5D0000    # FPD Memory Protection Unit

PAGE_SIZE = 0x1000           # 4 KB /dev/mem granularity

# ---------------------------------------------------------------------------
# GIC-400 Distributor register offsets (GICv2 spec)
# ---------------------------------------------------------------------------
GICD_ICDDCR   = 0x000   # Distributor Control
GICD_ICDICTR  = 0x004   # Interrupt Controller Type
GICD_ISENABLER_BASE = 0x100  # Set-Enable (each bit enables one IRQ)
GICD_IPRIORITYR_BASE= 0x400  # Priority (1 byte per IRQ, lower = higher prio)
GICD_ITARGETSR_BASE = 0x800  # Target CPU (1 byte per IRQ, bit per interface)
GICD_ICFGR_BASE     = 0xC00  # Edge/level config (2 bits per IRQ)
GICD_ICDISR_BASE    = 0x080  # Interrupt Security (1=non-secure)

# ---------------------------------------------------------------------------
# TTC1 counter-0 interrupt
# ---------------------------------------------------------------------------
TTC1_C0_IRQ = 71   # GIC SPI 71 (verified from ZynqMP TRM)

# RPU0 GIC CPU interface assignment.
# ZynqMP GIC-400 is connected to 6 CPUs: A53x4 (interfaces 0-3), RPUx2 (4-5).
RPU0_CPU_TARGET = (1 << 4)   # interface 4 = RPU0; adjust to 0x01 if different

# ---------------------------------------------------------------------------
# XMPU_FPD register offsets (ZynqMP TRM Section 13.4)
# ---------------------------------------------------------------------------
XMPU_CTRL    = 0x000   # Global control (bit 0 = enable)
XMPU_ERR1    = 0x004   # Address of failed access
XMPU_ERR2    = 0x008   # Master ID of failed access
XMPU_LOCK    = 0x020   # Lock register (non-zero = locked, no further writes)

# Region n starts at 0x100 + n*0x10; 16 regions (0..15)
XMPU_REGION_BASE   = 0x100
XMPU_REGION_STRIDE = 0x010
XMPU_RSTART  = 0x00   # Region start address   (PA >> 12)
XMPU_REND    = 0x04   # Region end address     (PA >> 12, inclusive)
XMPU_RMASTERS= 0x08   # Master permission word
XMPU_RCONFIG = 0x0C   # Region config

# XMPU RCONFIG bits
RCONFIG_RDALLOWED = (1 << 0)   # Allow reads
RCONFIG_WRALLOWED = (1 << 1)   # Allow writes
RCONFIG_NSCHECKTYPE = (1 << 3) # Bypass TrustZone NS check for this region
RCONFIG_REGIONENBL  = (1 << 4) # Enable this region

# XMPU RMASTERS format
# Bits [11:4] = MID value; bits [25:16] = MID mask; bit 0 = enable check
# Set mask = 0x3F (match all 6 low bits exactly), MID = RPU0 = 0x1B
RPU0_MID       = 0x1B
RPU0_MID_MASK  = 0x3F   # exact match (no wildcard bits)

def rmasters_for_mid(mid, mask=0x3F):
    """Encode XMPU RMASTERS register for a single master ID."""
    return (1) | ((mid & 0x3F) << 4) | ((mask & 0x3F) << 16)

# The GICC aperture spans 0xF9020000–0xF9021FFF (8 KB CPU interface registers)
GICC_RSTART = GICC_BASE >> 12               # 0xF9020
GICC_REND   = (GICC_BASE + 0x1FFF) >> 12   # 0xF9021

# ---------------------------------------------------------------------------
# /dev/mem helpers
# ---------------------------------------------------------------------------
_maps = {}

def _map(phys_base, length=PAGE_SIZE):
    """Return an mmap object for [phys_base, phys_base+length)."""
    page_base  = phys_base & ~(PAGE_SIZE - 1)
    page_count = ((phys_base + length - page_base + PAGE_SIZE - 1)
                  // PAGE_SIZE) * PAGE_SIZE
    key = (page_base, page_count)
    if key not in _maps:
        fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        m  = mmap.mmap(fd, page_count, mmap.MAP_SHARED,
                       mmap.PROT_READ | mmap.PROT_WRITE,
                       offset=page_base)
        os.close(fd)
        _maps[key] = (m, page_base)
    return _maps[key]

def _rd32(phys):
    m, base = _map(phys & ~(PAGE_SIZE - 1), PAGE_SIZE)
    off = phys - base
    return struct.unpack_from("<I", m, off)[0]

def _wr32(phys, val, dry_run=False):
    tag = f"WR32  0x{phys:08X} = 0x{val:08X}"
    if dry_run:
        print(f"  [DRY] {tag}")
        return
    m, base = _map(phys & ~(PAGE_SIZE - 1), PAGE_SIZE)
    off = phys - base
    struct.pack_into("<I", m, off, val)
    m.flush()
    print(f"  {tag}")

# ---------------------------------------------------------------------------
# Diagnostic helpers
# ---------------------------------------------------------------------------
def diagnose():
    print("=" * 60)
    print("GIC Distributor state")
    print("=" * 60)
    ctrl = _rd32(GICD_BASE + GICD_ICDDCR)
    print(f"  GICD_ICDDCR  = 0x{ctrl:08X}  (EN={ctrl & 1})")

    irq = TTC1_C0_IRQ
    # Enable register
    en_reg  = GICD_BASE + GICD_ISENABLER_BASE + (irq // 32) * 4
    en_bit  = (irq % 32)
    en_val  = _rd32(en_reg)
    print(f"\n  TTC1-C0 (IRQ {irq}):")
    print(f"    ISENABLER[{irq//32}] @ 0x{en_reg:08X} = 0x{en_val:08X}"
          f"  (bit {en_bit} = {'ENABLED' if (en_val >> en_bit) & 1 else 'disabled'})")

    # Priority
    prio_reg = GICD_BASE + GICD_IPRIORITYR_BASE + (irq // 4) * 4
    prio_val = _rd32(prio_reg)
    prio_byte = (prio_val >> ((irq % 4) * 8)) & 0xFF
    print(f"    IPRIORITYR  @ 0x{prio_reg:08X} byte{irq%4} = 0x{prio_byte:02X}")

    # Target
    tgt_reg  = GICD_BASE + GICD_ITARGETSR_BASE + (irq // 4) * 4
    tgt_val  = _rd32(tgt_reg)
    tgt_byte = (tgt_val >> ((irq % 4) * 8)) & 0xFF
    print(f"    ITARGETSR   @ 0x{tgt_reg:08X} byte{irq%4} = 0x{tgt_byte:02X}"
          f"  (A53=0x0F, RPU0=0x10, RPU1=0x20)")

    # Security
    sec_reg  = GICD_BASE + GICD_ICDISR_BASE + (irq // 32) * 4
    sec_val  = _rd32(sec_reg)
    sec_bit  = (irq % 32)
    print(f"    ICDISR[{irq//32}]   @ 0x{sec_reg:08X} = 0x{sec_val:08X}"
          f"  (bit {sec_bit} = {'non-secure' if (sec_val >> sec_bit) & 1 else 'SECURE'})")

    print("\n" + "=" * 60)
    print("XMPU_FPD state")
    print("=" * 60)
    print("  NOTE: 0xFD5D0000 is in the FPD Secure aperture (TrustZone EL3-only).")
    print("  Reading from Linux EL1 always causes SIGBUS regardless of LOCK state.")
    print("  Current configuration is visible only from ATF (BL31) context.")
    print("  To unlock GICC for R5F: apply ATF patch — see xmpu_atf_smc.md")


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
def _gicd_route(irq, dry_run=False):
    """Steps 1-3: configure priority, target, security.  Safe to run at any time."""
    byte_shift = (irq % 4) * 8

    # 1. Ensure GICD is enabled
    ctrl = _rd32(GICD_BASE + GICD_ICDDCR)
    if not (ctrl & 1):
        print("  GICD_ICDDCR: enabling distributor")
        _wr32(GICD_BASE + GICD_ICDDCR, ctrl | 1, dry_run)
    else:
        print("  GICD_ICDDCR: already enabled")

    # 2. Priority — 0xA0 so GICC PMR of 0xFF passes it
    prio_reg = GICD_BASE + GICD_IPRIORITYR_BASE + (irq // 4) * 4
    prio_val = _rd32(prio_reg)
    prio_val = (prio_val & ~(0xFF << byte_shift)) | (0xA0 << byte_shift)
    print(f"  GICD_IPRIORITYR[{irq}]: set priority to 0xA0")
    _wr32(prio_reg, prio_val, dry_run)

    # 3. Target — route to RPU0 (CPU interface 4, target byte 0x10)
    tgt_reg  = GICD_BASE + GICD_ITARGETSR_BASE + (irq // 4) * 4
    tgt_val  = _rd32(tgt_reg)
    tgt_byte = (tgt_val >> byte_shift) & 0xFF
    tgt_val  = (tgt_val & ~(0xFF << byte_shift)) | (RPU0_CPU_TARGET << byte_shift)
    print(f"  GICD_ITARGETSR[{irq}]: route to CPU-IF{RPU0_CPU_TARGET.bit_length()-1}"
          f" (was 0x{tgt_byte:02X} → 0x{RPU0_CPU_TARGET:02X})")
    _wr32(tgt_reg, tgt_val, dry_run)

    # 4. Mark non-secure so R5F can acknowledge via GICC_IAR
    sec_reg = GICD_BASE + GICD_ICDISR_BASE + (irq // 32) * 4
    sec_val = _rd32(sec_reg)
    sec_val |= (1 << (irq % 32))
    print(f"  GICD_ICDISR[{irq}]: mark non-secure")
    _wr32(sec_reg, sec_val, dry_run)


def _gicd_enable(irq, dry_run=False):
    """Step 4: enable SPI forwarding.  Only safe AFTER XMPU_FPD is unlocked for RPU."""
    en_reg = GICD_BASE + GICD_ISENABLER_BASE + (irq // 32) * 4
    en_bit = (1 << (irq % 32))
    print(f"  GICD_ISENABLER[{irq}]: enable SPI forwarding")
    _wr32(en_reg, en_bit, dry_run)


def configure(dry_run=False, route_only=False):
    prefix = "[DRY-RUN] " if dry_run else ""
    if route_only:
        print(f"\n{prefix}Routing IRQ {TTC1_C0_IRQ} to RPU0 (GICC enable deferred — XMPU not yet patched)")
    else:
        print(f"\n{prefix}Configuring GIC for R5F interrupt-driven operation")
        print("  PRECONDITION: XMPU_FPD must be patched (xmpu_atf_smc.md Option C)")
        print("  Running without the ATF patch WILL crash R5F on next TTC interrupt.")
    print("-" * 60)

    _gicd_route(TTC1_C0_IRQ, dry_run)

    if not route_only:
        _gicd_enable(TTC1_C0_IRQ, dry_run)
    else:
        print(f"\n  GICD_ISENABLER[{TTC1_C0_IRQ}]: NOT enabled (--route-only).")
        print("  When XMPU_FPD is patched, re-run without --route-only to enable.")

    print(f"\n  XMPU_FPD status: 0xFD5D0000 is TrustZone EL3-only (SIGBUS from Linux).")
    print("  Unlock RPU → GICC via ATF BL31 patch: see xmpu_atf_smc.md")
    print("\nDone.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--diagnose", action="store_true",
                   help="Read and display current GIC state without changing anything")
    p.add_argument("--route-only", action="store_true",
                   help="Configure priority/target/security but do NOT enable forwarding "
                        "(safe to run before ATF XMPU patch is applied)")
    p.add_argument("--dry-run", action="store_true",
                   help="Print what would be written without writing")
    args = p.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (requires /dev/mem access)")
        sys.exit(1)

    if args.diagnose:
        diagnose()
    else:
        diagnose()
        configure(dry_run=args.dry_run, route_only=args.route_only)


if __name__ == "__main__":
    main()
