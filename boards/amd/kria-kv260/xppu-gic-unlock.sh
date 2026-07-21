#!/bin/bash
# xppu-gic-unlock.sh — Grant RPU0 write access to the RPU GIC (0xF9000000)
#
# See boot-xmpu-fix-plan.md (same directory) for the full diagnostic flow.
# (The older ldp-xppu-gic-fix.md this script originally referenced has been
# removed/superseded — do not go looking for it.)
#
# ⚠️ 2026-07-20 reassessment: run boot-xmpu-fix-plan.md's "Part 0" checks
# (GIC secure/non-secure state) BEFORE this script. This board's own
# Vivado-generated psu_lpd_xppu_data() enumerates every LPD_XPPU aperture by
# name/address, and none of them are anywhere near 0xF9000000 — they're all
# 0xFF...-prefixed peripherals (UART, IPI, TTC, GPIO, ...). So the scan below
# most likely will NOT find an aperture "covering" 0xF9000000, because that
# address probably isn't LPD_XPPU-gated at all. An empty scan is a real
# result, not a sign to keep guessing at aperture numbers.
#
# Run as root (or sudo) on the KV260 Linux console BEFORE loading the R5F firmware.
#
# Usage:
#   sudo bash xppu-gic-unlock.sh             # interactive: scans, then prompts
#   sudo bash xppu-gic-unlock.sh <aperture>  # non-interactive: apply aperture N directly

set -e

LPD_XPPU_APERT_BASE=$(( 0xFF980000 + 0x100 ))

# Permission bits (UG1085 §16.8)
XPPU_APU_BIT=$(( 1 << 0 ))   # Cortex-A53
XPPU_RPU0_BIT=$(( 1 << 2 ))  # R5F-0
XPPU_ENABLE_BIT=$(( 1 << 19 ))

GICD_CTLR=0xF9000000
GICD_IIDR=0xF9000008
GICD_ISENABLER2=0xF9000108  # covers SPI 33-63 (bits [31:0] = SPI[63:32])
GICC_PMR=0xF9001004

dm() { sudo devmem "$@" 2>/dev/null; }

echo "=== Step 1: LPD_XPPU Aperture Scan ==="
echo "Scanning apertures 0-79 for non-zero values..."
echo ""
printf "%-14s %-12s %s\n" "Aperture[N]" "Address" "Value"
printf "%-14s %-12s %s\n" "-----------" "-------" "-----"

for i in $(seq 0 79); do
    addr=$(printf "0x%08X" $(( LPD_XPPU_APERT_BASE + i * 4 )))
    val=$(dm "$addr" 32)
    if [ "$val" != "0x00000000" ]; then
        printf "aperture[%02d]   %s   %s\n" "$i" "$addr" "$val"
    fi
done

echo ""
echo "=== Current RPU GIC registers (expect 0 while XPPU blocks writes) ==="
printf "  GICD_CTLR  0x%08X : %s\n" "$GICD_CTLR" "$(dm $GICD_CTLR 32)"
printf "  GICD_IIDR  0x%08X : %s\n" "$GICD_IIDR"  "$(dm $GICD_IIDR 32)"
printf "  GICC_PMR   0x%08X : %s\n" "$GICC_PMR"   "$(dm $GICC_PMR 32)"
echo ""

# Determine aperture to patch
if [ -n "$1" ]; then
    APERTURE_N="$1"
    echo "Using command-line aperture: $APERTURE_N"
else
    echo "Cross-reference the scan with UG1085 Table 2-3 to find which aperture"
    echo "covers 0xF9000000 (RPU Private Bus / PL390 GIC distributor)."
    echo ""
    echo "NOTE: if the scan above showed no aperture anywhere near 0xF9000000,"
    echo "that is expected (see the 2026-07-20 note at the top of this script) —"
    echo "picking an unrelated aperture number here will not fix anything."
    echo ""
    read -rp "Enter aperture number to patch (or Ctrl-C to abort): " APERTURE_N
fi

# Validate
if ! [[ "$APERTURE_N" =~ ^[0-9]+$ ]] || [ "$APERTURE_N" -gt 79 ]; then
    echo "ERROR: aperture must be 0-79" >&2
    exit 1
fi

APERT_ADDR=$(printf "0x%08X" $(( LPD_XPPU_APERT_BASE + APERTURE_N * 4 )))
CURRENT_HEX=$(dm "$APERT_ADDR" 32)
CURRENT=$(( CURRENT_HEX ))

echo ""
echo "=== Step 3: Apply XPPU fix ==="
printf "aperture[%d] @ %s = %s\n" "$APERTURE_N" "$APERT_ADDR" "$CURRENT_HEX"

NEW=$(( CURRENT | XPPU_RPU0_BIT | XPPU_APU_BIT | XPPU_ENABLE_BIT ))
NEW_HEX=$(printf "0x%08X" "$NEW")
printf "Writing %s (added RPU0 bit 2 + APU bit 0 + enable bit 19)...\n" "$NEW_HEX"
sudo devmem "$APERT_ADDR" 32 "$NEW_HEX"

READBACK=$(dm "$APERT_ADDR" 32)
printf "Readback : %s\n" "$READBACK"

echo ""
echo "=== Step 4: Verify GIC writes ==="
TEST_VAL=0x00000082   # enable SPI 33 (IPI) + SPI 39 (TTC)
printf "Writing %s to GICD_ISENABLER2 (0x%08X)...\n" "$TEST_VAL" "$GICD_ISENABLER2"
sudo devmem "$GICD_ISENABLER2" 32 "$TEST_VAL"
READBACK_GIC=$(dm "$GICD_ISENABLER2" 32)
printf "Readback: %s\n" "$READBACK_GIC"

if [ "$READBACK_GIC" = "$TEST_VAL" ]; then
    echo ""
    echo "SUCCESS: GIC writes are accepted — XPPU aperture unlocked."
    echo ""
    echo "Next steps:"
    echo "  1. Reload R5F firmware:"
    echo "       echo stop | sudo tee /sys/class/remoteproc/remoteproc0/state"
    echo "       echo start | sudo tee /sys/class/remoteproc/remoteproc0/state"
    echo "  2. Verify TTC ISR running:  sudo devmem 0x1ED3FFDC 32  (expect >0)"
    echo "  3. Verify IPI ISR firing:   sudo devmem 0x1ED3FFE8 32  (expect >0)"
    echo "  4. Run ping test:           sudo python3 /tmp/ping_test.py"
else
    echo ""
    echo "FAILED: GIC still rejected the write."
    echo "  - Check that aperture $APERTURE_N actually covers 0xF9000000."
    echo "  - Re-run the scan and try a different aperture."
    exit 1
fi
