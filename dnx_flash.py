#!/usr/bin/env python3
"""
Merrifield DnX Firmware Flasher — watches for device, flashes automatically.

Config from dnx_flash.json. Monitor via dnx_ctl.py (STATUS/TAIL/FULL).
Stays running after flash success or failure. Watches for next device.

Usage:
    sudo python3 dnx_flash.py
"""

import usb.core
import usb.util
import struct
import time
import sys
import os
import json
import socket
import threading
import traceback

SOCK_PATH = '/tmp/dnx_flash.sock'
CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dnx_flash.json')

INTEL_VID = 0x8086
MERRIFIELD_PID = 0xe005

ONE28_K = 131072
CDPH_HEADER_SIZE = 512
SIXTEEN_KB = 16384
SEVENTY_TWO_KB = 73728
TWO_HUNDRED_KB = 204800

PREAMBLE_DNER = 0x52456E44

OP_DFRM  = b'DFRM';    OP_DxxM  = b'DxxM';    OP_DXBL  = b'DXBL'
OP_RUPH  = b'RUPH';    OP_MFLD  = b'MFLD';    OP_CLVT  = b'CLVT'
OP_HLT0  = b'HLT0';    OP_ER00  = b'ER00';    OP_DORM  = b'DORM'
OP_DONE  = b'DONE';    OP_RIMG  = b'RIMG';    OP_EOIU  = b'EOIU'
OP_RUPHS = b'RUPHS';   OP_RESET = b'RESET';   OP_DIFWI = b'DIFWI'
OP_ROSIP = b'ROSIP';   OP_DCSDB = b'DCSDB';   OP_UCSDB = b'UCSDB'
OP_DCFI00 = b'DCFI00'; OP_OSIPSZ = b'OSIP Sz'
OP_HLT_DOLLAR = b'HLT$'

ERROR_OPCODES = {
    b'ER01', b'ER02', b'ER03', b'ER04', b'ER06', b'ER07',
    b'ER10', b'ER11', b'ER12', b'ER13', b'ER15', b'ER16',
    b'ER17', b'ER18', b'ER20', b'ER21', b'ER22', b'ER25', b'ER26',
}

READ_TIMEOUT = 5000
WRITE_TIMEOUT = 10000

DEFAULT_CONFIG = {
    "fwdnx": "fwr_dnx_PRQ_ww27_001.bin",
    "fwimage": "IFWI_MERR_PRQ_UOS_TH2_YT2_ww27_001.bin",
    "osdnx": "",
    "osimage": "",
    "gpflags": "0x80000007",
}


# ── Socket server (background, for monitoring) ────────────────

class StatusServer:
    def __init__(self, path=SOCK_PATH):
        self.path = path
        self.log_lines = []
        self.lock = threading.Lock()
        self.last_read = 0
        self._running = True
        self.state = 'idle'

        if os.path.exists(path):
            os.unlink(path)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        os.chmod(path, 0o666)
        self.sock.listen(2)
        self.sock.settimeout(0.2)
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def log(self, msg):
        print(msg, flush=True)
        with self.lock:
            self.log_lines.append(msg)
            if len(self.log_lines) > 5000:
                self.log_lines = self.log_lines[-5000:]

    def _serve(self):
        while self._running:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                conn.settimeout(1.0)
                raw = conn.recv(4096).decode().strip()
                if not raw: continue
                resp = self._handle(raw)
                conn.sendall((json.dumps(resp) + '\n').encode())
            except:
                pass
            finally:
                try: conn.close()
                except: pass

    def _handle(self, cmd):
        verb = cmd.split()[0].upper()
        parts = cmd.split(None, 1)
        if verb == 'STATUS':
            with self.lock:
                new = self.log_lines[self.last_read:]
                self.last_read = len(self.log_lines)
            return {"lines": new, "state": self.state}
        elif verb == 'FULL':
            with self.lock:
                return {"lines": list(self.log_lines), "state": self.state}
        elif verb == 'TAIL':
            n = int(parts[1]) if len(parts) > 1 else 20
            with self.lock:
                return {"lines": self.log_lines[-n:], "state": self.state}
        elif verb == 'PING':
            return {"pong": True, "state": self.state}
        return {"error": f"unknown: {verb}"}

    def close(self):
        self._running = False
        try: self.sock.close()
        except: pass
        if os.path.exists(self.path):
            os.unlink(self.path)


# ── Protocol helpers ───────────────────────────────────────────

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


