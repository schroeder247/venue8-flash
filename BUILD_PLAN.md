# Dell Venue 8 3840 LTE — Android 15 + UBports Build Plan

## Target
- **AOSP version**: Android 15 (AOSP master or android-15.0.0_r1)
- **Kernel**: 6.6 GKI (android15-6.6 branch) — cloned to `project/kernel-port/android15-6.6/`
- **Architecture**: x86_64
- **Codename**: saltbay / merrifield
- **Secondary target**: UBports (Ubuntu Touch) contribution

## Strategy
Port ALL Dell kernel driver sources (MIT/GPLv2) to kernel 6.6, replacing the stock 3.10
kernel entirely. No proprietary kernel modules — everything open source. Userspace blobs
(GPU libs, HALs) run via 32-bit compatibility layer.

## What We Have

### Kernel Source (GPL, fully open)
`/home/thomas/venue8-flash/kernel-src/linux/`
- Full 3.10.20 kernel with Dell/Intel patches
- Defconfig: `x86_64_mrfl_defconfig`
- External modules with full source:
  - **GPU**: `modules/intel_media/graphics/rgx/` — PowerVR Rogue G6400 (346 .c files, Dual MIT/GPLv2)
  - **Display**: `modules/intel_media/display/tng/` — Tangier DSI/DRM (88 .c files, MIT)
  - **Camera**: `modules/camera/` — Intel atomisp2400 (321 .c files, GPLv2)
  - **WiFi**: `modules/wlan/` — BCM4335 (145 .c files)
  - **Modem control**: `modules/drivers/modem_control/` — IMC XMM7160 (5 .c files, GPLv2)
  - **HSIC USB**: `kernel/drivers/usb/host/ehci-tangier-hsic-pci.c` — EHCI for modem (GPLv2)

### Proprietary Blobs (extracted from factory system.img)
`/home/thomas/venue8-flash/extracted/vendor-blobs/`
- **GPU userspace**: PowerVR EGL/GLES libs, gralloc, hwcomposer
- **HALs**: audio, camera, sensors, lights, power, GPS, BT (29 modules)
- **Modem**: mmgr, rild, nvm_server, Rapid-RIL libs, telephony configs
- **Firmware**: WiFi (fw_bcmdhd.bin_4335_b0), modem (modem.zip 95MB), camera ISP, video codecs

### Factory Images
`/home/thomas/venue8-flash/images-YTD802A519500/`
- boot.img (OSIP format), recovery.img, system.img, droidboot.img, radio_firmware.bin

### Android 15 Kernel (cloned)
`/home/thomas/venue8-flash/project/kernel-port/android15-6.6/` — 1.8GB, android15-6.6 branch

---

## Build Phases

### Phase 1: Kernel 6.6 Platform Bringup

#### 1A: SFI → Hardcoded Platform Data
SFI (Simple Firmware Interface) was removed from mainline in kernel 5.12. The Merrifield
SoC uses SFI to enumerate devices, GPIOs, and PMIC configs. Must replace with:
- **PCI quirk fixups** (`DECLARE_PCI_FIXUP_EARLY`) for each PCI device
- **Hardcoded platform_device** registrations for non-PCI devices (sensors, touch, modem ctrl)
- **GPIO pin numbers** extracted from Dell kernel SFI tables or runtime sysfs

Key mainline infrastructure that IS available:
- `intel_scu_ipc` — in mainline (`drivers/platform/x86/intel/scu/ipc.c`)
- `intel_soc_pmic_mrfld` — Basin Cove PMIC driver in mainline
- `pinctrl-merrifield` — pin control in mainline (`drivers/pinctrl/intel/`)

**Files to create**: `arch/x86/platform/intel-mid/merrifield-venue8.c` (~500 LOC)

#### 1B: PowerVR Rogue GPU Driver
The GPU source (346 .c files) needs porting from kernel 3.10 DRM APIs to 6.6 DRM APIs:
- Replace `drm_fb_helper` legacy with atomic modesetting
- Update PVR services kernel interface (memory management, sync)
- Port `tngdisp` display driver to DRM/KMS framework
- ~15,000 LOC total, **HIGH EFFORT**

Alternative: Use the open-source `pvr` driver being upstreamed by Imagination Technologies
for PowerVR Rogue GPUs (merged in kernel 6.8+). Check if G6400 is supported.

#### 1C: Display / DSI Panel
Port `tngdisp` (88 .c files) to modern DRM/KMS:
- Replace Intel MID framebuffer with `drm_simple_display_pipe` or full CRTC/encoder/connector
- Port MIPI DSI panel driver to `drm_panel` framework
- Port backlight to mainline `backlight_device`
- **MEDIUM EFFORT** (~2000 LOC to port)

