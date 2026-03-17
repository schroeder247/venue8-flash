#!/usr/bin/env python3
"""
Dell Venue 8 3840 — Full DnX flash: IFWI + droidboot → fastboot → kernel

Based on xFSTK protocol analysis (mrfdldrstate.cpp). Key differences from
the original dnx_flash.py:
  - FW and OS phases run in ONE continuous USB session (no re-enumeration)
  - DORM (not HLT$) triggers transition to OS phase
  - OS image is droidboot.img.POS.bin (loads fastboot into RAM)
  - After droidboot loads, uses fastboot to flash the actual kernel

Usage:
    python3 dnx_full_flash.py [--kernel boot-6.6-new.img]

The device must be in DnX mode (USB 8086:e005).
"""

import usb.core
import usb.util
import struct
import time
import sys
import os
import subprocess

INTEL_VID = 0x8086
MERRIFIELD_PID = 0xe005

ONE28_K = 131072
CDPH_HEADER_SIZE = 512
SIXTEEN_KB = 16384
SEVENTY_TWO_KB = 73728

PREAMBLE_DNER = 0x52456E44

READ_TIMEOUT = 5000
WRITE_TIMEOUT = 10000

BASE = os.path.dirname(os.path.abspath(__file__))


def log(msg):
    print(msg, flush=True)


# ── Protocol helpers (from dnx_flash.py) ──────────────────────

def build_dnx_header(file_size, gpflags):
    checksum = file_size ^ gpflags
    return struct.pack('<IIIIII', file_size, gpflags, 0, 0, 0, checksum)


def find_chaabi_markers(dnx_data):
    ch00_pos = dnx_data.find(b'CH00')
    cdph_pos = dnx_data.find(b'CDPH')
    cht_pos = dnx_data.find(b'$CHT')
    dtkn_pos = dnx_data.find(b'DTKN')
    chpr_pos = dnx_data.find(b'ChPr')
    if ch00_pos < 0:
        return None
    ch00_start = max(0, ch00_pos - 0x80)
    token_size = 0
    if dtkn_pos >= 0 and dtkn_pos < ch00_start:
        search_start = dtkn_pos + SIXTEEN_KB
        if search_start < len(dnx_data):
            ch00_pos2 = dnx_data.find(b'CH00', search_start)
            if ch00_pos2 >= 0:
                ch00_start = ch00_pos2 - 0x80
                token_size = ch00_start - dtkn_pos
            else:
                return None
        else:
            return None
    elif cht_pos >= 0:
        token_size = ch00_start - (cht_pos - 0x80)
    elif chpr_pos >= 0:
        token_size = ch00_start - chpr_pos
    else:
        return None
    fw_size = (cdph_pos - ch00_start) if cdph_pos >= 0 else 0
    return (token_size, fw_size) if token_size > 0 and fw_size > 0 else None


def prepare_dnx_fw(dnx_data):
    file_size = len(dnx_data)
    markers = find_chaabi_markers(dnx_data)
    if markers:
        token_size, fw_size = markers
        log(f"    Chaabi: token={token_size}, fw={fw_size}")
    else:
        token_size, fw_size = SIXTEEN_KB, SEVENTY_TWO_KB
        log(f"    Chaabi defaults: token={token_size}, fw={fw_size}")
    chaabi_total = token_size + fw_size
    if file_size < chaabi_total + CDPH_HEADER_SIZE:
        return dnx_data, b''
    cdph = dnx_data[file_size - CDPH_HEADER_SIZE:]
    main_size = file_size - chaabi_total - CDPH_HEADER_SIZE
    dxbl_data = cdph + dnx_data[:main_size]
    chaabi_start = file_size - chaabi_total - CDPH_HEADER_SIZE
    chfi00_data = cdph + dnx_data[chaabi_start:chaabi_start + chaabi_total]
    log(f"    DXBL={len(dxbl_data)}B  CHFI00={len(chfi00_data)}B")
    return dxbl_data, chfi00_data


def find_fuph_in_ifwi(ifwi_data):
    file_size = len(ifwi_data)
    footer_size = 0
    cfs_pos = ifwi_data.rfind(b'$CFS')
    fuph_pos = ifwi_data.rfind(b'UPH$')
    if cfs_pos >= 0 and fuph_pos >= 0 and cfs_pos > fuph_pos:
        footer_size = file_size - (cfs_pos - 4)
    search_start = max(0, file_size - 204800 - footer_size)
    uph_offset = ifwi_data.find(b'UPH$', search_start)
    if uph_offset < 0:
        return b'', 0, file_size
    fuph_data = ifwi_data[uph_offset:]
    fuph_size = len(fuph_data)
    ifwi_chunk_size = file_size if footer_size else uph_offset
    return fuph_data, fuph_size, ifwi_chunk_size


