# DnX OS Flash Research — Intel Merrifield / Dell Venue 8 3840

## Executive Summary

**The OS flash phase fails because your Python tool's approach to the OS phase is fundamentally wrong.** Based on deep analysis of the xFSTK source code (`mrfdldrstate.cpp`, `merrifielddownloader.cpp`, `merrifieldos.cpp`, `merrifieldoptions.cpp`, `merrifieldmessages.h`), the following critical issues have been identified:

1. **The FW and OS phases happen in ONE continuous USB session** — there is NO USB re-enumeration between them
2. **The "osimage" is NOT your custom boot image** — it must be `droidboot.img.POS.bin` (a signed droidboot bootloader)
3. **The OS DnX phase loads droidboot into RAM**, which then presents a fastboot interface for actual OS flashing
4. **Your tool incorrectly waits for re-enumeration** after HLT$, but the device has already rebooted and left DnX mode

---

## Finding 1: FW and OS Phases Are ONE Continuous State Machine

### Source Evidence

From `mrfdldrstate.cpp` — the `StateMachine()` method:

```cpp
bool mrfdldrstate::StateMachine()
{
    if(!Start()) return false;
    while(1) {
        ackcode = GetOpCode();
        iter1 = m_bulk_ack_map.find(ackcode);
        if (iter1 != m_bulk_ack_map.end()) {
            iter1->second->Accept(*this);
        } else {
            m_abort = true; ret = false;
        }
        if(m_abort) break;
        if(m_fw_done) { FinishProgressBar(); break; }
        if(m_os_done) { FinishProgressBar(); break; }
    }
    return ret;
}
```

**The state machine runs a single loop** that handles BOTH firmware and OS operations. There is no "stop, re-enumerate, restart" between phases. The device transitions from FW states to OS states **within the same USB session**.

### The Transition Mechanism

The FW phase does NOT end with HLT$ when OS flash is requested. Instead:

1. The gpflags (embedded in the DnX header sent at the start) tell the device firmware whether OS loading follows
2. If gpflags indicate OS, after IFWI programming the device sends `DORM` (Device OS Ready Message) instead of `HLT$`
3. `DORM` signals the host to begin the OS DnX phase immediately, on the same USB connection

From `mrfdldrstate.cpp`:
```cpp
void mrfdldrstate::Visit(MrfdOsHandleDORM& )
{
    InitializeProgressBar(DOWNLOAD_OS_PROGRESS);
    m_b_DnX_OS = m_gpflags & 0x20;
    if(m_b_DnX_OS) {
        GotoState(DLDR_STATE_OS_MISC);
    } else if(m_gpflags & 0x1) {
        GotoState(DLDR_STATE_OS_NORMAL);
    }
}
```

### What This Means For Your Tool

Your tool's `flash_firmware()` function already correctly handles the case where `DORM` appears — it breaks out of the FW loop with `fw_done = True`. But then your tool **disposes of the USB device, waits 5 seconds, and tries to find a new device**. This is wrong. The OS phase must continue on the SAME connection immediately after DORM.

---

## Finding 2: GP Flags Control the Protocol Flow

### GP Flags Bit Definitions (from xFSTK source)

`0x80000007` = `0x80000000 | 0x4 | 0x2 | 0x1`

| Bit(s) | Hex Mask | Meaning |
|--------|----------|---------|
| Bit 0 | `0x01` | OS normal download enabled |
| Bit 1 | `0x02` | FW cold reset flag |
| Bit 2 | `0x04` | (Reserved/platform-specific) |
| Bit 5 | `0x20` | OS misc/DnX mode (Chaabi/secure) |
| Bit 31 | `0x80000000` | Upper flag (always set for Merrifield) |

From `merrifieldoptions.cpp`:
```cpp
// If no gpflags specified, auto-determine:
this->gpFlags = ((this->osImagePath != "BLANK.bin")) ? 0x80000001 : 0x80000000;
```

**Key insight**: When an OS image is provided, bit 0 (`0x01`) is set. The value `0x80000007` has bits 0, 1, 2 set — this enables OS download in normal mode.

### How gpflags are embedded