#### 1D: WiFi (BCM4335)
The BCM4335 driver in Dell source is a Broadcom fullmac driver (brcmfmac variant).
Mainline `brcmfmac` supports BCM4335 SDIO natively:
- PCI ID `02D0:4335` is in mainline `brcmfmac` device table
- Need firmware: `brcm/brcmfmac4335-sdio.bin` (extract from vendor-blobs `fw_bcmdhd.bin_4335_b0`)
- **LOW EFFORT** — likely works out of box with correct firmware file

#### 1E: Bluetooth (BCM4335)
Same chip as WiFi. Mainline `btbcm` + `hci_uart` should work:
- Need firmware: `brcm/BCM4335A0.hcd` (extract from vendor BT firmware)
- **LOW EFFORT**

#### 1F: Camera (atomisp)
Intel atomisp was merged into mainline staging (`drivers/staging/media/atomisp/`).
The mainline version targets atomisp2 (ISP2400/ISP2401). Our device uses ISP2400b0:
- Check if `atomisp_css2400b0_v21` variant is in staging
- May need to use mainline staging driver directly
- **MEDIUM EFFORT** — staging driver exists but may need platform glue

#### 1G: Audio (WM8958 + Intel SST)
- WM8958 codec: mainline `wm8994` driver supports WM8958 variant
- Intel SST: `snd-intel-sst-core` in mainline, but Merrifield SST may need platform data
- **MEDIUM EFFORT**

#### 1H: Touch (Synaptics DSX + ELAN eKTF2k)
- Synaptics: mainline `rmi4` or `synaptics_dsx` drivers
- ELAN: mainline `elan_i2c` driver
- **LOW EFFORT**

#### 1I: Sensors (I2C)
All sensors have mainline IIO drivers:
- LSM303D (acc+mag): `st_lsm303d` or `st_accel`/`st_magn`
- L3GD20 (gyro): `st_gyro`
- AL1721 (ALS): may need small driver
- **LOW EFFORT**

#### 1J: Power (BQ24261 + BQ27441)
Both have mainline drivers:
- BQ24261: `bq24257` driver (compatible)
- BQ27441: `bq27xxx_battery`
- **LOW EFFORT**

---

### Phase 2: LTE Modem (Intel XMM 7160)

This is the most complex subsystem. The modem is "flashless" — firmware must be
loaded at every boot via USB.

#### Architecture
```
                    Kernel Space
┌─────────────────────────────────────────────┐
│  modem_control.ko    ehci-tangier-hsic.ko   │
│  (GPIO power ctrl)   (HSIC USB transport)   │
│         │                    │               │
│    PMIC power           USB device          │
│    (SCU IPC)         VID:PID changes        │
└─────────────────────────────────────────────┘
                         │
                    Userspace
┌─────────────────────────────────────────────┐
│  mmgr (firmware loader)                      │
│    1. Enable HSIC link (sysfs)              │
│    2. Modem appears as 8087:0716 (flash)    │
│    3. Upload PSI → EBL → FLS firmware       │
│    4. Modem reboots as 1519:0452 (baseband) │
│    5. Start n_gsm MUX (frame_size=1509)     │
│    6. Create /dev/gsmtty0-31 channels       │
│                                              │
│  rild + librapid-ril-core.so (Android)      │
│    OR                                        │
│  oFono + ifxmodem plugin (UBports)          │
│    - AT commands via gsmtty channels        │
│    - Data via USBHSIC/NCM interface         │
└─────────────────────────────────────────────┘
```

#### 2A: HSIC USB Driver Port (~300-500 LOC new code)
The mainline `ehci-pci` will bind to PCI `00:10.0` (8086:119D) but lacks HSIC PHY init.
Need an out-of-tree companion module:
- Use mainline `pinctrl-merrifield` for GPIO mux (replaces `lnw_gpio_set_alt()`)
- Use mainline `gpiod_*` API (replaces `get_gpio_by_name()`)
- Perform HSIC PHY power sequence via PORTSC register writes
- Implement `hsic_enable` sysfs for mmgr to control link
- Replace `wake_lock` with `wakeup_source_*`

#### 2B: Modem Control Driver Port (~400 LOC)
Port `modules/drivers/modem_control/` (5 files, ~890 LOC total):
- Replace SFI platform data with hardcoded: PMIC register 0x31, on=0x2, off=0x0, mask=0xFC
- Use `intel_scu_ipc_dev_iowrite8()` for PMIC power on/off (mainline API)
- Use `gpiod_*` for RST_OUT, PWR_ON, RST_BBN, CDUMP GPIOs
- Keep char device `/dev/mdm_ctrl` interface (mmgr uses ioctl to control modem)