# ── USB helpers ───────────────────────────────────────────────

def setup_device(dev):
    for cfg in dev:
        for intf in cfg:
            try:
                if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                    dev.detach_kernel_driver(intf.bInterfaceNumber)
            except usb.core.USBError:
                pass
    cfg = dev.get_active_configuration()
    intf = cfg[(0, 0)]
    ep_out = usb.util.find_descriptor(
        intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
    ep_in = usb.util.find_descriptor(
        intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
    if not ep_out or not ep_in:
        log("[!] No bulk endpoints")
        return None, None
    log(f"[+] EP OUT=0x{ep_out.bEndpointAddress:02x} IN=0x{ep_in.bEndpointAddress:02x}")
    try:
        usb.util.claim_interface(dev, intf.bInterfaceNumber)
    except usb.core.USBError as e:
        log(f"[!] Claim: {e}")
    return ep_out, ep_in


def usb_write(ep, data, desc=""):
    try:
        ep.write(data, timeout=WRITE_TIMEOUT)
        return True
    except usb.core.USBError as e:
        log(f"[!] Write fail ({desc}): {e}")
        return False


def usb_read(ep, timeout_ms=None):
    t = timeout_ms or READ_TIMEOUT
    try:
        raw = bytes(ep.read(512, timeout=t))
        return raw
    except usb.core.USBError:
        return None


def match_opcode(data):
    if not data or len(data) < 4:
        return None
    # Check multi-byte opcodes first
    if len(data) >= 7 and data[:7] == b'OSIP Sz': return 'OSIPSZ'
    if len(data) >= 6 and data[:6] == b'DCFI00': return 'DCFI00'
    for n, ops in [(5, [b'RUPHS', b'RESET', b'DIFWI', b'ROSIP', b'DCSDB', b'UCSDB']),
                   (4, [b'DFRM', b'DxxM', b'DXBL', b'RUPH', b'HLT$', b'HLT0',
                        b'MFLD', b'CLVT', b'ER00', b'DORM', b'DONE', b'RIMG', b'EOIU'])]:
        for op in ops:
            if data[:n] == op:
                return op.decode('ascii')
    op4 = data[:4]
    if op4[:2] == b'ER':
        return op4.decode('ascii', errors='replace')
    return None


# ── Combined FW + OS flash (single session) ──────────────────

def flash_all(ep_out, ep_in, fwdnx, ifwi, osdnx, osimg, gpflags):
    """Full DnX protocol: IFWI firmware + OS image in one USB session."""

    # Prepare FW data
    log("[*] Preparing FW DnX...")
    fw_dxbl, fw_chfi00 = prepare_dnx_fw(fwdnx)
    fuph_data, fuph_size, ifwi_data_size = find_fuph_in_ifwi(ifwi)
    log(f"    FUPH: {fuph_size}B at end of IFWI")

    # Prepare OS data
    log("[*] Preparing OS DnX...")
    os_dxbl, os_chfi00 = prepare_dnx_fw(osdnx)
    osip_data = osimg[:512]

    # IFWI chunking
    ifwi_chunk_data = ifwi[:ifwi_data_size]
    num_chunks = ifwi_data_size // ONE28_K
    residual = ifwi_data_size - (num_chunks * ONE28_K)
    total_chunks = num_chunks + (1 if residual else 0)
    log(f"    IFWI: {ifwi_data_size}B = {total_chunks} chunks")

    # Build DnX header
    dnx_hdr = build_dnx_header(len(fwdnx), gpflags)
    os_dnx_hdr = build_dnx_header(len(osdnx), gpflags)

    # Send DnER preamble
    preamble = struct.pack('<I', PREAMBLE_DNER)
    log("[*] Sending DnER...")
    if not usb_write(ep_out, preamble, "DnER"):
        return False

    chunk_idx = 0
    pass_num = 1
    phase = 'fw'  # 'fw' or 'os'
    img_offset = 0

    for iteration in range(50000):
        timeout = 30000 if (phase == 'fw' and chunk_idx >= total_chunks) else READ_TIMEOUT
        resp = usb_read(ep_in, timeout)
        if resp is None:
            if phase == 'fw' and chunk_idx >= total_chunks:
                log("[*] Waiting for device (flash programming)...")
                resp = usb_read(ep_in, 60000)
            if resp is None:
                log("[!] No response"); return False

        opname = match_opcode(resp)
        if opname is None:
            continue

        # ── FW phase opcodes ──
        if opname in ('DFRM', 'DxxM'):
            log(f"[*] {opname} — sending DnX header")
            usb_write(ep_out, dnx_hdr, "DnX hdr")

        elif opname == 'ER00':
            usb_write(ep_out, preamble, "DnER retry")

        elif opname in ('MFLD', 'CLVT'):
            usb_write(ep_out, preamble, "DnER")

        elif opname == 'DXBL' and phase == 'fw':
            log(f"[*] FW DXBL ({len(fw_dxbl)}B)")
            usb_write(ep_out, fw_dxbl, "FW DXBL")

        elif opname == 'DCFI00' and phase == 'fw':
            log(f"[*] FW DCFI00 ({len(fw_chfi00)}B)")
            usb_write(ep_out, fw_chfi00, "FW CHFI00")

        elif opname == 'RUPHS':
            usb_write(ep_out, struct.pack('<I', fuph_size), "FUPH size")

        elif opname == 'RUPH':
            log(f"[*] RUPH ({fuph_size}B)")
            usb_write(ep_out, fuph_data, "FUPH")

        elif opname == 'DIFWI':
            if chunk_idx >= total_chunks:
                pass_num += 1
                chunk_idx = 0
                log(f"[*] IFWI pass {pass_num}")
            offset = chunk_idx * ONE28_K
            if residual and chunk_idx == total_chunks - 1:
                chunk = ifwi_chunk_data[offset:offset + residual]
                chunk += b'\x00' * (ONE28_K - len(chunk))
            else:
                chunk = ifwi_chunk_data[offset:offset + ONE28_K]
            ep_out.write(chunk, timeout=WRITE_TIMEOUT)
            chunk_idx += 1
            pct = (chunk_idx * 100) // total_chunks
            if pct % 10 == 0 or chunk_idx == total_chunks:
                log(f"    IFWI: {pct}% ({chunk_idx}/{total_chunks}) pass {pass_num}")

        elif opname in ('DCSDB', 'UCSDB'):
            usb_write(ep_out, b'\x00' * 4, opname)

        elif opname == 'HLT$':
            log("[+] HLT$ — IFWI flash complete (no OS phase requested by device)")
            log("[*] Device will reboot. Wait for fastboot/droidboot.")
            return 'fw_only'

        elif opname == 'HLT0':
            log("[!] HLT0 — error"); return False

        # ── DORM: transition to OS phase (same session!) ──
        elif opname == 'DORM':
            log("[+] DORM — device ready for OS phase!")
            log("[*] === OS PHASE (same USB session) ===")
            phase = 'os'
            usb_write(ep_out, os_dnx_hdr, "OS DnX hdr")

        # ── OS phase opcodes ──
        elif opname == 'DXBL' and phase == 'os':
            log(f"[*] OS DXBL ({len(os_dxbl)}B)")
            usb_write(ep_out, os_dxbl, "OS DXBL")

        elif opname == 'DCFI00' and phase == 'os':
            if os_chfi00:
                log(f"[*] OS DCFI00 ({len(os_chfi00)}B)")
                usb_write(ep_out, os_chfi00, "OS CHFI00")

        elif opname == 'OSIPSZ':
            usb_write(ep_out, struct.pack('<I', 1), "OSIP count")

        elif opname == 'ROSIP':
            log(f"[*] Sending OSIP header (512B)")
            usb_write(ep_out, osip_data, "OSIP")

        elif opname == 'RIMG':
            sz = min(ONE28_K, len(osimg) - img_offset)
            if sz > 0:
                try:
                    ep_out.write(osimg[img_offset:img_offset + sz], timeout=WRITE_TIMEOUT)
                except usb.core.USBError as e:
                    log(f"[!] OS write error: {e}"); return False
                img_offset += sz
                pct = (img_offset * 100) // len(osimg)
                if pct % 10 == 0 or img_offset >= len(osimg):
                    log(f"    OS: {pct}% ({img_offset}/{len(osimg)})")

        elif opname == 'EOIU':
            log("[+] EOIU — OS flash complete!")
            # Send DFN acknowledgment
            usb_write(ep_out, b'DFN\x00', "DFN ack")
            return 'os_done'

        elif opname == 'DONE':
            log("[+] DONE")
            return 'os_done'

        elif opname == 'RESET':
            log("[+] RESET"); return 'os_done'

        elif opname and opname.startswith('ER'):
            log(f"[!] Error: {opname}"); return False

    log("[!] Protocol loop exhausted")
    return False


# ── Main ──────────────────────────────────────────────────────

def main():
    import argparse
    parser = argparse.ArgumentParser(description='Dell Venue 8 DnX + fastboot flash')
    parser.add_argument('--kernel', default=os.path.join(BASE, 'boot-6.6-new.img'),
                        help='Kernel boot.img to flash via fastboot (default: boot-6.6-new.img)')
    parser.add_argument('--fw-only', action='store_true',
                        help='Flash IFWI only, skip OS/droidboot')
    args = parser.parse_args()

    # Load files
    fwdnx_path = os.path.join(BASE, 'fwr_dnx_PRQ_ww27_001.bin')
    ifwi_path = os.path.join(BASE, 'IFWI_MERR_PRQ_UOS_TH2_YT2_ww27_001.bin')
    osdnx_path = os.path.join(BASE, 'osr_dnx_PRQ_ww27_001.bin')
    osimg_path = os.path.join(BASE, 'droidboot.img.POS.bin')

    with open(fwdnx_path, 'rb') as f: fwdnx = f.read()
    with open(ifwi_path, 'rb') as f: ifwi = f.read()
    with open(osdnx_path, 'rb') as f: osdnx = f.read()
    with open(osimg_path, 'rb') as f: osimg = f.read()

    gpflags = 0x80000007
    if args.fw_only:
        gpflags = 0x80000000  # FW only, no OS phase
    log(f"[*] GP flags: 0x{gpflags:08X}")
    log(f"[*] Files: fwdnx={len(fwdnx)} ifwi={len(ifwi)} osdnx={len(osdnx)} osimg={len(osimg)}")
    log(f"[*] Kernel: {args.kernel}")

    # Find device
    log("[*] Looking for DnX device (8086:e005)...")
    dev = None
    for i in range(120):
        dev = usb.core.find(idVendor=INTEL_VID, idProduct=MERRIFIELD_PID)
        if dev:
            break
        if i % 10 == 0 and i > 0:
            log(f"    waiting... ({i}s)")
        time.sleep(1)
    if not dev:
        log("[!] No DnX device found"); sys.exit(1)

    log(f"[+] Device: Bus {dev.bus} Dev {dev.address}")
    ep_out, ep_in = setup_device(dev)
    if not ep_out:
        sys.exit(1)

    # Flash
    result = flash_all(ep_out, ep_in, fwdnx, ifwi, osdnx, osimg, gpflags)

    if result == 'os_done':
        log("[+] DnX complete — droidboot loaded into RAM")
        log("[*] Waiting for fastboot device...")
        time.sleep(5)

        # Wait for fastboot
        for i in range(60):
            r = subprocess.run(['fastboot', 'devices'], capture_output=True, text=True)
            if r.stdout.strip():
                log(f"[+] Fastboot device: {r.stdout.strip()}")
                break
            time.sleep(2)
        else:
            log("[!] Fastboot device not found")
            sys.exit(1)

        # Flash kernel via fastboot
        log(f"[*] Flashing kernel: {args.kernel}")
        r = subprocess.run(['fastboot', 'flash', 'boot', args.kernel],
                           capture_output=True, text=True)
        log(r.stdout)
        if r.returncode != 0:
            log(f"[!] fastboot flash failed: {r.stderr}")
            log("[*] Trying fastboot boot (RAM boot)...")
            r = subprocess.run(['fastboot', 'boot', args.kernel],
                               capture_output=True, text=True)
            log(r.stdout)
            if r.returncode != 0:
                log(f"[!] fastboot boot also failed: {r.stderr}")
                sys.exit(1)

        log("[*] Rebooting...")
        subprocess.run(['fastboot', 'reboot'], capture_output=True)
        log("[+] DONE — device should boot kernel 6.6!")

    elif result == 'fw_only':
        log("[+] IFWI flashed. Device will reboot.")
        log("[*] After reboot, enter fastboot (Vol-Down + Power) and run:")
        log(f"    fastboot flash boot {args.kernel}")

    else:
        log("[!] Flash failed")
        sys.exit(1)


if __name__ == '__main__':
    main()