def prepare_dnx_fw(dnx_data, log):
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
        log(f"[!] DnX too small for chaabi split")
        return dnx_data, b''
    cdph = dnx_data[file_size - CDPH_HEADER_SIZE:]
    main_size = file_size - chaabi_total - CDPH_HEADER_SIZE
    dxbl_data = cdph + dnx_data[:main_size]
    chaabi_start = file_size - chaabi_total - CDPH_HEADER_SIZE
    chfi00_data = cdph + dnx_data[chaabi_start:chaabi_start + chaabi_total]
    log(f"    DXBL={len(dxbl_data)}B  CHFI00={len(chfi00_data)}B")
    return dxbl_data, chfi00_data


def find_fuph_in_ifwi(ifwi_data, log):
    file_size = len(ifwi_data)
    footer_size = 0
    cfs_pos = ifwi_data.rfind(b'$CFS')
    fuph_pos = ifwi_data.rfind(b'UPH$')
    if cfs_pos >= 0 and fuph_pos >= 0 and cfs_pos > fuph_pos:
        footer_size = file_size - (cfs_pos - 4)
        log(f"    Footer at {cfs_pos}, size={footer_size}")
    search_start = max(0, file_size - TWO_HUNDRED_KB - footer_size)
    uph_offset = ifwi_data.find(b'UPH$', search_start)
    if uph_offset < 0:
        log("[!] FUPH not found")
        return b'', 0, file_size, False
    fuph_data = ifwi_data[uph_offset:]
    fuph_size = len(fuph_data)
    log(f"    FUPH at {uph_offset}: {fuph_size}B")
    ifwi_chunk_size = file_size if footer_size else uph_offset
    return fuph_data, fuph_size, ifwi_chunk_size, footer_size > 0


# ── USB helpers ────────────────────────────────────────────────

def setup_device(dev, log):
    for cfg in dev:
        for intf in cfg:
            try:
                if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                    dev.detach_kernel_driver(intf.bInterfaceNumber)
                    log(f"[+] Detached kernel driver iface {intf.bInterfaceNumber}")
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


def usb_write(ep_out, data, log, desc=""):
    try:
        ep_out.write(data, timeout=WRITE_TIMEOUT)
        if desc:
            hdr = data[:24].hex() if len(data) <= 24 else data[:24].hex() + '...'
            log(f"    -> {desc} ({len(data)}B) [{hdr}]")
        return True
    except usb.core.USBError as e:
        log(f"[!] Write fail ({desc}): {e}")
        return False


def usb_read(ep_in, log, size=512, timeout_ms=None):
    t = timeout_ms or READ_TIMEOUT
    try:
        raw = bytes(ep_in.read(size, timeout=t))
        ascii_rep = raw.decode('ascii', errors='replace')
        log(f"    <- {len(raw)}B [{raw.hex()}] '{ascii_rep}'")
        return raw
    except usb.core.USBError as e:
        log(f"[!] Read t/o ({t}ms): {e}")
        return None


def match_opcode(data):
    if not data or len(data) < 4:
        return None, None
    if len(data) >= 7 and data[:7] == OP_OSIPSZ: return 'OSIPSZ', data[:7]
    if len(data) >= 6 and data[:6] == OP_DCFI00: return 'DCFI00', data[:6]
    if len(data) >= 5:
        for op, name in [(OP_RUPHS,'RUPHS'),(OP_RESET,'RESET'),(OP_DIFWI,'DIFWI'),
                         (OP_ROSIP,'ROSIP'),(OP_DCSDB,'DCSDB'),(OP_UCSDB,'UCSDB')]:
            if data[:5] == op: return name, data[:5]
    op4 = data[:4]
    for op, name in [(OP_DFRM,'DFRM'),(OP_DxxM,'DxxM'),(OP_DXBL,'DXBL'),(OP_RUPH,'RUPH'),
                     (OP_HLT_DOLLAR,'HLT$'),(OP_HLT0,'HLT0'),(OP_MFLD,'MFLD'),(OP_CLVT,'CLVT'),
                     (OP_ER00,'ER00'),(OP_DORM,'DORM'),(OP_DONE,'DONE'),(OP_RIMG,'RIMG'),
                     (OP_EOIU,'EOIU')]:
        if op4 == op: return name, op4
    if op4 in ERROR_OPCODES:
        return op4.decode('ascii'), op4
    return None, None


# ── Flash protocol ─────────────────────────────────────────────

