# Dell Venue 8 3840 LTE — Hardware Profile

## SoC & CPU
- **SoC**: Intel Merrifield (Tangier) / Saltbay platform
- **CPU**: Intel Atom Z3480 (Silvermont), 2 cores, family 6 model 74 stepping 8
- **Arch**: x86 (32-bit Android, but CPU supports x86_64 — `address sizes: 36 bits physical, 48 bits virtual`)
- **Clock**: 1.33GHz base, 2.13GHz turbo (measured)
- **Features**: SSE 4.2, AES-NI, RDRAND, VMX, EPT, POPCNT, MOVBE
- **Microcode**: 0x81b

## Memory
- **RAM**: ~1GB (968464 kB = 946 MB usable)

## Storage
- **eMMC**: 16GB (mmcblk0 = 15392768 blocks = ~15GB)
- **Partitions**:
  - mmcblk0p1: 256MB
  - mmcblk0p2: 32MB
  - mmcblk0p3: 128MB
  - mmcblk0p4: 128MB
  - mmcblk0p5: 128MB
  - mmcblk0p6: 1.5GB (system)
  - mmcblk0p7: 24MB
  - mmcblk0p8: 1.5GB (cache)
  - mmcblk0p9: 11GB (data)
  - mmcblk0boot0: 4MB
  - mmcblk0boot1: 4MB
  - mmcblk0rpmb: 4MB
- **Filesystem**: ext4, mounted via `/dev/block/platform/intel/by-label/`
- **Mount map**: factory, system (ro), cache, config, data, logs

## GPU
- **GPU**: PowerVR G6400 (Rogue) — `pvrsrvkm` driver
- **MMIO**: 0x80000000-0x8FFFFFFF (256MB) + 0xC0000000-0xC1FFFFFF (32MB)
- **OpenGL ES**: 3.0 (version code 196608)
- **DRM**: card0 (DSI-1 panel, DVI-D-1 port)
- **Display driver**: `tngdisp` (Tangier display, 1.3MB module)
- **Backlight**: `psb-bl` (Poulsbo/Cedarview lineage)

## Display
- **Resolution**: 1200x1920 (portrait)
- **Type**: DSI panel (MIPI DSI)
- **Physical size**: 107mm x 172mm
- **Density**: 320dpi

## WiFi
- **Chip**: Broadcom BCM4335 (SDIO: 02D0:4335)
- **Driver**: `bcm4335` kernel module (out-of-tree, marked (O))
- **Interface**: wlan0
- **cfg80211** based

## Bluetooth
- **Chip**: Broadcom (same BCM4335 combo chip)
- **Driver**: `bcm_bt_lpm` (low power management)
- **Config**: `/system/etc/bluetooth/bt_Venue.conf`

## Cellular / Modem
- **Modem**: Intel XMM 7160 (3.5G, flashless)
- **Baseband**: `Merrifield_7160_REV_3.5G_1423.021_V1.2_FLASHLESS`
- **RIL**: Intrinsyc Rapid-RIL M6.59
- **TTY**: `/dev/gsmtty13`
- **LTE capable** (default network type 9)

## Audio
- **Codec**: Wolfson WM8958 (I2C bus 1, addr 0x1a)
- **Driver**: `wm8958audio`
- **Sound cards**: card0, card1 (wm8958audio), card2
- **HDMI audio**: `hdmi_audio` module
- **Intel SST** (Smart Sound Technology): `intel_sst_driver` at 0x05E00000

## Camera
- **ISP**: Intel Atom ISP (atomisp_css2400b0_v21 module, 723KB)
- **Helper**: `libmsrlisthelper`
- **Video**: videobuf_vmalloc + videobuf_core

## Touch
- **Touchscreen**: Synaptics DSX (input: `synaptics_dsx`)
- **Pen**: Synaptics DSX pen (input: `synaptics_dsx_pen`)
- **Secondary touch**: ELAN eKTF2k (module: `ektf2k`)

## Sensors (I2C bus 6)
- **Accelerometer**: STMicro LSM303D (addr 0x1e, acc + mag)
- **Magnetometer**: STMicro LSM303D (addr 0x1e)
- **Gyroscope**: STMicro L3GD20 (addr 0x6b)
- **Ambient Light**: AL1721 (addr 0x23)
- **Proximity**: (likely part of AL1721)

## Power
- **Charger**: TI BQ24261 (I2C)
- **Fuel Gauge**: TI BQ27441 (I2C bus 2, addr 0x55)

## Other I2C Devices
- 4-0010: Unknown (bus 4, addr 0x10) — likely camera sensor
- 4-0036: Unknown (bus 4, addr 0x36) — likely camera sensor/VCM
- 6-0018: Unknown (bus 6, addr 0x18) — likely accelerometer alt
- 7-0010: Unknown (bus 7, addr 0x10) — likely NFC or secondary camera
- 7-0020: Unknown (bus 7, addr 0x20) — likely GPIO expander or LED
- 8-006b: Unknown (bus 8, addr 0x6b) — likely L3GD20 secondary

