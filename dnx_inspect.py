#!/usr/bin/env python3
"""Inspect DnX and IFWI binaries — verify parsing matches xFSTK expectations."""

import struct
import sys

def dword_checksum(data):
    """xFSTK-style DWORD checksum: sum of all 32-bit little-endian words (mod 2^32)."""
    total = 0
    for i in range(0, len(data) - 3, 4):
        total = (total + struct.unpack_from('<I', data, i)[0]) & 0xFFFFFFFF
    return total

def inspect_dnx_fw(path):
    print(f"\n{'='*60}")
    print(f"DnX FW Binary: {path}")
    print(f"{'='*60}")
    with open(path, 'rb') as f:
        data = f.read()
    file_size = len(data)
    print(f"  File size: {file_size} bytes (0x{file_size:X})")

    # Look for markers
    for marker in [b'CDPH', b'CH00', b'$CHT', b'DTKN', b'ChPr']:
        pos = data.find(marker)
        if pos >= 0:
            print(f"  {marker.decode()}: offset {pos} (0x{pos:X})")
        else:
            print(f"  {marker.decode()}: NOT FOUND")

    # CDPH should be at the end (last 512 bytes)
    cdph_pos = data.find(b'CDPH')
    if cdph_pos >= 0:
        print(f"  CDPH at end? {cdph_pos == file_size - 512} (expected at {file_size - 512})")

    # Chaabi size detection (matching xFSTK initChaabiSize)
    CH00 = data.find(b'CH00')
    CDPH = data.find(b'CDPH')
    CHT = data.find(b'$CHT')
    DTKN = data.find(b'DTKN')
    ChPr = data.find(b'ChPr')

    token_size = 0
    fw_size = 0
    ch00_start = 0

    if CH00 >= 0:
        ch00_start = CH00 - 0x80  # 128 bytes before CH00
        print(f"  CH00 start (ch00-0x80): {ch00_start} (0x{ch00_start:X})")

    if DTKN >= 0 and CH00 >= 0 and DTKN < ch00_start:
        # B0+ path: DTKN is before first CH00, search for second CH00
        search_start = DTKN + 16384  # SIXTEEN_KB
        ch00_2 = data.find(b'CH00', search_start)
        if ch00_2 >= 0:
            ch00_start = ch00_2 - 0x80
            token_size = ch00_start - DTKN
            print(f"  B0+ Chaabi: DTKN={DTKN}, second CH00 at {ch00_2}, ch00_start={ch00_start}")
            print(f"  Token size: {token_size} (0x{token_size:X})")
    elif CHT >= 0 and CH00 >= 0:
        token_size = ch00_start - (CHT - 0x80)
        print(f"  A0 Chaabi: $CHT={CHT}, token_size={token_size}")
    elif ChPr >= 0 and CH00 >= 0:
        token_size = ch00_start - ChPr
        print(f"  ChPr Chaabi: ChPr={ChPr}, token_size={token_size}")

    if CDPH >= 0 and CH00 >= 0:
        fw_size = CDPH - ch00_start
        print(f"  FW size: {fw_size} (0x{fw_size:X})")

    if token_size == 0 or fw_size == 0:
        token_size = 16384  # defaults
        fw_size = 73728
        print(f"  Using defaults: token={token_size}, fw={fw_size}")

    chaabi_total = token_size + fw_size
    main_size = file_size - chaabi_total - 512  # minus CDPH
    print(f"\n  Chaabi total: {chaabi_total} (0x{chaabi_total:X})")
    print(f"  Main code size: {main_size} (0x{main_size:X})")

    # Build DXBL and CHFI00 like xFSTK
    cdph = data[file_size - 512:]
    dxbl = cdph + data[:main_size]
    chaabi_start = file_size - chaabi_total - 512
    chfi00 = cdph + data[chaabi_start:chaabi_start + chaabi_total]

    print(f"  DXBL size: {len(dxbl)} (0x{len(dxbl):X})")
    print(f"  CHFI00 size: {len(chfi00)} (0x{len(chfi00):X})")
    print(f"  DXBL first 16 bytes: {dxbl[:16].hex()}")
    print(f"  CHFI00 first 16 bytes: {chfi00[:16].hex()}")

    # DnX header
    checksum = file_size ^ 0x80000007
    print(f"\n  DnX header would be:")
    print(f"    size={file_size} (0x{file_size:X})")
    print(f"    gpflags=0x80000007")
    print(f"    checksum=0x{checksum:08X}")
    hdr = struct.pack('<IIIIII', file_size, 0x80000007, 0, 0, 0, checksum)
    print(f"    raw: {hdr.hex()}")