def flash_firmware(ep_out, ep_in, fwdnx_data, ifwi_data, gpflags, log):
    log("[*] === FIRMWARE FLASH ===")
    log("[*] Preparing DnX FW binary...")
    dxbl_data, chfi00_data = prepare_dnx_fw(fwdnx_data, log)

    log("[*] Extracting FUPH from IFWI...")
    fuph_data, fuph_size, ifwi_data_size, has_footer = find_fuph_in_ifwi(ifwi_data, log)

    dnx_hdr = build_dnx_header(len(fwdnx_data), gpflags)
    log(f"[*] DnX hdr: size={len(fwdnx_data)} gp=0x{gpflags:08X} chk=0x{(len(fwdnx_data)^gpflags):08X}")

    ifwi_chunk_data = ifwi_data[:ifwi_data_size]
    num_chunks = ifwi_data_size // ONE28_K
    residual = ifwi_data_size - (num_chunks * ONE28_K)
    total_chunks = num_chunks + (1 if residual else 0)
    log(f"[*] IFWI: {ifwi_data_size}B = {total_chunks} chunks")

    preamble = struct.pack('<I', PREAMBLE_DNER)
    log("[*] Sending DnER preamble...")
    if not usb_write(ep_out, preamble, log, "DnER"):
        return False

    chunk_idx = 0
    fw_done = False
    pass_num = 1

    for iteration in range(5000):
        # Use longer timeout after all chunks sent (device may be programming flash)
        timeout = 30000 if chunk_idx >= total_chunks else READ_TIMEOUT
        resp = usb_read(ep_in, log, 512, timeout_ms=timeout)
        if resp is None:
            if chunk_idx >= total_chunks:
                # After all chunks, one retry with even longer timeout
                log("[*] Waiting for device (programming flash)...")
                resp = usb_read(ep_in, log, 512, timeout_ms=60000)
            if resp is None:
                log("[!] No response from device")
                return False

        opname, _ = match_opcode(resp)
        if opname is None:
            log(f"[?] Unknown: {resp.hex()}")
            continue

        if opname in ('DFRM', 'DxxM'):
            log(f"[*] {opname} — sending DnX header")
            if not usb_write(ep_out, dnx_hdr, log, "DnX hdr"): return False
        elif opname == 'ER00':
            log("[*] ER00 — retry DnER")
            if not usb_write(ep_out, preamble, log, "DnER"): return False
        elif opname in ('MFLD', 'CLVT'):
            log(f"[*] {opname} — resend DnER")
            if not usb_write(ep_out, preamble, log, "DnER"): return False
        elif opname in ERROR_OPCODES or (opname and opname.startswith('ER')):
            log(f"[!] Error: {opname}")
            return False
        elif opname == 'HLT0':
            log("[*] HLT0"); fw_done = True; break
        elif opname == 'HLT$':
            log("[+] HLT$ — SUCCESS"); fw_done = True; break
        elif opname == 'RESET':
            log("[+] RESET"); fw_done = True; break
        elif opname == 'DXBL':
            log(f"[*] DXBL ({len(dxbl_data)}B)")
            if not usb_write(ep_out, dxbl_data, log, "DXBL"): return False
        elif opname == 'RUPHS':
            log(f"[*] RUPHS — size={fuph_size}")
            if not usb_write(ep_out, struct.pack('<I', fuph_size), log, "FUPH size"): return False
        elif opname == 'RUPH':
            log(f"[*] RUPH ({fuph_size}B)")
            if fuph_size > 0:
                if not usb_write(ep_out, fuph_data, log, "FUPH"): return False
        elif opname == 'DCFI00':
            log(f"[*] DCFI00 ({len(chfi00_data)}B)")
            if chfi00_data:
                if not usb_write(ep_out, chfi00_data, log, "CHFI00"): return False
        elif opname in ('DCSDB', 'UCSDB'):
            log(f"[*] {opname} — empty")
            if not usb_write(ep_out, b'\x00' * 4, log, opname): return False
        elif opname == 'DIFWI':
            # Multi-pass: device requests DIFWI again after all chunks — reset
            if chunk_idx >= total_chunks:
                pass_num += 1
                chunk_idx = 0
                log(f"[*] IFWI pass {pass_num} — device requested re-send")

            offset = chunk_idx * ONE28_K
            if residual and chunk_idx == total_chunks - 1:
                chunk = ifwi_chunk_data[offset:offset + residual]
                chunk += b'\x00' * (ONE28_K - len(chunk))
            else:
                chunk = ifwi_chunk_data[offset:offset + ONE28_K]
            try:
                ep_out.write(chunk, timeout=WRITE_TIMEOUT)
            except usb.core.USBError as e:
                log(f"[!] Chunk write: {e}")
                return False
            chunk_idx += 1
            pct = (chunk_idx * 100) // total_chunks
            log(f"    IFWI {pct}% ({chunk_idx}/{total_chunks}) pass {pass_num}")
        elif opname in ('DORM', 'OSIPSZ'):
            log(f"[*] {opname} — OS stage")
            fw_done = True; break
        else:
            log(f"[?] Unhandled: {opname}")

    if fw_done:
        log(f"[+] FW done ({chunk_idx} chunks, {pass_num} pass{'es' if pass_num > 1 else ''})")
    else:
        log("[!] FW incomplete")
    return fw_done