#### 2C: Firmware Loading (mmgr)
The `mmgr` binary is proprietary (32-bit x86 ELF). Two approaches:

**Option A — Run mmgr binary natively** (fastest):
- It's x86 32-bit, runs on x86_64 Linux with 32-bit libc (multilib)
- Needs: `/dev/mdm_ctrl` (from 2B), `/dev/ttyACM0`, HSIC sysfs (from 2A)
- Needs: modem firmware at `/config/telephony/` (extract from modem.zip)
- Needs: `libmmgr_cnx.so`, `libmmgr_utils.so` (extracted)

**Option B — Reverse-engineer firmware upload** (for fully open stack):
- PSI injection → EBL → FLS protocol over USB endpoints
- Rapid-RIL source on GitHub has modem initialization sequences
- xmm7360-pci project shows similar (but different interface) protocol
- **HIGH EFFORT** but would make fully open-source LTE stack

#### 2D: Telephony Stack

**For Android 15**:
- Run `rild` + `librapid-ril-core.so` in 32-bit compat mode
- Write AIDL IRadio shim → legacy RIL socket adapter
- OR use Android 15's legacy RIL compatibility path

**For UBports**:
- oFono with `ifxmodem` plugin directly on gsmtty channels
- oFono's ifxmodem understands Infineon/IMC AT extensions natively
- Data via CDC-NCM USB class (`cdc_ncm` mainline driver)
- No Android RIL needed — pure AT command telephony

#### Modem Files Inventory
| File | Location | Purpose |
|------|----------|---------|
| mmgr | vendor-blobs/bin/ | Modem manager (firmware loader) |
| rild | vendor-blobs/bin/ | Android RIL daemon |
| nvm_server | vendor-blobs/bin/ | NVM calibration server |
| librapid-ril-core.so | vendor-blobs/lib/ | Rapid-RIL AT engine |
| librapid-ril-util.so | vendor-blobs/lib/ | Rapid-RIL utilities |
| libmamgr-xmm.so | vendor-blobs/lib/ | Modem asset manager |
| libmmgr_cnx.so | vendor-blobs/lib/ | mmgr connection lib |
| libmmgr_utils.so | vendor-blobs/lib/ | mmgr utilities |
| libmmgrcli.so | vendor-blobs/lib/ | mmgr client lib |
| libia_nvm.so | vendor-blobs/lib/ | NVM access lib |
| libril.so | vendor-blobs/lib/ | Android RIL framework |
| librilutils.so | vendor-blobs/lib/ | RIL utilities |
| modem.zip | vendor-blobs/etc/firmware/modem/ | XMM 7160 firmware (95MB) |
| modem_nvm.zip | vendor-blobs/etc/firmware/modem/ | NVM calibration data |
| mmgr_7160_conf_1.xml | vendor-blobs/etc/telephony/ | mmgr config (USB PIDs, paths, MUX) |
| repository7160.txt | vendor-blobs/etc/rril/ | Rapid-RIL config (AT timeouts, channels) |

#### Modem USB Identities
| State | VID | PID | Interface |
|-------|-----|-----|-----------|
| Flash mode | 0x8087 | 0x0716 | USB bulk (firmware upload) |
| Baseband | 0x1519 | 0x0452 | CDC-ACM + CDC-NCM |
| Coredump | 0x1519 | 0xF000 | USB bulk (ymodem crash dump) |

#### n_gsm MUX Channel Map
| Channel | gsmtty# | Purpose |
|---------|---------|---------|
| Main RIL | gsmtty12 | Primary AT commands |
| AT proxy | gsmtty1 | AT command proxy |
| Network | gsmtty2 | Network status |
| Data | gsmtty3,4,15-17 | Data bearers |
| MUX | gsmtty6 | Multiplexer control |
| Control | gsmtty8 | Modem control |
| OEM | gsmtty9 | OEM commands |
| AT target | gsmtty10 | AT proxy target |
| Audio | gsmtty13 | Audio routing |

---

### Phase 3: AOSP 15 Device Tree

Create `device/dell/venue8/`:
- `BoardConfig.mk` — x86_64, kernel 6.6 path, partition sizes, OSIP boot format
- `device.mk` — vendor blob includes, HAL overrides, modem config
- `AndroidProducts.mk` + `dell_venue8.mk` — product definition
- `vendorsetup.sh` — lunch target
- Custom `mkbootimg` for OSIP ($OS$) boot image format
- SELinux policy for Merrifield hardware
- `init.saltbay.rc` — hardware init (insmod, modem start, WiFi)
- `fstab.saltbay` — partition mount table

