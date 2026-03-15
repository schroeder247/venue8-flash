# Dell Venue 8 3840 — Open Source Platform Port

## Goal
Port Dell Venue 8 3840 (Intel Merrifield/Saltbay) to:
1. **AOSP 15** (Android 15) with kernel 6.6
2. **Ubuntu Touch** (UBports) contribution

## Legal Status
All driver source code from Dell's open source release is redistributable:
- PowerVR Rogue GPU: Dual MIT/GPLv2 (Imagination Technologies)
- Intel Display (tngdisp): MIT (Intel)
- Camera ISP (atomisp): GPLv2 (Intel/Silicon Hive)
- WiFi (BCM4335): GPL (Broadcom, mainline brcmfmac)
- Kernel + SFI/MID platform: GPLv2

Source: https://opensource.dell.com/releases/Venue_8_3840_Merrifield/developer-edition/

## Project Structure
```
project/
├── kernel-port/     — kernel 6.6 with ported drivers
│   ├── platform/    — SFI + Intel MID platform (from 3.10)
│   ├── gpu/         — PowerVR Rogue DRM driver (from 3.10 intel_media)
│   ├── display/     — Tangier display/DSI (from 3.10 intel_media)
│   └── patches/     — patch series against android15-6.6
├── aosp/            — AOSP 15 device tree + vendor
│   ├── device/      — device/dell/venue8
│   └── vendor/      — vendor/dell/venue8 (firmware + HAL configs)
└── ubports/         — Halium/UBports port
    ├── halium/      — Halium device config
    └── docs/        — UBports porting documentation
```

## Hardware
- SoC: Intel Atom Z3480 (Merrifield/Silvermont), 2C/2T, 2.13GHz
- GPU: PowerVR G6400 (Rogue), OpenGL ES 3.0
- RAM: 1GB
- Storage: 16GB eMMC
- Display: 1200x1920 MIPI DSI
- WiFi/BT: Broadcom BCM4335
- Modem: Intel XMM 7160 LTE
- Touch: Synaptics DSX
- Audio: Wolfson WM8958 + Intel SST
- Sensors: LSM303D, L3GD20, AL1721
- Camera: Intel Atom ISP (atomisp2400)

## Driver Source Inventory
| Module | Source Files | Lines (est.) | Port Difficulty |
|--------|-------------|-------------|-----------------|
| PowerVR Rogue GPU | 346 .c, 491 .h | ~80K | Hard |
| Tangier Display | 88 .c | ~20K | Medium |
| Camera ISP | 321 .c | ~60K | Easy (in mainline staging) |
| WiFi BCM4335 | 145 .c | ~30K | None (mainline brcmfmac) |
| SFI/Intel MID | ~40 .c | ~8K | Medium |
| Audio/SST | in-kernel | N/A | Easy (mainline) |
| Sensors | mainline | N/A | None |
