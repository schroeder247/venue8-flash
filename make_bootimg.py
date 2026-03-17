#!/usr/bin/env python3
"""
Create a Dell Venue 8 3840 boot.img in OSIP + stitch.py format.

Layout (matching Dell factory format):
  [OSIP header: 0x4D8 bytes]
  [cmdline: 1024 bytes]
  [size fields: 3072 bytes]
  [bootstub: 8192 bytes]
  [kernel: page-aligned]
  [ramdisk: page-aligned]

The OSIP header is reused from osip_header.bin (first 0x4D8 bytes),
with the OSII image size field patched.
"""

import struct
import sys
import os

PAGE_SIZE = 4096

OSIP_PREFIX_SIZE = 0x4D8
CMDLINE_SIZE = 1024
SIZES_BLOCK_SIZE = 3072
BOOTSTUB_SIZE = 8192

# OSII descriptor layout at offset 0x20:
#   0x20: os_rev_minor (u16)
#   0x22: os_rev_major (u16)
#   0x24: logical_start_block (u32)
#   0x28: ddr_load_address (u32) = 0x01100000
#   0x2C: entry_point (u32) = 0x01101000
#   0x30: size_of_os_image (u32, in 512-byte blocks)
OSII_SIZE_OFFSET = 0x30


def page_align(size):
    """Round up to next PAGE_SIZE boundary."""
    if size % PAGE_SIZE != 0:
        return size + (PAGE_SIZE - size % PAGE_SIZE)
    return size


def main():
    if len(sys.argv) != 6:
        print(f"Usage: {sys.argv[0]} osip_header bootstub bzImage ramdisk output")
        print()
        print("  osip_header: existing osip_header.bin (first 0x4D8 bytes used)")
        print("  bootstub:    bootstub binary (max 8192 bytes)")
        print("  bzImage:     kernel bzImage")
        print("  ramdisk:     ramdisk.gz (gzip-compressed cpio)")
        print("  output:      output boot.img")
        sys.exit(1)

    osip_path, bootstub_path, kernel_path, ramdisk_path, output_path = sys.argv[1:6]

    # Read inputs
    with open(osip_path, "rb") as f:
        osip_data = f.read()
    with open(bootstub_path, "rb") as f:
        bootstub_data = f.read()
    with open(kernel_path, "rb") as f:
        kernel_data = f.read()
    with open(ramdisk_path, "rb") as f:
        ramdisk_data = f.read()

    kernel_sz = page_align(len(kernel_data))
    ramdisk_sz = page_align(len(ramdisk_data))

    print(f"Kernel:  {len(kernel_data):>10} bytes (padded to {kernel_sz})")
    print(f"Ramdisk: {len(ramdisk_data):>10} bytes (padded to {ramdisk_sz})")
    print(f"Bootstub: {len(bootstub_data):>10} bytes")

    assert len(bootstub_data) <= BOOTSTUB_SIZE, \
        f"Bootstub too large: {len(bootstub_data)} > {BOOTSTUB_SIZE}"

    # Cmdline for kernel 6.6 / Android 15
    cmdline = (
        b"init=/init pci=noearly console=ttyMFD2,115200n8 "
        b"loglevel=7 vmalloc=256M "
        b"androidboot.hardware=saltbay "
        b"watchdog.watchdog_thresh=60 "
        b"androidboot.serialno=01234567890123456789012345678901 "
        b"snd_pcm.maximum_substreams=8 "
        b"earlyprintk=hsu2 "
        b"keep_bootcon "
        b"drm.debug=0x0e "
    )

    assert len(cmdline) <= CMDLINE_SIZE - 1, \
        f"Cmdline too long: {len(cmdline)} > {CMDLINE_SIZE - 1}"

    # Build the size fields block (matches stitch.py format)
    console_suppression = 0
    console_dev_type = 0xFF
    reserved_flag_0 = 0x02BD02BD
    reserved_flag_1 = 0x12BD12BD

    sizes = struct.pack("<IIIIII",
                        kernel_sz,
                        ramdisk_sz,
                        console_suppression,
                        console_dev_type,
                        reserved_flag_0,
                        reserved_flag_1)

    # Build output image
    with open(output_path, "wb") as out:
        # 1. OSIP header (first 0x4D8 bytes from template)
        osip_header = bytearray(osip_data[:OSIP_PREFIX_SIZE])

        # Patch OSII size_of_os_image (in 512-byte blocks)
        # Total image payload = cmdline + sizes + bootstub + kernel + ramdisk
        payload_size = CMDLINE_SIZE + SIZES_BLOCK_SIZE + BOOTSTUB_SIZE + kernel_sz + ramdisk_sz
        total_size = OSIP_PREFIX_SIZE + payload_size
        image_blocks = (total_size + 511) // 512
        struct.pack_into("<I", osip_header, OSII_SIZE_OFFSET, image_blocks)
        print(f"OSII size: {image_blocks} blocks ({image_blocks * 512} bytes)")

        # Recalculate OSIP header checksum (XOR of first 56 bytes with
        # checksum byte zeroed). Required by IAFW and droidboot validation.
        osip_header[7] = 0  # zero checksum field before calculation
        xor = 0
        for b in osip_header[:56]:
            xor ^= b
        osip_header[7] = xor
        print(f"OSIP checksum: 0x{xor:02X}")

        out.write(bytes(osip_header))

        # 2. Cmdline (1024 bytes, null-padded)
        out.write(cmdline)
        out.write(b'\0' * (CMDLINE_SIZE - len(cmdline)))

        # 3. Size fields (3072 bytes, zero-padded)
        out.write(sizes)
        out.write(b'\0' * (SIZES_BLOCK_SIZE - len(sizes)))

        # 4. Bootstub (8192 bytes, zero-padded)
        out.write(bootstub_data)
        out.write(b'\0' * (BOOTSTUB_SIZE - len(bootstub_data)))

        # 5. Kernel (page-aligned)
        out.write(kernel_data)
        out.write(b'\0' * (kernel_sz - len(kernel_data)))

        # 6. Ramdisk (page-aligned)
        out.write(ramdisk_data)
        out.write(b'\0' * (ramdisk_sz - len(ramdisk_data)))

    final_size = os.path.getsize(output_path)
    print(f"Output:  {final_size:>10} bytes -> {output_path}")
    print("Done!")


if __name__ == "__main__":
    main()
