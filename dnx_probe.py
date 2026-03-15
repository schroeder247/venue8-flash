#!/usr/bin/env python3
"""Minimal DnX device probe — figure out what the device expects."""

import usb.core
import usb.util
import usb.backend.libusb1
import struct
import time
import sys

INTEL_VID = 0x8086
MERRIFIELD_PID = 0xe005
PREAMBLE_DNER = struct.pack('<I', 0x52456E44)

# Check which backend pyusb is using
be = usb.backend.libusb1.get_backend()
if be:
    print(f"[+] Using libusb-1.0 backend")
else:
    print("[!] libusb-1.0 backend NOT available, using default")
    be = None

print("[*] Waiting for Merrifield device...")
dev = None
for i in range(120):
    dev = usb.core.find(idVendor=INTEL_VID, idProduct=MERRIFIELD_PID, backend=be)
    if dev:
        break
    time.sleep(0.5)

if not dev:
    print("[!] Device not found")
    sys.exit(1)

print(f"[+] Found: Bus {dev.bus} Device {dev.address}")
try:
    print(f"    Manufacturer: {dev.manufacturer}")
    print(f"    Product: {dev.product}")
    print(f"    Serial: {dev.serial_number}")
except Exception as e:
    print(f"    (Could not read strings: {e})")

print(f"    bNumConfigurations: {dev.bNumConfigurations}")
print(f"    bDeviceClass: {dev.bDeviceClass}")
print(f"    bDeviceSubClass: {dev.bDeviceSubClass}")
print(f"    bDeviceProtocol: {dev.bDeviceProtocol}")
print(f"    Speed: {dev.speed}")

# Print all configurations and interfaces
for cfg in dev:
    print(f"\n    Config {cfg.bConfigurationValue}:")
    for intf in cfg:
        print(f"      Interface {intf.bInterfaceNumber} alt {intf.bAlternateSetting}: "
              f"class={intf.bInterfaceClass} subclass={intf.bInterfaceSubClass} "
              f"protocol={intf.bInterfaceProtocol} numEP={intf.bNumEndpoints}")
        for ep in intf:
            direction = "IN" if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN else "OUT"
            ep_type = {0: "CTRL", 1: "ISO", 2: "BULK", 3: "INTR"}[ep.bmAttributes & 0x3]
            print(f"        EP 0x{ep.bEndpointAddress:02x} {direction} {ep_type} maxpkt={ep.wMaxPacketSize}")

# Detach kernel drivers
for cfg in dev:
    for intf in cfg:
        ifnum = intf.bInterfaceNumber
        try:
            if dev.is_kernel_driver_active(ifnum):
                dev.detach_kernel_driver(ifnum)
                print(f"\n[+] Detached kernel driver from interface {ifnum}")
        except usb.core.USBError as e:
            print(f"    (detach interface {ifnum}: {e})")

# ============================================================
# TEST A: WITHOUT set_configuration (like xFSTK on Linux)
# ============================================================
print("\n" + "="*60)
print("[TEST A] Without set_configuration (xFSTK Linux behavior)")
print("="*60)

cfg = dev.get_active_configuration()
intf = cfg[(0, 0)]

ep_out = usb.util.find_descriptor(
    intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
ep_in = usb.util.find_descriptor(
    intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

print(f"  EP OUT=0x{ep_out.bEndpointAddress:02x} IN=0x{ep_in.bEndpointAddress:02x}")

# Claim interface
try:
    usb.util.claim_interface(dev, intf.bInterfaceNumber)
    print(f"  [+] Claimed interface {intf.bInterfaceNumber}")
except usb.core.USBError as e:
    print(f"  [!] Claim: {e}")

# Test A1: Write DnER
print("\n  [A1] Write DnER (10s timeout)...")
try:
    written = ep_out.write(PREAMBLE_DNER, timeout=10000)
    print(f"  [+] Write OK! ({written} bytes)")
    # Read response
    try:
        data = ep_in.read(512, timeout=5000)
        print(f"  [+] Response: {bytes(data).hex()} = '{bytes(data).decode('ascii', errors='replace')}'")
    except usb.core.USBError as e:
        print(f"  [-] Read response: {e}")
except usb.core.USBError as e:
    print(f"  [-] Write failed: {e}")

    # Test A2: Read first, then write
    print("\n  [A2] Read first (5s timeout)...")
    try:
        data = ep_in.read(512, timeout=5000)
        print(f"  [+] Got: {bytes(data).hex()}")
    except usb.core.USBError as e:
        print(f"  [-] Read: {e}")

# Release
try:
    usb.util.release_interface(dev, intf.bInterfaceNumber)
except:
    pass

# ============================================================
# TEST B: WITH set_configuration
# ============================================================
print("\n" + "="*60)
print("[TEST B] With set_configuration")
print("="*60)

try:
    dev.set_configuration()
    print("  [+] set_configuration OK")
except usb.core.USBError as e:
    print(f"  [!] set_configuration: {e}")

cfg = dev.get_active_configuration()
intf = cfg[(0, 0)]
ep_out = usb.util.find_descriptor(
    intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
ep_in = usb.util.find_descriptor(
    intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

try:
    usb.util.claim_interface(dev, intf.bInterfaceNumber)
    print(f"  [+] Claimed interface")
except usb.core.USBError as e:
    print(f"  [!] Claim: {e}")

print("\n  [B1] Write DnER (10s timeout)...")
try:
    written = ep_out.write(PREAMBLE_DNER, timeout=10000)
    print(f"  [+] Write OK! ({written} bytes)")
    try:
        data = ep_in.read(512, timeout=5000)
        print(f"  [+] Response: {bytes(data).hex()} = '{bytes(data).decode('ascii', errors='replace')}'")
    except usb.core.USBError as e:
        print(f"  [-] Read response: {e}")
except usb.core.USBError as e:
    print(f"  [-] Write failed: {e}")

# ============================================================
# TEST C: Raw libusb via ctypes (bypass pyusb entirely)
# ============================================================
print("\n" + "="*60)
print("[TEST C] Direct libusb-1.0 bulk transfer")
print("="*60)

try:
    import ctypes
    import ctypes.util

    libusb = ctypes.cdll.LoadLibrary(ctypes.util.find_library('usb-1.0') or 'libusb-1.0.so')
    print(f"  [+] Loaded libusb-1.0 directly")

    # We'll use the pyusb device handle but do raw transfers
    # Access the internal handle
    dev_handle = dev._ctx.handle
    print(f"  Device handle: {dev_handle}")

    # Try bulk transfer
    transferred = ctypes.c_int(0)
    buf = (ctypes.c_ubyte * 4)(*PREAMBLE_DNER)

    print("  [C1] libusb_bulk_transfer OUT EP 0x01 (10s)...")
    ret = libusb.libusb_bulk_transfer(
        dev_handle,
        ctypes.c_ubyte(0x01),  # OUT endpoint
        buf,
        4,
        ctypes.byref(transferred),
        10000  # timeout
    )
    print(f"  Result: ret={ret}, transferred={transferred.value}")
except Exception as e:
    print(f"  [-] Direct libusb test failed: {e}")

print("\n[*] Probe complete. Power off tablet now.")