The gpflags are sent in the **DnX header** at the very start:
```
Bytes 0-3:  Total DnX file size
Bytes 4-7:  GP flags
Bytes 8-19: Reserved (zeros)
Bytes 20-23: XOR checksum (size ^ gpflags)
```

The device firmware reads gpflags from this header and uses them to determine the complete protocol flow. **The device knows whether OS flash will follow from the very first DnX header it receives.**

---

## Finding 3: The "OS Image" is droidboot, NOT Your Custom Boot Image

### What xFSTK Actually Sends as "osimage"

The standard xFSTK unbrick command for Dell Venue is:
```bash
xfstk-dldr-solo --gpflags 0x80000007 \
  --fwdnx dnx_fwr_PRQ.bin \
  --fwimage for_product_ifwi_PRQ.bin \
  --osdnx dnx_osr_PRQ.bin \
  --osimage droidboot.img.POS.bin
```

The `--osimage` parameter is **`droidboot.img.POS.bin`** — a signed droidboot bootloader image in OSIP format (~16MB). This is NOT an Android system image and NOT a custom kernel boot.img.

### What droidboot Does

The DnX OS phase loads `droidboot.img.POS.bin` into RAM and executes it. Droidboot then:
1. Initializes the hardware
2. Presents a **fastboot USB interface** (different VID:PID)
3. Waits for fastboot commands to flash partitions

### The Two-Phase Flash Approach

**Phase 1 — DnX Protocol (xFSTK):**
- Flash IFWI firmware
- Load droidboot into RAM
- Device now runs droidboot (fastboot mode)

**Phase 2 — Fastboot Protocol (separate tool):**
```bash
fastboot flash boot boot.img
fastboot flash system system.img
fastboot flash recovery recovery.img
fastboot reboot
```

This is how ALL Dell Venue unbrick procedures work. xFSTK does NOT flash the actual OS directly. It loads a fastboot-capable bootloader into RAM, then fastboot handles the partition flashing.

---

## Finding 4: Why HLT$ Means "Device Is Done, No OS Phase"

### What Actually Happens in Your Case

Your tool sends gpflags `0x80000007` in the DnX header, but does NOT provide osdnx/osimage to the device in the protocol. The flow:

1. DnER preamble sent
2. Device responds DFRM (virgin eMMC) or DxxM (non-virgin)
3. DnX header sent (with gpflags 0x80000007)
4. DXBL/DCFI00/RUPHS/RUPH handshake
5. IFWI chunks sent (2 passes)
6. **Device sends HLT$** — firmware update complete

When you receive `HLT$`, the device has:
- Written the IFWI to flash
- Rebooted itself using the new firmware
- Exited DnX mode entirely

The device that re-enumerates as `8086:e005` after HLT$ is NOT in DnX mode anymore. It may be:
- In normal boot (if OSIP is valid) — booting Android
- In fastboot/droidboot mode (if the gpflags triggered a fastboot boot)
- In DnX mode again (only if boot fails and it falls back)

**When the device re-enumerates with the same PID, it's likely starting to boot normally**, not waiting for OS DnX data. That's why all reads time out — the device is busy booting its OS, not listening for DnX protocol messages.

---

## Finding 5: The Correct Approach

### Option A: FW-only + Fastboot (Recommended)

If you just want to flash IFWI and then flash your custom kernel:

1. Flash IFWI only (no osdnx/osimage — your current working approach)
2. Wait for device to boot into Android/fastboot
3. Use fastboot to flash your boot.img:
   ```bash
   fastboot flash boot boot-6.6-new.img
   ```

**Note**: `fastboot flash boot` may not work on OSIP-based Merrifield because the boot partition uses OSIP headers, not standard Android boot format. You may need to use `adb` from a running Android to `dd` the boot image.

### Option B: Full DnX FW+OS (droidboot into RAM)

If you want to use the full xFSTK FW+OS protocol:

1. Use the correct `droidboot.img.POS.bin` as osimage (you already have this file!)
2. Use `osr_dnx_PRQ_ww27_001.bin` as osdnx
3. **Do NOT re-enumerate** — handle DORM in the same USB session
4. After droidboot loads, it will re-enumerate as a **fastboot device** (different PID)
5. Then use fastboot to flash your kernel