def inspect_ifwi(path):
    print(f"\n{'='*60}")
    print(f"IFWI Image: {path}")
    print(f"{'='*60}")
    with open(path, 'rb') as f:
        data = f.read()
    file_size = len(data)
    FOUR_MB = 4 * 1024 * 1024
    ONE28_K = 128 * 1024

    print(f"  File size: {file_size} bytes (0x{file_size:X})")
    print(f"  4MB = {FOUR_MB} (0x{FOUR_MB:X})")
    print(f"  Difference: {file_size - FOUR_MB} bytes")

    # Search for markers
    for marker in [b'UPH$', b'$CFS', b'IFWI', b'CDPH']:
        pos = 0
        positions = []
        while True:
            pos = data.find(marker, pos)
            if pos < 0:
                break
            positions.append(pos)
            pos += 1
        if positions:
            for p in positions:
                print(f"  {marker.decode()}: offset {p} (0x{p:X}), bytes after: {data[p:p+8].hex()}")
        else:
            print(f"  {marker.decode()}: NOT FOUND")

    # Footer detection (xFSTK FooterSizeInit)
    uph_pos = -1
    cfs_pos = -1
    # Search from end (xFSTK uses StringLocation which searches from beginning)
    # Actually xFSTK searches the file for UPH$ and $CFS
    for marker_bytes, name in [(b'UPH$', 'UPH$'), (b'$CFS', '$CFS')]:
        pos = data.find(marker_bytes)
        if pos >= 0:
            if name == 'UPH$':
                uph_pos = pos
            elif name == '$CFS':
                cfs_pos = pos

    footer_size = 0
    if cfs_pos >= 0 and uph_pos >= 0 and cfs_pos > uph_pos:
        footer_size = file_size - (cfs_pos - 4)
        print(f"\n  Footer detected: $CFS after UPH$, footer_size={footer_size}")
    elif uph_pos >= 0 and uph_pos < FOUR_MB:
        footer_size = 1
        print(f"\n  Footer=1 (UPH$ at {uph_pos} < 4MB, CFS-less footer)")
    else:
        print(f"\n  No footer (UPH$ at {uph_pos}, {'< 4MB' if uph_pos < FOUR_MB else '>= 4MB'})")

    # FUPH analysis
    if uph_pos >= 0:
        fuph_start = uph_pos
        fuph_data = data[fuph_start:]
        fuph_size = len(fuph_data)
        print(f"\n  FUPH at offset {fuph_start} (0x{fuph_start:X})")
        print(f"  FUPH size: {fuph_size} bytes (0x{fuph_size:X})")
        print(f"  FUPH is B0+ (164)? {fuph_size == 0xA4}")

        # Dump FUPH header fields
        if fuph_size >= 16:
            print(f"  FUPH first 32 bytes: {fuph_data[:32].hex()}")
            # Parse as DWORDs
            for i in range(min(fuph_size // 4, 42)):
                val = struct.unpack_from('<I', fuph_data, i * 4)[0]
                ascii_rep = fuph_data[i*4:i*4+4].decode('ascii', errors='replace')
                print(f"    DWORD[{i:2d}] @ offset {i*4:3d}: 0x{val:08X} = {val:10d}  '{ascii_rep}'")

        # Verify checksums
        if fuph_size >= 16:
            stored_checksum = struct.unpack_from('<I', fuph_data, 12)[0]
            print(f"\n  Stored FUPH checksum (offset 12): 0x{stored_checksum:08X}")
            # Zero checksum field and recalculate
            test_data = bytearray(fuph_data)
            test_data[12:16] = b'\x00\x00\x00\x00'
            calc_checksum = dword_checksum(bytes(test_data))
            print(f"  Calculated checksum: 0x{calc_checksum:08X}")
            print(f"  Checksum valid? {stored_checksum == calc_checksum}")

        # FUP_LAST_CHUNK_OFFSET = 38
        if fuph_size >= 156:
            last_chunk_cksum = struct.unpack_from('<I', fuph_data, 38 * 4)[0]
            print(f"\n  FUPH last chunk checksum (DWORD[38]): 0x{last_chunk_cksum:08X}")
            # Calculate actual last 128K checksum
            last_128k = data[FOUR_MB - ONE28_K:FOUR_MB]
            actual_cksum = dword_checksum(last_128k)
            print(f"  Actual last 128K checksum: 0x{actual_cksum:08X}")
            print(f"  Match? {last_chunk_cksum == actual_cksum}")

    # B0+ IFWI size extraction (xFSTK method)
    ifwi_marker_pos = data.find(b'IFWI')
    if ifwi_marker_pos >= 0:
        ifwi_size_val = struct.unpack_from('<I', data, ifwi_marker_pos + 4)[0]
        print(f"\n  'IFWI' marker at offset {ifwi_marker_pos} (0x{ifwi_marker_pos:X})")
        print(f"  Size field after IFWI: {ifwi_size_val} (0x{ifwi_size_val:X})")
        print(f"  Equals FOUR_MB? {ifwi_size_val == FOUR_MB}")

    # IFWI chunk calculation
    if footer_size == 0:
        ifwi_data_size = file_size - fuph_size if uph_pos >= 0 else file_size
    else:
        ifwi_data_size = file_size

    num_chunks = ifwi_data_size // ONE28_K
    residual = ifwi_data_size - (num_chunks * ONE28_K)
    total_chunks = num_chunks + (1 if residual else 0)
    print(f"\n  IFWI data size: {ifwi_data_size} (0x{ifwi_data_size:X})")
    print(f"  Chunks: {num_chunks} full + {residual} residual = {total_chunks} total")
    print(f"  Residual bytes: {residual}")

    # Show first and last chunk hashes for verification
    import hashlib
    first_chunk = data[:ONE28_K]
    last_chunk_start = (total_chunks - 1) * ONE28_K
    last_chunk = data[last_chunk_start:last_chunk_start + ONE28_K]
    print(f"\n  First chunk (0-128K) SHA256: {hashlib.sha256(first_chunk).hexdigest()[:16]}...")
    print(f"  Last chunk ({last_chunk_start}-{last_chunk_start+ONE28_K}) SHA256: {hashlib.sha256(last_chunk).hexdigest()[:16]}...")


if __name__ == '__main__':
    import glob
    base = '/home/thomas/venue8-flash'

    dnx_files = glob.glob(f'{base}/fwr_dnx_*.bin')
    ifwi_files = glob.glob(f'{base}/IFWI_*.bin')

    for f in dnx_files:
        inspect_dnx_fw(f)
    for f in ifwi_files:
        inspect_ifwi(f)

    if not dnx_files and not ifwi_files:
        print("No firmware files found in", base)