def flash_os(ep_out, ep_in, osdnx_data, osimage_data, gpflags, log):
    log("[*] === OS FLASH ===")
    os_dnx_hdr = build_dnx_header(len(osdnx_data), gpflags)
    os_dxbl, os_chfi00 = prepare_dnx_fw(osdnx_data, log)
    osip_data = osimage_data[:512]
    if not usb_write(ep_out, os_dnx_hdr, log, "OS DnX hdr"): return False
    os_done = False
    img_offset = 0
    for _ in range(50000):
        resp = usb_read(ep_in, log, 512)
        if resp is None: return False
        opname, _ = match_opcode(resp)
        if opname is None: continue
        if opname == 'DXBL':
            if not usb_write(ep_out, os_dxbl, log, "OS DXBL"): return False
        elif opname == 'DCFI00':
            if os_chfi00:
                if not usb_write(ep_out, os_chfi00, log, "OS CHFI00"): return False
        elif opname == 'OSIPSZ':
            if not usb_write(ep_out, struct.pack('<I', 1), log, "OSIP cmd"): return False
        elif opname == 'ROSIP':
            if not usb_write(ep_out, osip_data, log, "OSIP"): return False
        elif opname == 'RIMG':
            sz = min(ONE28_K, len(osimage_data) - img_offset)
            if sz > 0:
                if not usb_write(ep_out, osimage_data[img_offset:img_offset+sz], log): return False
                img_offset += sz
                log(f"    OS {(img_offset*100)//len(osimage_data)}%")
        elif opname == 'DONE':
            os_done = True
        elif opname in ('EOIU', 'HLT$', 'RESET'):
            log(f"[+] {opname}"); os_done = True; break
        elif opname in ERROR_OPCODES or (opname and opname.startswith('ER')):
            log(f"[!] OS error: {opname}"); return False
    return os_done


# ── Main ───────────────────────────────────────────────────────

def load_config(log):
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH) as f:
            cfg = json.load(f)
    else:
        cfg = dict(DEFAULT_CONFIG)
        with open(CONFIG_PATH, 'w') as f:
            json.dump(cfg, f, indent=2)
        log(f"[*] Default config written: {CONFIG_PATH}")
    return cfg


def do_flash(cfg, log):
    base = os.path.dirname(os.path.abspath(CONFIG_PATH))
    gpflags = int(cfg['gpflags'], 16)
    def resolve(p):
        if not p: return None
        return p if os.path.isabs(p) else os.path.join(base, p)

    fwdnx_path = resolve(cfg['fwdnx'])
    fwimage_path = resolve(cfg['fwimage'])
    osdnx_path = resolve(cfg.get('osdnx', ''))
    osimage_path = resolve(cfg.get('osimage', ''))

    log(f"[*] FW DnX: {fwdnx_path}")
    with open(fwdnx_path, 'rb') as f: fwdnx_data = f.read()
    log(f"    {len(fwdnx_data)}B")
    log(f"[*] IFWI: {fwimage_path}")
    with open(fwimage_path, 'rb') as f: ifwi_data = f.read()
    log(f"    {len(ifwi_data)}B")

    os_dnx_data = os_image_data = None
    if osdnx_path and osimage_path:
        with open(osdnx_path, 'rb') as f: os_dnx_data = f.read()
        with open(osimage_path, 'rb') as f: os_image_data = f.read()

    log(f"[*] GP Flags: 0x{gpflags:08X}")

    ep_out, ep_in = setup_device(dev, log)
    if not ep_out: return False

    if not flash_firmware(ep_out, ep_in, fwdnx_data, ifwi_data, gpflags, log):
        log("[!] FW flash failed")
        return False

    if os_dnx_data and os_image_data:
        log("[*] Waiting for re-enumeration...")
        usb.util.dispose_resources(dev)
        time.sleep(5)
        dev2 = None
        for _ in range(120):
            dev2 = usb.core.find(idVendor=INTEL_VID, idProduct=MERRIFIELD_PID)
            if dev2: break
            time.sleep(0.5)
        if not dev2:
            log("[!] No re-enum — try fastboot")
            return False
        ep2_out, ep2_in = setup_device(dev2, log)
        if not ep2_out: return False
        if not flash_os(ep2_out, ep2_in, os_dnx_data, os_image_data, gpflags, log):
            log("[!] OS flash failed")
            return False

    log("[+] DONE")
    return True