### Option C: Modify the FW DnX header gpflags

If you only want IFWI and don't need OS loading:
- Use gpflags `0x80000000` instead of `0x80000007`
- This tells the device firmware: "FW only, no OS phase"
- The device will still send HLT$ after IFWI programming

---

## Finding 6: How xFSTK Handles the FW→OS Transition (Complete)

From `merrifielddownloader.cpp` — `UpdateTarget()`:

```cpp
bool MerrifieldDownloader::UpdateTarget()
{
    // Check if OS download is requested
    if(m_dldr_state.GetOppCode() & 0x1) {
        // Both FW and OS in one session
        m_dldr_state.DoUpdate(fwdnx, fwimage, osdnx, osimage, ...);
    } else {
        // FW only
        m_dldr_state.DoUpdate(fwdnx, fwimage, NULL, NULL, ...);
    }
}
```

The state machine `DoUpdate()` loads ALL file paths upfront. The protocol then proceeds:

### FW Phase States
```
Start() → send DnER preamble
← DFRM (virgin) or DxxM (non-virgin)
→ DnX header (24 bytes: size + gpflags + reserved + checksum)
← DXBL → send DXBL data (DnX binary minus Chaabi)
← DCFI00 → send Chaabi data (token + FW + CDPH)
← RUPHS → send FUPH size (4 bytes)
← RUPH → send FUPH data
← DIFWI → send IFWI chunks (128KB each, multiple passes)
← HLT$ → FW DONE (if no OS phase)
   OR
← DORM → transition to OS phase
```

### OS Phase States (within same session)
```
← DORM → initialize OS download state
→ OS DnX header (24 bytes)
← DXBL → send OS DnX binary
← DCFI00 → send OS Chaabi data
← OSIPSZ → send OSIP size command (H_SEND_OSIP_SIZE = 0x01)
← ROSIP → send OSIP header (512 bytes from osimage)
← RIMG → send OS image chunks (128KB * RIMGChunkSize)
← RIMG → ... (repeat until all data sent)
← EOIU → send "DFN" acknowledgment, set os_done
```

### USB Re-enumeration (USB 3.0 only)

The `OsDXBL()` method has USB re-enumeration code, but it's **conditionally compiled only for USB 3.0**:
```cpp
#ifdef USB30_POC_OSDNX
    SleepMs(20000);
    m_usbdev->Abort();
    // re-open device loop
#endif
```

For USB 2.0 (which is what the Venue 8 uses), there is NO re-enumeration. The OS phase continues on the same connection.

---

## Finding 7: DnX Header Format

```
Offset  Size  Field
0       4     DnX file size (total, including headers)
4       4     GP flags
8       4     Reserved (0)
12      4     Reserved (0)
16      4     Reserved (0)
20      4     Checksum (file_size XOR gpflags)
```
Total: 24 bytes (`DNX_FW_SIZE_HDR_SIZE`)

This is the same format for both FW DnX and OS DnX headers.

---

## Finding 8: Protocol Message Reference (Merrifield)

### Preambles (host → device)
| Value | ASCII | Purpose |
|-------|-------|---------|
| `0x52456E44` | `DnER` | Download and Execute Request |
| `0x51524449` | `IDRQ` | ID Request |

### FW Acknowledgments (device → host)
| ASCII | Meaning |
|-------|---------|
| `DFRM` | Device Firmware Ready Message (virgin eMMC) |
| `DxxM` | Device non-virgin Message (eMMC has data) |
| `DXBL` | Device requesting DnX Boot Loader data |
| `DCFI00` | Device requesting Chaabi FW Image |
| `RUPHS` | Request FUPH Size |
| `RUPH` | Request FUPH data |
| `DIFWI` | Device requesting IFWI chunk |
| `HLT$` | Halt — FW update SUCCESS |
| `HLT0` | Halt — error condition |
| `RESET` | Device requesting reset |
| `MFLD` | Merrifield platform ID |
| `CLVT` | Clovertrail platform ID |

### OS Messages (device → host)
| ASCII | Meaning |
|-------|---------|
| `DORM` | Device OS Ready Message — begin OS phase |
| `OSIP Sz` | Request OSIP size/command |
| `ROSIP` | Request OSIP header |
| `RIMG` | Request OS image chunk |
| `EOIU` | End Of Image Upload — OS flash complete |
| `DONE` | Transfer done |

