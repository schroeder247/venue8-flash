#!/usr/bin/env python3
"""
DnX USB Reconnect Helper for Intel Merrifield devices.

Monitors USB for the Merrifield device after xFSTK sends stage 1,
detaches any kernel driver, resets the device, and keeps it ready
for xFSTK's reconnect scan.

Run this BEFORE starting xFSTK, in a separate terminal:
    sudo python3 dnx_reconnect_helper.py

Then run xFSTK in another terminal as normal.
"""

import usb.core
import usb.util
import time
import sys

INTEL_VID = 0x8086
MERRIFIELD_PID = 0xe005
INTEL_DNX_VID = 0x8087  # Initial DnX enumeration

def find_merrifield():
    return usb.core.find(idVendor=INTEL_VID, idProduct=MERRIFIELD_PID)

def monitor():
    print("[*] DnX Reconnect Helper - monitoring for Merrifield device...")
    print("[*] Start xFSTK in another terminal now.")
    print()

    last_state = None

    while True:
        dev = find_merrifield()

        if dev and last_state != "found":
            print(f"[+] Merrifield device detected: Bus {dev.bus} Device {dev.address}")

            # Detach kernel driver from all interfaces
            for cfg in dev:
                for intf in cfg:
                    ifnum = intf.bInterfaceNumber
                    try:
                        if dev.is_kernel_driver_active(ifnum):
                            dev.detach_kernel_driver(ifnum)
                            print(f"[+] Detached kernel driver from interface {ifnum}")
                    except usb.core.USBError as e:
                        print(f"[!] Could not detach driver from interface {ifnum}: {e}")

            # Reset device to force clean re-enumeration
            try:
                dev.reset()
                print("[+] USB device reset")
            except usb.core.USBError as e:
                print(f"[!] Reset failed (may be OK): {e}")

            # Wait for device to come back
            time.sleep(2)

            dev = find_merrifield()
            if dev:
                # Detach again after reset
                for cfg in dev:
                    for intf in cfg:
                        ifnum = intf.bInterfaceNumber
                        try:
                            if dev.is_kernel_driver_active(ifnum):
                                dev.detach_kernel_driver(ifnum)
                                print(f"[+] Detached kernel driver from interface {ifnum} (post-reset)")
                        except usb.core.USBError:
                            pass

                # Set configuration
                try:
                    dev.set_configuration()
                    print("[+] Device configured and ready for xFSTK")
                except usb.core.USBError as e:
                    print(f"[!] set_configuration failed: {e}")

                print("[*] Device is ready - xFSTK should reconnect now")
                print("[*] Continuing to monitor...")

            last_state = "found"

        elif not dev and last_state == "found":
            print("[-] Device disconnected, waiting for re-enumeration...")
            last_state = "gone"

        elif not dev and last_state != "gone":
            last_state = "waiting"

        time.sleep(0.5)

if __name__ == "__main__":
    if sys.platform == "linux":
        import os
        if os.geteuid() != 0:
            print("[!] This script needs root. Run with sudo.")
            sys.exit(1)

    try:
        monitor()
    except KeyboardInterrupt:
        print("\n[*] Stopped.")