def main():
    if os.geteuid() != 0:
        print("[!] Run with sudo!")
        sys.exit(1)

    srv = StatusServer()
    log = srv.log

    log("[*] DnX Flash Server — watching for Merrifield device")
    log(f"[*] Socket: {SOCK_PATH}")
    cfg = load_config(log)

    try:
        while True:
            srv.state = 'waiting'
            dev = usb.core.find(idVendor=INTEL_VID, idProduct=MERRIFIELD_PID)
            if dev:
                log(f"[+] Device detected: Bus {dev.bus} Dev {dev.address}")
                srv.state = 'flashing'
                try:
                    # Reload config each time
                    cfg = load_config(log)
                    # Pass dev into do_flash scope
                    base = os.path.dirname(os.path.abspath(CONFIG_PATH))
                    gpflags = int(cfg['gpflags'], 16)
                    def resolve(p):
                        if not p: return None
                        return p if os.path.isabs(p) else os.path.join(base, p)

                    fwdnx_path = resolve(cfg['fwdnx'])
                    fwimage_path = resolve(cfg['fwimage'])
                    osdnx_path = resolve(cfg.get('osdnx', ''))
                    osimage_path = resolve(cfg.get('osimage', ''))

                    log(f"[*] FW DnX: {fwdnx_path}")
                    with open(fwdnx_path, 'rb') as f: fwdnx_data = f.read()
                    log(f"    {len(fwdnx_data)}B")
                    log(f"[*] IFWI: {fwimage_path}")
                    with open(fwimage_path, 'rb') as f: ifwi_data = f.read()
                    log(f"    {len(ifwi_data)}B")

                    os_dnx_data = os_image_data = None
                    if osdnx_path and osimage_path:
                        with open(osdnx_path, 'rb') as f: os_dnx_data = f.read()
                        with open(osimage_path, 'rb') as f: os_image_data = f.read()

                    log(f"[*] GP Flags: 0x{gpflags:08X}")

                    ep_out, ep_in = setup_device(dev, log)
                    if ep_out:
                        ok = flash_firmware(ep_out, ep_in, fwdnx_data, ifwi_data, gpflags, log)
                        if ok:
                            if os_dnx_data and os_image_data:
                                log("[*] Waiting for re-enumeration...")
                                usb.util.dispose_resources(dev)
                                time.sleep(5)
                                dev2 = None
                                for _ in range(120):
                                    dev2 = usb.core.find(idVendor=INTEL_VID, idProduct=MERRIFIELD_PID)
                                    if dev2: break
                                    time.sleep(0.5)
                                if dev2:
                                    ep2_out, ep2_in = setup_device(dev2, log)
                                    if ep2_out:
                                        flash_os(ep2_out, ep2_in, os_dnx_data, os_image_data, gpflags, log)
                                else:
                                    log("[!] No re-enum — try fastboot")
                            log("[+] DONE")
                            srv.state = 'done'
                        else:
                            log("[!] FW flash failed")
                            srv.state = 'error'
                    else:
                        srv.state = 'error'
                except Exception as e:
                    log(f"[!] {e}")
                    log(traceback.format_exc())
                    srv.state = 'error'

                # Wait for device to disappear before scanning again
                log("[*] Waiting for device to disconnect...")
                for _ in range(60):
                    if not usb.core.find(idVendor=INTEL_VID, idProduct=MERRIFIELD_PID):
                        break
                    time.sleep(1)
                log("[*] Watching for next device...")
            else:
                time.sleep(1)
    except KeyboardInterrupt:
        log("[*] Shutting down")
    finally:
        srv.close()


if __name__ == '__main__':
    main()