### Phase 4: Vendor Blobs Package

Create `vendor/dell/venue8/`:
- All proprietary blobs with `Android.bp` / `Android.mk`
- EGL config for PowerVR Rogue
- Audio policy for WM8958
- WiFi/BT config for BCM4335
- Modem firmware + configs
- 32-bit multilib support

### Phase 5: Build & Flash
- `repo init` AOSP android-15.0.0_r1
- Add device + vendor trees
- `lunch dell_venue8-userdebug && make -j$(nproc)`
- Flash via fastboot (boot, system, recovery) or DnX

### Phase 6: UBports / Halium Port
- Create Halium device config from Android device tree
- Build Halium system image
- Configure oFono with ifxmodem for telephony
- Run mmgr via compatibility shim for firmware loading
- Submit device port to UBports community

---

## Partition Layout
| Label | Start Block | Size (blocks) | Size |
|-------|------------|---------------|------|
| reserved | 34 | 524288 | 256MB |
| panic | 524322 | 65536 | 32MB |
| factory | 589858 | 262144 | 128MB |
| misc | 852002 | 262144 | 128MB |
| config | 1114146 | 262144 | 128MB |
| cache | 1376290 | 3145728 | 1.5GB |
| logs | 4522018 | 49152 | 24MB |
| system | 4571170 | 3145728 | 1.5GB |
| data | 7716898 | remaining | ~11GB |

## Effort Estimate
| Component | Effort | Status |
|-----------|--------|--------|
| Platform bringup (SFI replacement) | HIGH | **DONE** — merrifield-venue8.c (900+ LOC) |
| GPU (PowerVR Rogue) | HIGH | Deferred — pvr driver in kernel 6.8+, simpledrm for now |
| Display (tngdisp → DRM/KMS) | MEDIUM | **DONE** — venue8_display.c DRM driver + backlight |
| WiFi (BCM4335) | LOW | **DONE** — mainline brcmfmac + SDIO power regulator |
| Bluetooth | LOW | **DONE** — mainline btbcm + platform device + GPIOs |
| Camera (atomisp) | MEDIUM | **DONE** — venue8_camera.c platform glue + staging OV5693/OV2722 |
| Audio (WM8958 + SST) | MEDIUM | **DONE** — mainline mrfld_wm8958.c + regulators + platform device |
| Touch (Synaptics + ELAN) | LOW | **DONE** — mainline rmi4 + elan + I2C board info |
| Sensors (LSM303D, L3GD20) | LOW | **DONE** — mainline ST IIO + I2C board info + GPIO IRQs |
| Power (BQ24261 + BQ27441) | LOW | **DONE** — mainline bq27xxx + bq24257 + I2C board info |
| HSIC USB for modem | MEDIUM | **DONE** — ehci_hsic.c (~315 LOC) |
| Modem control driver | MEDIUM | **DONE** — modem_control.c (~663 LOC) |
| mmgr firmware loading | LOW* | Run binary natively |
| Telephony (Android RIL) | MEDIUM | Shim needed |
| Telephony (oFono/UBports) | LOW | ifxmodem plugin |
| AOSP device tree | MEDIUM | Not started |
| OSIP boot image tooling | MEDIUM | **DONE** — make_bootimg.py + flash_kernel.sh |

\* LOW if running proprietary binary; HIGH if reverse-engineering protocol

## Key Resources
- Rapid-RIL source (XMM7160): `github.com/HazouPH/android_device_motorola_smi/tree/LOS-14.1/modules/ril/rapid_ril`
- oFono ifxmodem: `git.kernel.org/pub/scm/network/ofono/ofono.git` → `drivers/ifxmodem/`
- Intel SCU IPC mainline: `drivers/platform/x86/intel/scu/ipc.c`
- Merrifield PMIC mainline: `drivers/mfd/intel_soc_pmic_mrfld.c`
- Merrifield pinctrl mainline: `drivers/pinctrl/intel/pinctrl-merrifield.c`
- atomisp staging: `drivers/staging/media/atomisp/`
- Intel Edison wiki (same SoC): `edison-fw.github.io/edison-wiki/`

## Legal Status
All driver source code is open source (GPLv2 or Dual MIT/GPLv2). Fully redistributable.
Proprietary blobs (GPU userspace, mmgr, rild) are device-extracted — standard practice
for AOSP device ports (same as LineageOS, /e/, CalyxOS for any device).
UBports contribution: kernel drivers are clean GPL, no legal issues.
