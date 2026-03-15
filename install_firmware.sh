#!/bin/bash
# Install Dell Venue 8 3840 firmware blobs to a target filesystem
# Usage: ./install_firmware.sh <target_rootfs>
# e.g. ./install_firmware.sh /mnt/venue8

set -e

TARGET="${1:?Usage: $0 <target_rootfs>}"
SRC="/home/thomas/venue8-flash/extracted/firmware"

echo "Installing firmware to $TARGET/lib/firmware/"

# Create directories
mkdir -p "$TARGET/lib/firmware/brcm"
mkdir -p "$TARGET/lib/firmware/intel"
mkdir -p "$TARGET/lib/firmware/isp"

# WiFi: BCM4335 SDIO
# brcmfmac expects: brcmfmac4335-sdio.bin
cp "$SRC/fw_bcmdhd.bin_4335_b0" "$TARGET/lib/firmware/brcm/brcmfmac4335-sdio.bin"
echo "  WiFi: brcmfmac4335-sdio.bin"

# WiFi calibration (brcmfmac also looks for .txt or .cal)
cp "$SRC/bcmdhd_aob.cal_4335_b0_edge" "$TARGET/lib/firmware/brcm/brcmfmac4335-sdio.cal"
echo "  WiFi cal: brcmfmac4335-sdio.cal"

# Bluetooth: BCM4335 (hciattach/btbcm expects BCM4335B0.hcd)
cp "$SRC/bt/BCM4335B0_002.001.006.0283.0295.hcd" "$TARGET/lib/firmware/brcm/BCM4335B0.hcd"
echo "  Bluetooth: BCM4335B0.hcd"

# Audio: SST DSP firmware for PCI 0x119a (Merrifield)
# sst-acpi driver loads: intel/fw_sst_0f28.bin (Baytrail) but our PCI driver uses: fw_sst_119a.bin
cp "$SRC/fw_sst_119a.bin" "$TARGET/lib/firmware/intel/fw_sst_119a.bin"
echo "  Audio SST: intel/fw_sst_119a.bin"

# Camera: ISP2400 firmware
cp "$SRC/shisp_2400b0_v21.bin" "$TARGET/lib/firmware/shisp_2400b0_v21.bin"
echo "  Camera ISP: shisp_2400b0_v21.bin"

# Camera: ISP acceleration firmware
for f in "$SRC"/isp_acc_*.bin; do
    name=$(basename "$f")
    cp "$f" "$TARGET/lib/firmware/$name"
    echo "  Camera ISP acc: $name"
done

# Modem: XMM 7160 firmware (for modem_control driver)
mkdir -p "$TARGET/lib/firmware/modem"
cp "$SRC/modem/modem.zip" "$TARGET/lib/firmware/modem/modem.zip"
cp "$SRC/modem/modem_nvm.zip" "$TARGET/lib/firmware/modem/modem_nvm.zip"
echo "  Modem: modem/modem.zip + modem_nvm.zip"

echo ""
echo "Firmware installation complete."
echo "Files installed:"
find "$TARGET/lib/firmware" -type f | sort | sed "s|$TARGET||"