## PCI Topology (34 devices on bus 00)
- 00:00.0 — Host bridge
- 00:01.0-1.3 — SPI controllers
- 00:02.0 — GPU (PowerVR G6400, pvrsrvkm)
- 00:03.0 — ISP (atomisp)
- 00:04.0-04.3 — UART/HSU (Intel High Speed UART)
- 00:05.0 — HSU DMA
- 00:07.0-07.2 — SPI (3 controllers)
- 00:08.0-08.3 — I2C (Designware, 4 controllers)
- 00:09.0-09.2 — I2C (Designware, 3 controllers)
- 00:0a.0 — Unknown
- 00:0b.0 — Security Engine (sep54)
- 00:0c.0 — GPIO (langwell_gpio)
- 00:0d.0 — SST Audio (intel_sst_driver)
- 00:0e.0 — DMA (intel_mid_dmac)
- 00:10.0 — USB 2.0 EHCI (ehci_hcd)
- 00:11.0 — USB 3.0 DWC3 OTG (dwc3_otg / dwc_usb3_io)
- 00:12.0 — Unknown (large MMIO: FA000000-FAFFFFFF = 16MB)
- 00:13.0 — SCU IPC (intel_scu_ipc)
- 00:14.0 — PMU (intel_pmu_driver)
- 00:15.0 — DMA (intel_mid_dmac)
- 00:17.0 — Vibrator (intel_vibra_driver)
- 00:18.0 — Unknown

## Firmware Versions
- **IFWI**: 0005.11B1
- **Bootloader**: 1.31
- **SCU**: 00B0.004D
- **SCU BS**: 00B0.0003
- **IA32**: 0001.001F
- **PUnit**: 0000.0147
- **Chaabi**: 0000.004B
- **Microcode**: 0000.081B
- **ValHooks**: 0001.000D
- **MIA**: B018.2929

## Kernel
- **Version**: 3.10.20-263202-g0bcfac3
- **Build**: SMP PREEMPT, GCC 4.7, Aug 23 2014
- **Boot**: SFI (Simple Firmware Interface, NOT ACPI, NOT UEFI)

## Android
- **Version**: 4.4.4 (KitKat, API 19)
- **Build**: saltbay_64-user
- **ABI**: x86 primary, armeabi-v7a secondary (Houdini ARM translation)
- **VM**: Dalvik (libdvm.so)
- **Heap**: 16MB start, 192MB growth limit, 512MB max
- **Encryption**: unencrypted
- **SELinux**: enforcing

## Boot Modes

The Venue 8 3840 has four boot modes controlled by the IAFW (IA Firmware)
and OSNIB (OS-to-firmware notification information block):

### Normal Boot
- Power button press
- Boots OSIP → kernel → Android

### Recovery Mode
- **Hold Vol-Up** during power-on
- Boots the recovery partition
- Used for factory reset, sideloading

### Fastboot / Droidboot Mode
- **Hold Vol-Down + Power** from power-off, OR
- `adb reboot bootloader` from running Android
- Shows Droidboot provisioning screen (gears)
- Accepts `fastboot` commands (but `fastboot flash boot` fails on OSIP platforms — use DnX instead)

### DnX (Download and Execute) Mode
DnX is Intel's low-level firmware provisioning protocol. The device
enumerates as USB VID `0x8086`, PID `0xe005`.

**Entry Method 1 — Hardware (from Dell OSS_A195.pdf, pp. 4, 7-8):**
1. Power off completely (hold power 15+ seconds)
2. **Hold Volume Up** on the tablet
3. **Plug in USB cable** while holding Vol-Up
4. Screen stays black — device is in DnX mode
5. Release Vol-Up once flash tool detects the device

**Entry Method 2 — Software (from stock kernel reboot_target driver):**
```
adb reboot dnx
```
The kernel writes target ID `0x14` to OSNIB CMOS at base `0x0E`, then
reboots. IAFW reads this value and enters DnX mode.

Source: `kernel-src/linux/modules/drivers/platform/x86/reboot_target.c`
```c
static const struct name2id NAME2ID[] = {
    { "main",       0x00 },
    { "recovery",   0x0C },
    { "fastboot",   0x0E },
    { "dnx",        0x14 },
};
```

**Entry Method 3 — Automatic fallback:**
If the IAFW cannot find a valid OSIP entry or encounters an invalid
checksum, the device falls back to DnX mode automatically. A bricked
device will enumerate as `8086:e005` when USB is connected.

**Flashing via DnX:**
```bash
# Configure dnx_flash.json with osdnx + osimage, then:
sudo python3 dnx_flash.py
# Flash tool watches for device, runs protocol automatically
```

**DnX Protocol Parameters:**
| Parameter | Value |
|-----------|-------|
| USB VID:PID | `8086:e005` |
| GP Flags | `0x80000007` |
| OSNIB target ID | `0x14` |
| FW DnX binary | `fwr_dnx_PRQ_ww27_001.bin` |
| IFWI image | `IFWI_MERR_PRQ_UOS_TH2_YT2_ww27_001.bin` |
| OS DnX binary | `osr_dnx_PRQ_ww27_001.bin` |
| xFSTK tab | "MRD A0/B0 + MOOR A0 + CRC" |

## Key Driver Modules (loaded)
| Module | Size | Description |
|--------|------|-------------|
| tngdisp | 1.3MB | Tangier display (PowerVR) |
| atomisp_css2400b0_v21 | 724KB | Intel Atom ISP camera |
| bcm4335 | 540KB | Broadcom WiFi (out-of-tree) |
| cfg80211 | 441KB | Wireless config |
| ektf2k | 98KB | ELAN touchscreen |
| hdmi_audio | 45KB | HDMI audio output |
| dfrgx | 19KB | GPU frequency management |
| bcm_bt_lpm | 14KB | Bluetooth low power |
| libmsrlisthelper | 13KB | Camera ISP helper |
| videobuf_vmalloc | 13KB | Video buffer alloc |
| videobuf_core | 25KB | Video buffer core |