### Host OS Commands
| Value | Meaning |
|-------|---------|
| `0x01` | H_SEND_OSIP_SIZE |
| `0x02` | H_REQ_XFER_SIZE |
| `0x03` | H_XFER_COMPLETE |

### Error Codes
`ER00` through `ER99` — various eMMC, provisioning, and flash errors.

---

## Recommended Fix for dnx_flash.py

### Critical Changes Needed

1. **Do NOT disconnect/re-enumerate between FW and OS phases.** When `DORM` is received after IFWI flash, immediately start the OS protocol on the same endpoints.

2. **Use `droidboot.img.POS.bin` as osimage**, not `boot-6.6-new.img`. The DnX OS phase loads droidboot into RAM to provide fastboot.

3. **After droidboot loads**, the device will re-enumerate as a fastboot device (different USB PID). Then use fastboot to flash your custom kernel.

4. **If you only need IFWI + custom kernel**, skip the OS DnX entirely:
   - Flash IFWI only (already works)
   - Boot device normally
   - Use `adb` or `fastboot` to flash boot partition

### Minimal Code Fix

In `flash_firmware()`, when `DORM` or `OSIPSZ` is received:
```python
elif opname in ('DORM', 'OSIPSZ'):
    log(f"[*] {opname} — transitioning to OS phase")
    # DO NOT break — continue on same endpoints
    if os_dnx_data and os_image_data:
        return flash_os(ep_out, ep_in, os_dnx_data, os_image_data, gpflags, log)
    else:
        log("[!] DORM received but no OS files provided")
        fw_done = True; break
```

And remove the re-enumeration code from `main()`.

---

## Why the Device Appears Unresponsive After HLT$

When your tool receives `HLT$` (not `DORM`), it means:
- The device firmware did NOT request an OS phase
- The IFWI flash completed
- The device is now **rebooting**

The device re-enumerates as `8086:e005` briefly during reboot, but this is the normal USB disconnect/reconnect cycle. It is NOT waiting for DnX commands. The device will either:
1. Boot into Android (normal boot)
2. Boot into droidboot/fastboot (if OSIP is corrupted)
3. Fall back into DnX mode (if boot completely fails)

Your reads time out because the device is booting, not listening for DnX protocol data.

---

## Sources

- [edison-fw/xFSTK GitHub Repository](https://github.com/edison-fw/xFSTK)
  - `xfstk-sources/core/factory/platforms/merrifield/mrfdldrstate.cpp` (state machine, 81KB)
  - `xfstk-sources/core/factory/platforms/merrifield/merrifielddownloader.cpp`
  - `xfstk-sources/core/factory/platforms/merrifield/merrifieldos.cpp`
  - `xfstk-sources/core/factory/platforms/merrifield/merrifieldfw.cpp`
  - `xfstk-sources/core/factory/platforms/merrifield/merrifieldmessages.h`
  - `xfstk-sources/core/factory/platforms/merrifield/merrifieldoptions.cpp`
  - `xfstk-sources/core/factory/platforms/merrifield/mrfdldrhandler.h`
- [Edison Wiki — Dell Venue 7 3740](https://edison-fw.github.io/edison-wiki/edison-venue.html)
- [Dell Open Source Guide (OSS_A195.pdf)](https://opensource.dell.com/releases/Venue_7_3740_Merrifield/developer-edition/Doc/OSS_A195.pdf)
- [Dell Venue 8 3840 Unbrick Files](https://opensource.dell.com/releases/Venue_8_3840_Merrifield/developer-edition/A195/Unbrick/)
- [Intel EC DnX Documentation](https://intel.github.io/ecfw-zephyr/reference/dnx/index.html)
- [Intel Community — Moorefield Flash Issues](https://community.intel.com/t5/Processors/Intel-Atom-Z3560-Moorefield-Not-Flashing-Correctly/td-p/1315597)
- [XDA Forums — Dell Venue 8 3840](https://xdaforums.com/t/dell-venue-8-3840-root-unbrick-flash.3048312/)
