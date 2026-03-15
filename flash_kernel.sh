#!/bin/bash
# Flash kernel 6.6 to Dell Venue 8 3840
#
# Prerequisites:
# 1. Device in droidboot/fastboot mode:
#    adb reboot bootloader
# 2. fastboot binary available
# 3. boot-6.6.img built by make_bootimg.py
#
# This ONLY flashes the kernel. System partition and data are untouched.
# The old recovery image is preserved — if boot fails, hold Vol-Up
# during power-on to enter recovery.

set -e

BOOT_IMG="/home/thomas/venue8-flash/boot-6.6.img"

if [ ! -f "$BOOT_IMG" ]; then
    echo "ERROR: $BOOT_IMG not found. Run make_bootimg.py first."
    exit 1
fi

echo "=== Dell Venue 8 3840 Kernel Flash ==="
echo ""
echo "Boot image: $BOOT_IMG ($(stat -c %s "$BOOT_IMG") bytes)"
echo ""

# Check fastboot connection
if ! fastboot devices | grep -q .; then
    echo "ERROR: No device in fastboot mode."
    echo ""
    echo "To enter fastboot/droidboot:"
    echo "  1. From ADB:  adb reboot bootloader"
    echo "  2. Manual:    Power off, then hold Vol-Down + Power"
    echo ""
    exit 1
fi

echo "Device found:"
fastboot devices
echo ""

echo "WARNING: This will flash a new kernel to the boot partition."
echo "The existing recovery image is NOT touched."
echo "If boot fails, hold Vol-Up during power-on for recovery."
echo ""
read -p "Continue? [y/N] " confirm
if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
    echo "Aborted."
    exit 0
fi

echo ""
echo "Flashing boot.img..."
fastboot flash boot "$BOOT_IMG"

echo ""
echo "Flash complete. Rebooting..."
fastboot reboot

echo ""
echo "=== Done ==="
echo "Watch serial console (ttyMFD2 115200n8) or ADB for boot output."
echo "Kernel modules still need to be installed on the rootfs."
